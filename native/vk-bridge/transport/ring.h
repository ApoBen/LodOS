/* vk-bridge transport: SPSC variable-length message ring in shared memory (memfd).
 * Layout: [ring_hdr (4 KiB)] [data: cap bytes, cap power of two].
 * Msg: u32 len at 8-aligned pos, payload, padded to 8. len==RING_WRAP => skip to start.
 * Wakeup: futex on shared 32-bit seq words (spin first, then FUTEX_WAIT). */
#ifndef VKB_RING_H
#define VKB_RING_H
#include <stddef.h>
#include <stdint.h>

#define RING_DEFAULT_CAP (1u << 20)
#define RING_HDR_SIZE 4096u

typedef struct ring_hdr ring_hdr;
typedef struct {
    ring_hdr *h;
    uint8_t *data;
    uint32_t cap;      /* data bytes, power of two */
    size_t map_len;
    int fd;
} ring;

/* create memfd-backed ring (cap=0 -> default 1 MiB); returns 0 or -errno */
int ring_create(ring *r, uint32_t cap);
/* map an existing ring fd (e.g. received via SCM_RIGHTS); takes ownership of fd */
int ring_attach(ring *r, int fd);
void ring_destroy(ring *r);
/* max payload a single message may have */
uint32_t ring_max_msg(const ring *r);
/* blocking push; returns 0 or -EMSGSIZE */
int ring_push(ring *r, const void *msg, uint32_t len);
/* blocking pop into buf; returns payload len, or -EMSGSIZE if buf too small (msg kept) */
int64_t ring_pop(ring *r, void *buf, uint32_t buflen);
/* non-blocking pop; returns -EAGAIN when empty */
int64_t ring_trypop(ring *r, void *buf, uint32_t buflen);
#endif
