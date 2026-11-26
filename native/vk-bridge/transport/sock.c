#define _GNU_SOURCE
#include "sock.h"
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int sock_pair(int sv[2]) {
    return socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv) < 0 ? -errno : 0;
}

ssize_t sock_send(int s, const void *buf, size_t len, const int *fds, int nfds) {
    if (nfds < 0 || nfds > SOCK_MAX_FDS) return -EINVAL;
    char dummy = 0;
    struct iovec iov = { .iov_base = len ? (void *)buf : &dummy, .iov_len = len ? len : 1 };
    union { char b[CMSG_SPACE(sizeof(int) * SOCK_MAX_FDS)]; struct cmsghdr align; } u;
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1 };
    if (nfds) {
        memset(&u, 0, sizeof(u));
        m.msg_control = u.b;
        m.msg_controllen = CMSG_SPACE(sizeof(int) * nfds);
        struct cmsghdr *c = CMSG_FIRSTHDR(&m);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * nfds);
        memcpy(CMSG_DATA(c), fds, sizeof(int) * nfds);
    }
    ssize_t n;
    do n = sendmsg(s, &m, MSG_NOSIGNAL); while (n < 0 && errno == EINTR);
    return n < 0 ? -errno : n;
}

ssize_t sock_recv(int s, void *buf, size_t cap, int *fds, int *nfds) {
    struct iovec iov = { .iov_base = buf, .iov_len = cap };
    union { char b[CMSG_SPACE(sizeof(int) * SOCK_MAX_FDS)]; struct cmsghdr align; } u;
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1,
                        .msg_control = u.b, .msg_controllen = sizeof(u.b) };
    ssize_t n;
    do n = recvmsg(s, &m, MSG_CMSG_CLOEXEC); while (n < 0 && errno == EINTR);
    if (nfds) *nfds = 0;
    if (n < 0) return -errno;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        int k = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        int got[SOCK_MAX_FDS];
        memcpy(got, CMSG_DATA(c), sizeof(int) * (k > SOCK_MAX_FDS ? SOCK_MAX_FDS : k));
        for (int i = 0; i < k && i < SOCK_MAX_FDS; i++) {
            if (fds && nfds && *nfds < SOCK_MAX_FDS) fds[(*nfds)++] = got[i];
            else close(got[i]);
        }
    }
    if (m.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) return -EMSGSIZE;
    return n;
}
