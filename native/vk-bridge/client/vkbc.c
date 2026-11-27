/* libvkbridge_client: connect to vk-bridge server, pass 2 memfd rings (c2s, s2c), lockstep RPC. */
#define _GNU_SOURCE
#include "vkbc.h"
#include "../transport/ring.h"
#include "../transport/sock.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

typedef struct {
    uint64_t dev, mem;
    void *ptr;
    size_t map_len;
} vkbc_map;

struct vkbc {
    int sock;
    ring c2s, s2c;
    uint8_t *req, *rep;
    uint32_t req_cap, rep_cap;
    vkbc_map *maps;
    uint32_t nmaps, capmaps;
};

void vkbc__drop_map(vkbc *c, uint64_t dev, uint64_t mem) {
    if (!c) return;
    for (uint32_t i = 0; i < c->nmaps; i++) {
        if (c->maps[i].ptr && c->maps[i].dev == dev && (!mem || c->maps[i].mem == mem)) {
            munmap(c->maps[i].ptr, c->maps[i].map_len);
            c->maps[i] = (vkbc_map){ 0 };
        }
    }
}

#ifdef VKB_CMD_HAS_vkMapMemory
VkResult vkbc_vkMapMemory(vkbc *c, VkDevice device, VkDeviceMemory memory,
                          VkDeviceSize offset, VkDeviceSize size,
                          VkMemoryMapFlags flags, void **ppData) {
    if (!c || !ppData) return VK_ERROR_MEMORY_MAP_FAILED;
    size_t bcap; uint8_t *buf = vkbc__buf(c, &bcap);
    vkr_wr w = { buf, bcap, 0, 0, NULL };
    vkr_put_u32(&w, VKB_CMD_vkMapMemory);
    vkr_put_u64(&w, (uint64_t)(uintptr_t)device);
    vkr_put_u64(&w, (uint64_t)(uintptr_t)memory);
    vkr_put(&w, &offset, sizeof offset);
    vkr_put(&w, &size, sizeof size);
    vkr_put(&w, &flags, sizeof flags);
    if (w.err) return VK_ERROR_OUT_OF_HOST_MEMORY;
    const uint8_t *rep; size_t replen;
    if (vkbc__call(c, w.off, &rep, &replen) < 0) return VK_ERROR_DEVICE_LOST;
    vkr_rd r = { rep, replen, 0, 0, NULL, NULL };
    if (vkr_get_u32(&r) != VKB_CMD_vkMapMemory) return VK_ERROR_UNKNOWN;
    VkResult res; vkr_get(&r, &res, sizeof res);
    if (r.err) return VK_ERROR_UNKNOWN;
    if (res < 0) return res;
    uint64_t actual_sz = vkr_get_u64(&r);
    if (r.err) return VK_ERROR_UNKNOWN;
    char tag = 0; int fds[SOCK_MAX_FDS], nf = 0;
    ssize_t sn = sock_recv(c->sock, &tag, 1, fds, &nf);
    if (sn != 1 || tag != 'M' || nf != 1) {
        for (int i = 0; i < nf; i++) close(fds[i]);
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    size_t mlen = actual_sz ? (size_t)actual_sz : 1;
    void *p = mmap(NULL, mlen, PROT_READ | PROT_WRITE, MAP_SHARED, fds[0], 0);
    close(fds[0]);
    if (p == MAP_FAILED) {
#ifdef VKB_HAS_vkUnmapMemory
        (void)vkbc_vkUnmapMemory(c, device, memory);
#endif
        return VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    uint32_t slot = UINT32_MAX;
    for (uint32_t i = 0; i < c->nmaps; i++)
        if (!c->maps[i].ptr) { slot = i; break; }
    if (slot == UINT32_MAX) {
        uint32_t nc = c->capmaps ? c->capmaps * 2 : 16;
        vkbc_map *nm = realloc(c->maps, nc * sizeof *nm);
        if (!nm) {
            munmap(p, mlen);
#ifdef VKB_HAS_vkUnmapMemory
            (void)vkbc_vkUnmapMemory(c, device, memory);
#endif
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        c->maps = nm; c->capmaps = nc;
        slot = c->nmaps++;
    }
    c->maps[slot] = (vkbc_map){ (uint64_t)(uintptr_t)device, (uint64_t)(uintptr_t)memory, p, mlen };
    *ppData = p;
    return VK_SUCCESS;
}
#endif

uint8_t *vkbc__buf(vkbc *c, size_t *cap) { *cap = c->req_cap; return c->req; }

int vkbc__call(vkbc *c, size_t len, const uint8_t **rep, size_t *replen) {
    if (len > c->req_cap) return -EMSGSIZE;
    int rc = ring_push(&c->c2s, c->req, (uint32_t)len);
    if (rc < 0) return rc;
    int64_t n = ring_pop(&c->s2c, c->rep, c->rep_cap);
    if (n < 0) return (int)n;
    *rep = c->rep;
    *replen = (size_t)n;
    return 0;
}

vkbc *vkbc_connect(const char *name) {
    if (!name || !*name) name = getenv("LODOS_VK_SOCK");
    if (!name || !*name) name = "lodos-vk";
    vkbc *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->sock = -1; c->c2s.fd = c->s2c.fd = -1;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    socklen_t alen;
    if (name[0] == '/') {
        size_t nl = strlen(name);
        if (nl + 1 > sizeof a.sun_path) { errno = ENAMETOOLONG; goto fail; }
        memcpy(a.sun_path, name, nl + 1);
        alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + nl + 1);
    } else {
        if (name[0] == '@') name++;
        size_t nl = strlen(name);
        if (nl + 1 > sizeof a.sun_path) { errno = ENAMETOOLONG; goto fail; }
        memcpy(a.sun_path + 1, name, nl); /* abstract: leading NUL */
        alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + nl);
    }
    c->sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (c->sock < 0) goto fail;
    if (connect(c->sock, (struct sockaddr *)&a, alen) < 0)
        goto fail;
    int rc;
    if ((rc = ring_create(&c->c2s, 0)) < 0 || (rc = ring_create(&c->s2c, 0)) < 0) { errno = -rc; goto fail; }
    int fds[2] = { c->c2s.fd, c->s2c.fd };
    if ((rc = (int)sock_send(c->sock, "VKB1", 4, fds, 2)) < 0) { errno = -rc; goto fail; }
    char ok[4]; int nf;
    ssize_t n = sock_recv(c->sock, ok, sizeof ok, NULL, &nf);
    if (n != 2 || memcmp(ok, "ok", 2)) { errno = n < 0 ? (int)-n : EPROTO; goto fail; }
    c->req_cap = ring_max_msg(&c->c2s);
    c->rep_cap = ring_max_msg(&c->s2c);
    c->req = malloc(c->req_cap);
    c->rep = malloc(c->rep_cap);
    if (!c->req || !c->rep) { errno = ENOMEM; goto fail; }
    return c;
fail:;
    int e = errno;
    vkbc_disconnect(c);
    errno = e;
    return NULL;
}

void vkbc_disconnect(vkbc *c) {
    if (!c) return;
    for (uint32_t i = 0; i < c->nmaps; i++)
        if (c->maps[i].ptr) munmap(c->maps[i].ptr, c->maps[i].map_len);
    free(c->maps);
    if (c->c2s.h && c->req) {
        uint32_t bye = VKR_CMD_BYE;
        ring_push(&c->c2s, &bye, 4);
    }
    if (c->sock >= 0) close(c->sock);
    if (c->c2s.h || c->c2s.fd >= 0) ring_destroy(&c->c2s);
    if (c->s2c.h || c->s2c.fd >= 0) ring_destroy(&c->s2c);
    free(c->req); free(c->rep);
    free(c);
}
