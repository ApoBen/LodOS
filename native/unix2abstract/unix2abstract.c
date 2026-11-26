// LD_PRELOAD shim (guest glibc, aarch64): rewrite AF_UNIX path sockets under $LODOS_U2A_PREFIX
// (':'-separated prefixes, default /tmp/xdg) to abstract sockets "\0<path>". Workaround for R13 (shell uid can't create
// sock_file in /data/local/tmp). Header-free so it builds with NDK clang (-target aarch64-linux-gnu).
typedef unsigned long size_t;
typedef unsigned int socklen_t;
struct sockaddr_un { unsigned short sun_family; char sun_path[108]; };
#define AF_UNIX 1
#define RTLD_NEXT ((void *)-1l)
extern void *dlsym(void *, const char *);
extern char *getenv(const char *);


static int rewrite(const void *a, socklen_t len, struct sockaddr_un *out, socklen_t *olen) {
  const struct sockaddr_un *u = a;
  if (!a || len < 3 || u->sun_family != AF_UNIX || u->sun_path[0] == 0) return 0;
  const char *pre = getenv("LODOS_U2A_PREFIX");
  if (!pre) pre = "/tmp/xdg";
  size_t i, n = 0;
  int hit = 0;
  while (*pre && !hit) {
    size_t pl = 0;
    while (pre[pl] && pre[pl] != ':') pl++;
    if (pl) { hit = 1; for (i = 0; i < pl; i++) if (u->sun_path[i] != pre[i]) { hit = 0; break; } }
    pre += pl; if (*pre == ':') pre++;
  }
  if (!hit) return 0;
  while (n < sizeof(u->sun_path) - 1 && n + 2 < len && u->sun_path[n]) n++;
  out->sun_family = AF_UNIX;
  out->sun_path[0] = 0;
  for (i = 0; i < n; i++) out->sun_path[i + 1] = u->sun_path[i];
  *olen = (socklen_t)(2 + 1 + n);
  return 1;
}

typedef int (*fn_t)(int, const void *, socklen_t);
#define WRAP(name) \
  int name(int fd, const void *a, socklen_t len) { \
    static fn_t real; if (!real) real = (fn_t)dlsym(RTLD_NEXT, #name); \
    struct sockaddr_un u; socklen_t l; \
    if (rewrite(a, len, &u, &l)) return real(fd, &u, l); \
    return real(fd, a, len); }
WRAP(bind)
WRAP(connect)
