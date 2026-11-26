/* lodos-vk-bridge RPC wire runtime (hand-written; used by rpcgen.py output).
 * Writer: p==NULL -> sizing pass. Reader: every get is bounds-checked; first error sticks in r->err,
 * failed gets yield zeroes. Handles cross the wire as u64 ids via vkr_h_out/vkr_h_in, implemented
 * per side (client: identity, server: handle table). */
#ifndef VKB_WIRE_H
#define VKB_WIRE_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>

enum { VKR_OK = 0, VKR_E_BOUNDS, VKR_E_NOMEM, VKR_E_PNEXT, VKR_E_HANDLE, VKR_E_CMD, VKR_E_COUNT };

#define VKR_MAX_ARR 65536u     /* max elements of an out array the server will allocate */
#define VKR_MAX_PNEXT 256u     /* max nodes in an out pNext chain */
#define VKR_CMD_BYE 0xffffffffu /* client -> server: close connection */

typedef struct { uint8_t *base; size_t cap, used; } vkr_arena;
typedef struct { uint8_t *p; size_t cap, off; int err; void *hctx; } vkr_wr;
typedef struct { const uint8_t *p; size_t len, off; int err; vkr_arena *a; void *hctx; } vkr_rd;

/* per-side handle hooks (defined by client lib / server) */
uint64_t vkr_h_out(void *hctx, VkObjectType t, uint64_t h);
uint64_t vkr_h_in(void *hctx, VkObjectType t, uint64_t id, int *ok);
void vkr_h_drop(void *hctx, VkObjectType t, uint64_t h);

static inline void vkr_fail(int *err, int e) { if (!*err) *err = e; }

static inline void vkr_put(vkr_wr *w, const void *s, size_t n) {
    if (w->p) {
        if (n > w->cap || w->off > w->cap - n) vkr_fail(&w->err, VKR_E_BOUNDS);
        else memcpy(w->p + w->off, s, n);
    }
    w->off += n;
}
static inline void vkr_put_u8(vkr_wr *w, uint8_t v) { vkr_put(w, &v, 1); }
static inline void vkr_put_u32(vkr_wr *w, uint32_t v) { vkr_put(w, &v, 4); }
static inline void vkr_put_u64(vkr_wr *w, uint64_t v) { vkr_put(w, &v, 8); }
static inline void vkr_put_str(vkr_wr *w, const char *s) {
    vkr_put_u8(w, s != NULL);
    if (s) { uint32_t l = (uint32_t)strlen(s) + 1; vkr_put_u32(w, l); vkr_put(w, s, l); }
}

static inline int vkr_get(vkr_rd *r, void *d, size_t n) {
    if (r->err || n > r->len - r->off) { vkr_fail(&r->err, VKR_E_BOUNDS); memset(d, 0, n); return 0; }
    memcpy(d, r->p + r->off, n);
    r->off += n;
    return 1;
}
static inline uint8_t vkr_get_u8(vkr_rd *r) { uint8_t v; vkr_get(r, &v, 1); return v; }
static inline uint32_t vkr_get_u32(vkr_rd *r) { uint32_t v; vkr_get(r, &v, 4); return v; }
static inline uint64_t vkr_get_u64(vkr_rd *r) { uint64_t v; vkr_get(r, &v, 8); return v; }
static inline size_t vkr_left(const vkr_rd *r) { return r->err ? 0 : r->len - r->off; }

/* zeroed arena alloc; NULL + r->err on failure (never aborts) */
static inline void *vkr_alloc(vkr_rd *r, size_t n) {
    vkr_arena *a = r->a;
    if (r->err) return NULL;
    if (!a) { vkr_fail(&r->err, VKR_E_NOMEM); return NULL; }
    if (n == 0) n = 1;
    size_t off = (a->used + 15) & ~(size_t)15;
    if (off > a->cap || n > a->cap - off) { vkr_fail(&r->err, VKR_E_NOMEM); return NULL; }
    a->used = off + n;
    memset(a->base + off, 0, n);
    return a->base + off;
}
/* array alloc with count sanity: each wire element needs >= min_wire bytes */
static inline void *vkr_alloc_arr(vkr_rd *r, uint32_t c, size_t elem, size_t min_wire) {
    if (min_wire && (uint64_t)c * min_wire > vkr_left(r)) { vkr_fail(&r->err, VKR_E_BOUNDS); return NULL; }
    if (elem && c > SIZE_MAX / elem) { vkr_fail(&r->err, VKR_E_NOMEM); return NULL; }
    return vkr_alloc(r, (size_t)c * elem);
}
static inline const char *vkr_get_str(vkr_rd *r) {
    if (!vkr_get_u8(r)) return NULL;
    uint32_t l = vkr_get_u32(r);
    if (l == 0 || l > vkr_left(r)) { vkr_fail(&r->err, VKR_E_BOUNDS); return NULL; }
    char *s = vkr_alloc(r, l);
    if (!s) return NULL;
    vkr_get(r, s, l);
    s[l - 1] = 0;
    return s;
}
#endif
