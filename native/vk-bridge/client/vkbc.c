/* libvkbridge_client: connect to vk-bridge server, pass 2 memfd rings (c2s, s2c), lockstep RPC. */
#define _GNU_SOURCE
#include "vkbc.h"
#include "../transport/ring.h"
#include "../transport/sock.h"
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct vkbc {
    int sock;
    ring c2s, s2c;
    uint8_t *req, *rep;
    uint32_t req_cap, rep_cap;
};

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
    if (!name) name = "lodos-vk";
    vkbc *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->sock = -1; c->c2s.fd = c->s2c.fd = -1;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    size_t nl = strlen(name);
    if (nl + 1 > sizeof a.sun_path) { errno = ENAMETOOLONG; goto fail; }
    memcpy(a.sun_path + 1, name, nl); /* abstract: leading NUL */
    c->sock = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (c->sock < 0) goto fail;
    if (connect(c->sock, (struct sockaddr *)&a, (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + nl)) < 0)
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
