// LD_PRELOAD shim (guest glibc, aarch64) for running elogind rootless on Android:
// Android SELinux denies socket(AF_NETLINK, SOCK_RAW, NETLINK_KOBJECT_UEVENT) with EACCES.
// On failure, substitute an idle AF_UNIX SOCK_DGRAM fd (pollable via epoll, never readable)
// and stub bind/setsockopt/getsockname on that fd so sd-device-monitor initializes cleanly.
// Header-free so it builds with NDK clang (--target=aarch64-linux-gnu -nostdlib).
typedef unsigned int socklen_t;

struct sockaddr_nl {
  unsigned short nl_family;
  unsigned short nl_pad;
  unsigned int nl_pid;
  unsigned int nl_groups;
};

#define AF_UNIX 1
#define AF_NETLINK 16
#define SOCK_DGRAM 2
#define SOCK_CLOEXEC 02000000
#define SOCK_NONBLOCK 00004000
#define NETLINK_KOBJECT_UEVENT 15
#define RTLD_NEXT ((void *)-1l)
#define MAX_FD 1024

extern void *dlsym(void *, const char *);
extern int getpid(void);

static unsigned char fake_nl[MAX_FD];

static int is_fake(int fd) {
  return fd >= 0 && fd < MAX_FD && __atomic_load_n(&fake_nl[fd], __ATOMIC_RELAXED);
}

int socket(int domain, int type, int protocol) {
  static int (*real_socket)(int, int, int);
  if (!real_socket) real_socket = dlsym(RTLD_NEXT, "socket");
  int fd = real_socket(domain, type, protocol);
  if (fd < 0 && domain == AF_NETLINK && protocol == NETLINK_KOBJECT_UEVENT) {
    int flags = type & (SOCK_CLOEXEC | SOCK_NONBLOCK);
    int dfd = real_socket(AF_UNIX, SOCK_DGRAM | flags, 0);
    if (dfd >= 0 && dfd < MAX_FD) {
      __atomic_store_n(&fake_nl[dfd], 1, __ATOMIC_RELAXED);
    }
    return dfd;
  }
  return fd;
}

int bind(int fd, const void *addr, socklen_t len) {
  static int (*real_bind)(int, const void *, socklen_t);
  if (!real_bind) real_bind = dlsym(RTLD_NEXT, "bind");
  if (is_fake(fd)) return 0;
  return real_bind(fd, addr, len);
}

int setsockopt(int fd, int level, int optname, const void *optval, socklen_t optlen) {
  static int (*real_setsockopt)(int, int, int, const void *, socklen_t);
  if (!real_setsockopt) real_setsockopt = dlsym(RTLD_NEXT, "setsockopt");
  if (is_fake(fd)) return 0;
  return real_setsockopt(fd, level, optname, optval, optlen);
}

int getsockname(int fd, void *addr, socklen_t *addrlen) {
  static int (*real_getsockname)(int, void *, socklen_t *);
  if (!real_getsockname) real_getsockname = dlsym(RTLD_NEXT, "getsockname");
  if (is_fake(fd)) {
    if (addr && addrlen && *addrlen >= sizeof(struct sockaddr_nl)) {
      struct sockaddr_nl *nl = addr;
      nl->nl_family = AF_NETLINK;
      nl->nl_pad = 0;
      nl->nl_pid = (unsigned int)getpid();
      nl->nl_groups = 0;
      *addrlen = sizeof(struct sockaddr_nl);
    }
    return 0;
  }
  return real_getsockname(fd, addr, addrlen);
}

int close(int fd) {
  static int (*real_close)(int);
  if (!real_close) real_close = dlsym(RTLD_NEXT, "close");
  if (fd >= 0 && fd < MAX_FD) {
    __atomic_store_n(&fake_nl[fd], 0, __ATOMIC_RELAXED);
  }
  return real_close(fd);
}
