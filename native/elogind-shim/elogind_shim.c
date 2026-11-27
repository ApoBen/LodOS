// LD_PRELOAD shim (guest glibc, aarch64) for running elogind rootless on Android:
// 1. Android SELinux denies socket(AF_NETLINK, SOCK_RAW, NETLINK_KOBJECT_UEVENT) with EACCES.
//    On failure, substitute an idle AF_UNIX SOCK_DGRAM fd (pollable via epoll, never readable)
//    and stub bind/setsockopt/getsockname on that fd so sd-device-monitor initializes cleanly.
// 2. Fake writable cgroup2 support over a regular directory bound to /sys/fs/cgroup:
//    - statfs/statfs64/fstatfs/fstatfs64 report CGROUP2_SUPER_MAGIC for /sys/fs/cgroup paths.
//    - mkdir/mkdirat under /sys/fs/cgroup populate minimal cgroup2 control files.
//    - rmdir/unlinkat(AT_REMOVEDIR) under /sys/fs/cgroup remove dummy cgroup.* files first.
// 3. mount/umount2 on /run/user/* succeed as no-ops when unprivileged EPERM/EACCES occurs.
// Header-free so it builds with NDK clang (--target=aarch64-linux-gnu -nostdlib).
typedef unsigned int socklen_t;
typedef unsigned int mode_t;
typedef unsigned long size_t;
typedef long ssize_t;

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

#define CGROUP2_SUPER_MAGIC 0x63677270L
#define AT_FDCWD (-100)
#define AT_REMOVEDIR 0x200
#define O_WRONLY 1
#define O_CREAT 0100
#define O_EXCL 0200
#define O_CLOEXEC 02000000
#define EPERM 1
#define EACCES 13
#define EEXIST 17
#define EINVAL 22
#define ENOTEMPTY 39

extern void *dlsym(void *, const char *);
extern int getpid(void);
extern int *__errno_location(void);
extern ssize_t readlink(const char *, char *, size_t);
extern char *getcwd(char *, size_t);
extern int openat(int, const char *, int, ...);
extern ssize_t write(int, const void *, size_t);

static unsigned char fake_nl[MAX_FD];

static int is_fake(int fd) {
  return fd >= 0 && fd < MAX_FD && __atomic_load_n(&fake_nl[fd], __ATOMIC_RELAXED);
}

static size_t s_len(const char *s) {
  size_t n = 0;
  while (s && s[n]) n++;
  return n;
}

static int s_starts(const char *s, const char *pre) {
  if (!s || !pre) return 0;
  while (*pre) {
    if (*s++ != *pre++) return 0;
  }
  return 1;
}

static int is_cgroup_path(const char *p) {
  if (!s_starts(p, "/sys/fs/cgroup")) return 0;
  return p[14] == '\0' || p[14] == '/';
}

static int is_run_user_path(const char *p) {
  return s_starts(p, "/run/user/");
}

static int fd_to_path(int fd, char *out, size_t outsz) {
  char linkp[64];
  const char *pre = "/proc/self/fd/";
  size_t i = 0;
  while (pre[i]) { linkp[i] = pre[i]; i++; }
  if (fd < 0) return -1;
  char digits[16];
  int d = 0;
  unsigned int v = (unsigned int)fd;
  if (v == 0) {
    digits[d++] = '0';
  } else {
    while (v > 0 && d < 15) { digits[d++] = (char)('0' + (v % 10)); v /= 10; }
  }
  while (d > 0) linkp[i++] = digits[--d];
  linkp[i] = '\0';
  ssize_t n = readlink(linkp, out, outsz - 1);
  if (n < 0) return -1;
  out[n] = '\0';
  return (int)n;
}

static int resolve_at(int dirfd, const char *path, char *out, size_t outsz) {
  if (!path || !path[0]) return 0;
  if (path[0] == '/') {
    size_t n = s_len(path);
    if (n >= outsz) return 0;
    for (size_t i = 0; i <= n; i++) out[i] = path[i];
    return 1;
  }
  char base[512];
  if (dirfd == AT_FDCWD) {
    if (!getcwd(base, sizeof(base))) return 0;
  } else {
    if (fd_to_path(dirfd, base, sizeof(base)) < 0) return 0;
  }
  size_t bl = s_len(base), pl = s_len(path);
  if (bl + 1 + pl >= outsz) return 0;
  size_t pos = 0;
  for (size_t i = 0; i < bl; i++) out[pos++] = base[i];
  if (pos == 0 || out[pos - 1] != '/') out[pos++] = '/';
  for (size_t i = 0; i <= pl; i++) out[pos++] = path[i];
  return 1;
}

static void create_cg_file(const char *dir, const char *name, const char *content) {
  char p[640];
  size_t dl = s_len(dir), nl = s_len(name);
  if (dl + 1 + nl >= sizeof(p)) return;
  size_t pos = 0;
  for (size_t i = 0; i < dl; i++) p[pos++] = dir[i];
  if (pos == 0 || p[pos - 1] != '/') p[pos++] = '/';
  for (size_t i = 0; i <= nl; i++) p[pos++] = name[i];
  int fd = openat(AT_FDCWD, p, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
  if (fd >= 0) {
    if (content && content[0]) {
      (void)write(fd, content, s_len(content));
    }
    static int (*real_close)(int);
    if (!real_close) real_close = dlsym(RTLD_NEXT, "close");
    real_close(fd);
  }
}

static void populate_cgroup_dir(const char *dir) {
  int saved = *__errno_location();
  create_cg_file(dir, "cgroup.procs", "");
  create_cg_file(dir, "cgroup.threads", "");
  create_cg_file(dir, "cgroup.controllers", "");
  create_cg_file(dir, "cgroup.subtree_control", "");
  create_cg_file(dir, "cgroup.events", "populated 1\nfrozen 0\n");
  create_cg_file(dir, "cgroup.kill", "");
  create_cg_file(dir, "cgroup.type", "domain\n");
  *__errno_location() = saved;
}

static void clean_cgroup_files(const char *dir) {
  static int (*real_unlinkat)(int, const char *, int);
  if (!real_unlinkat) real_unlinkat = dlsym(RTLD_NEXT, "unlinkat");
  static const char *const names[] = {
    "cgroup.procs", "cgroup.threads", "cgroup.controllers",
    "cgroup.subtree_control", "cgroup.events", "cgroup.kill", "cgroup.type"
  };
  int saved = *__errno_location();
  char p[640];
  size_t dl = s_len(dir);
  for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
    size_t nl = s_len(names[k]);
    if (dl + 1 + nl >= sizeof(p)) continue;
    size_t pos = 0;
    for (size_t i = 0; i < dl; i++) p[pos++] = dir[i];
    if (pos == 0 || p[pos - 1] != '/') p[pos++] = '/';
    for (size_t i = 0; i <= nl; i++) p[pos++] = names[k][i];
    real_unlinkat(AT_FDCWD, p, 0);
  }
  *__errno_location() = saved;
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

int statfs(const char *path, void *buf) {
  static int (*real_statfs)(const char *, void *);
  if (!real_statfs) real_statfs = dlsym(RTLD_NEXT, "statfs");
  int r = real_statfs(path, buf);
  if (r == 0 && buf && is_cgroup_path(path)) {
    *(long *)buf = CGROUP2_SUPER_MAGIC;
  }
  return r;
}

int statfs64(const char *path, void *buf) {
  static int (*real_statfs64)(const char *, void *);
  if (!real_statfs64) real_statfs64 = dlsym(RTLD_NEXT, "statfs64");
  int r = real_statfs64(path, buf);
  if (r == 0 && buf && is_cgroup_path(path)) {
    *(long *)buf = CGROUP2_SUPER_MAGIC;
  }
  return r;
}

int fstatfs(int fd, void *buf) {
  static int (*real_fstatfs)(int, void *);
  if (!real_fstatfs) real_fstatfs = dlsym(RTLD_NEXT, "fstatfs");
  int r = real_fstatfs(fd, buf);
  if (r == 0 && buf) {
    char p[512];
    if (fd_to_path(fd, p, sizeof(p)) >= 0 && is_cgroup_path(p)) {
      *(long *)buf = CGROUP2_SUPER_MAGIC;
    }
  }
  return r;
}

int fstatfs64(int fd, void *buf) {
  static int (*real_fstatfs64)(int, void *);
  if (!real_fstatfs64) real_fstatfs64 = dlsym(RTLD_NEXT, "fstatfs64");
  int r = real_fstatfs64(fd, buf);
  if (r == 0 && buf) {
    char p[512];
    if (fd_to_path(fd, p, sizeof(p)) >= 0 && is_cgroup_path(p)) {
      *(long *)buf = CGROUP2_SUPER_MAGIC;
    }
  }
  return r;
}

int mkdir(const char *path, mode_t mode) {
  static int (*real_mkdir)(const char *, mode_t);
  if (!real_mkdir) real_mkdir = dlsym(RTLD_NEXT, "mkdir");
  int r = real_mkdir(path, mode);
  if (is_cgroup_path(path) && (r == 0 || *__errno_location() == EEXIST)) {
    populate_cgroup_dir(path);
  }
  return r;
}

int mkdirat(int dirfd, const char *path, mode_t mode) {
  static int (*real_mkdirat)(int, const char *, mode_t);
  if (!real_mkdirat) real_mkdirat = dlsym(RTLD_NEXT, "mkdirat");
  int r = real_mkdirat(dirfd, path, mode);
  char full[512];
  if ((r == 0 || *__errno_location() == EEXIST) &&
      resolve_at(dirfd, path, full, sizeof(full)) && is_cgroup_path(full)) {
    populate_cgroup_dir(full);
  }
  return r;
}

int rmdir(const char *path) {
  static int (*real_rmdir)(const char *);
  if (!real_rmdir) real_rmdir = dlsym(RTLD_NEXT, "rmdir");
  int r = real_rmdir(path);
  if (r < 0 && *__errno_location() == ENOTEMPTY && is_cgroup_path(path)) {
    clean_cgroup_files(path);
    r = real_rmdir(path);
  }
  return r;
}

int unlinkat(int dirfd, const char *path, int flags) {
  static int (*real_unlinkat)(int, const char *, int);
  if (!real_unlinkat) real_unlinkat = dlsym(RTLD_NEXT, "unlinkat");
  int r = real_unlinkat(dirfd, path, flags);
  if (r < 0 && (flags & AT_REMOVEDIR) && *__errno_location() == ENOTEMPTY) {
    char full[512];
    if (resolve_at(dirfd, path, full, sizeof(full)) && is_cgroup_path(full)) {
      clean_cgroup_files(full);
      r = real_unlinkat(dirfd, path, flags);
    }
  }
  return r;
}

int mount(const char *src, const char *target, const char *fstype, unsigned long flags, const void *data) {
  static int (*real_mount)(const char *, const char *, const char *, unsigned long, const void *);
  if (!real_mount) real_mount = dlsym(RTLD_NEXT, "mount");
  int r = real_mount(src, target, fstype, flags, data);
  if (r < 0 && is_run_user_path(target)) {
    int e = *__errno_location();
    if (e == EPERM || e == EACCES) {
      *__errno_location() = 0;
      return 0;
    }
  }
  return r;
}

int umount2(const char *target, int flags) {
  static int (*real_umount2)(const char *, int);
  if (!real_umount2) real_umount2 = dlsym(RTLD_NEXT, "umount2");
  int r = real_umount2(target, flags);
  if (r < 0 && is_run_user_path(target)) {
    int e = *__errno_location();
    if (e == EPERM || e == EACCES || e == EINVAL) {
      *__errno_location() = 0;
      return 0;
    }
  }
  return r;
}
