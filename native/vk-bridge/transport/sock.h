/* vk-bridge transport: control socket msgs with up to SOCK_MAX_FDS fds (SCM_RIGHTS). */
#ifndef VKB_SOCK_H
#define VKB_SOCK_H
#include <stddef.h>
#include <sys/types.h>

#define SOCK_MAX_FDS 4

/* SOCK_SEQPACKET socketpair (message boundaries preserved) */
int sock_pair(int sv[2]);
/* send one message + nfds fds (nfds<=4); returns bytes sent or -errno */
ssize_t sock_send(int s, const void *buf, size_t len, const int *fds, int nfds);
/* recv one message; fds[] gets up to 4 fds, *nfds set; returns bytes or -errno (0 = EOF) */
ssize_t sock_recv(int s, void *buf, size_t cap, int *fds, int *nfds);
#endif
