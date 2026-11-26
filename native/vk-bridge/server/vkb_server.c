/* vk-bridge server v0: listens on abstract AF_UNIX @lodos-vk (R13: shell uid can't bind path sockets),
 * one thread per client. Client sends "VKB1" + 2 ring fds (c2s, s2c); server loops
 * ring_pop -> vkr_srv_dispatch (decode, call real driver, encode) -> ring_push.
 * Driver: dlopen(LODOS_VK_LIB or -l path, default system libvulkan.so); pluggable for vk-layer later
 * (R12: an ICD lib would need vk_icdGetInstanceProcAddr instead of dlsym).
 * usage: vkb_server [-s name] [-l libpath] */
#define _GNU_SOURCE
#include "vkb_rpc.h"
#include "../transport/ring.h"
#include "../transport/sock.h"
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define ARENA_SZ (4u << 20)

static void *g_lib;
static PFN_vkGetInstanceProcAddr g_gipa;
static vkr_srv_fns g_fns;

/* ---- per-client handle table: id = index + 1 (0 = VK_NULL_HANDLE) */
typedef struct { VkObjectType t; uint64_t h; } hent;
typedef struct { hent *e; uint32_t n, cap; } htab;

uint64_t vkr_h_out(void *hctx, VkObjectType t, uint64_t h) {
    htab *ht = hctx;
    if (!h) return 0;
    uint32_t freei = UINT32_MAX;
    for (uint32_t i = 0; i < ht->n; i++) {
        if (ht->e[i].h == h && ht->e[i].t == t) return i + 1;
        if (!ht->e[i].h && freei == UINT32_MAX) freei = i;
    }
    if (freei == UINT32_MAX) {
        if (ht->n == ht->cap) {
            uint32_t nc = ht->cap ? ht->cap * 2 : 64;
            hent *ne = realloc(ht->e, nc * sizeof *ne);
            if (!ne) abort();
            ht->e = ne; ht->cap = nc;
        }
        freei = ht->n++;
    }
    ht->e[freei] = (hent){ t, h };
    return freei + 1;
}

uint64_t vkr_h_in(void *hctx, VkObjectType t, uint64_t id, int *ok) {
    htab *ht = hctx;
    *ok = 1;
    if (!id) return 0;
    if (id > ht->n || !ht->e[id - 1].h || ht->e[id - 1].t != t) { *ok = 0; return 0; }
    return ht->e[id - 1].h;
}

void vkr_h_drop(void *hctx, VkObjectType t, uint64_t h) {
    htab *ht = hctx;
    for (uint32_t i = 0; i < ht->n; i++)
        if (ht->e[i].h == h && ht->e[i].t == t) ht->e[i] = (hent){ 0, 0 };
    /* children of a destroyed instance (physical devices) or device (queues) die with it */
    if (t == VK_OBJECT_TYPE_INSTANCE) {
        int live = 0;
        for (uint32_t i = 0; i < ht->n; i++) live |= ht->e[i].t == VK_OBJECT_TYPE_INSTANCE && ht->e[i].h;
        if (!live)
            for (uint32_t i = 0; i < ht->n; i++)
                if (ht->e[i].t == VK_OBJECT_TYPE_PHYSICAL_DEVICE) ht->e[i] = (hent){ 0, 0 };
    } else if (t == VK_OBJECT_TYPE_DEVICE) {
        int live = 0;
        for (uint32_t i = 0; i < ht->n; i++) live |= ht->e[i].t == VK_OBJECT_TYPE_DEVICE && ht->e[i].h;
        if (!live)
            for (uint32_t i = 0; i < ht->n; i++)
                if (ht->e[i].t == VK_OBJECT_TYPE_QUEUE) ht->e[i] = (hent){ 0, 0 };
    }
}

/* ---- client thread */
typedef struct {
    int sock;
    ring c2s, s2c;
    atomic_int done;
    pthread_t watcher;
} client;

/* socket EOF = client gone (crash or close): unblock the worker by pushing BYE into c2s.
 * Safe as the producer side is dead/finished. */
static void *watch_fn(void *arg) {
    client *c = arg;
    char b[16]; int nf;
    while (sock_recv(c->sock, b, sizeof b, NULL, &nf) > 0) {}
    if (!atomic_load(&c->done)) { uint32_t bye = VKR_CMD_BYE; ring_push(&c->c2s, &bye, 4); }
    return NULL;
}

static void *client_fn(void *arg) {
    client *c = arg;
    int fds[SOCK_MAX_FDS], nf = 0;
    char hello[8];
    uint8_t *req = NULL, *rep = NULL, *arena_mem = NULL;
    htab ht = { 0 };
    ssize_t n = sock_recv(c->sock, hello, sizeof hello, fds, &nf);
    if (n != 4 || memcmp(hello, "VKB1", 4) || nf != 2) {
        fprintf(stderr, "vkb_server: bad hello (n=%zd nfds=%d)\n", n, nf);
        for (int i = 0; i < nf; i++) close(fds[i]);
        close(c->sock); free(c);
        return NULL;
    }
    if (ring_attach(&c->c2s, fds[0]) < 0 || ring_attach(&c->s2c, fds[1]) < 0) {
        fprintf(stderr, "vkb_server: ring_attach failed\n");
        close(fds[0]); close(fds[1]); close(c->sock); free(c);
        return NULL;
    }
    uint32_t reqcap = ring_max_msg(&c->c2s), repcap = ring_max_msg(&c->s2c);
    req = malloc(reqcap); rep = malloc(repcap); arena_mem = malloc(ARENA_SZ);
    if (!req || !rep || !arena_mem) abort();
    pthread_create(&c->watcher, NULL, watch_fn, c);
    sock_send(c->sock, "ok", 2, NULL, 0);
    uint64_t ncalls = 0;
    for (;;) {
        int64_t len = ring_pop(&c->c2s, req, reqcap);
        if (len < 0) { fprintf(stderr, "vkb_server: ring_pop %lld\n", (long long)len); break; }
        uint32_t cmd = 0;
        if (len >= 4) memcpy(&cmd, req, 4);
        if (cmd == VKR_CMD_BYE) break;
        vkr_arena a = { arena_mem, ARENA_SZ, 0 };
        size_t rl = vkr_srv_dispatch(&g_fns, &ht, &a, req, (size_t)len, rep, repcap);
        if (!rl) { /* unknown cmd / malformed header: reply cmd + VK_ERROR_FEATURE_NOT_PRESENT */
            VkResult e = VK_ERROR_FEATURE_NOT_PRESENT;
            memcpy(rep, &cmd, 4); memcpy(rep + 4, &e, 4); rl = 8;
        }
        ring_push(&c->s2c, rep, (uint32_t)rl);
        ncalls++;
    }
    atomic_store(&c->done, 1);
    shutdown(c->sock, SHUT_RDWR);
    pthread_join(c->watcher, NULL);
    /* leaked devices/instances of a dead/sloppy client */
#ifdef VKB_HAS_vkDestroyDevice
    for (uint32_t i = 0; i < ht.n; i++)
        if (ht.e[i].t == VK_OBJECT_TYPE_DEVICE && ht.e[i].h && g_fns.vkDestroyDevice)
            g_fns.vkDestroyDevice((VkDevice)(uintptr_t)ht.e[i].h, NULL);
#endif
    for (uint32_t i = 0; i < ht.n; i++)
        if (ht.e[i].t == VK_OBJECT_TYPE_INSTANCE && ht.e[i].h && g_fns.vkDestroyInstance)
            g_fns.vkDestroyInstance((VkInstance)(uintptr_t)ht.e[i].h, NULL);
    fprintf(stderr, "vkb_server: client done, %llu calls\n", (unsigned long long)ncalls);
    free(ht.e); free(req); free(rep); free(arena_mem);
    ring_destroy(&c->c2s); ring_destroy(&c->s2c);
    close(c->sock); free(c);
    return NULL;
}

static void *getproc(const char *name, void *ud) {
    (void)ud;
    void *p = dlsym(g_lib, name);
    if (!p && g_gipa) p = (void *)g_gipa(NULL, name);
    return p;
}

int main(int argc, char **argv) {
    const char *name = getenv("LODOS_VK_SOCK");
    if (!name || !*name) name = "lodos-vk";
    const char *lib = getenv("LODOS_VK_LIB");
    if (!lib) lib = "libvulkan.so";
    int o;
    while ((o = getopt(argc, argv, "s:l:")) != -1) {
        if (o == 's') name = optarg;
        else if (o == 'l') lib = optarg;
        else { fprintf(stderr, "usage: %s [-s name] [-l libvulkan]\n", argv[0]); return 2; }
    }
    signal(SIGPIPE, SIG_IGN);
    g_lib = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!g_lib) { fprintf(stderr, "vkb_server: dlopen %s: %s\n", lib, dlerror()); return 1; }
    g_gipa = (PFN_vkGetInstanceProcAddr)dlsym(g_lib, "vkGetInstanceProcAddr");
    if (!g_gipa) g_gipa = (PFN_vkGetInstanceProcAddr)dlsym(g_lib, "vk_icdGetInstanceProcAddr");
    int miss = vkr_srv_load(&g_fns, getproc, NULL);
    if (miss) fprintf(stderr, "vkb_server: warning: %d bridged fns missing in %s\n", miss, lib);

    int ls = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    if (ls < 0) { perror("socket"); return 1; }
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    socklen_t alen;
    const char *disp = name;
    if (name[0] == '/') {
        size_t nl = strlen(name);
        if (nl + 1 > sizeof a.sun_path) { fprintf(stderr, "name too long\n"); return 1; }
        unlink(name);
        memcpy(a.sun_path, name, nl + 1);
        alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + nl + 1);
    } else {
        if (name[0] == '@') name++;
        size_t nl = strlen(name);
        if (nl + 1 > sizeof a.sun_path) { fprintf(stderr, "name too long\n"); return 1; }
        memcpy(a.sun_path + 1, name, nl);
        alen = (socklen_t)(offsetof(struct sockaddr_un, sun_path) + 1 + nl);
        disp = name;
    }
    if (bind(ls, (struct sockaddr *)&a, alen) < 0) {
        perror("bind"); return 1;
    }
    if (listen(ls, 16) < 0) { perror("listen"); return 1; }
    fprintf(stderr, "vkb_server: listening %s%s lib=%s\n", name[0] == '/' ? "" : "@", disp, lib);
    for (;;) {
        int s = accept4(ls, NULL, NULL, SOCK_CLOEXEC);
        if (s < 0) { if (errno == EINTR) continue; perror("accept"); return 1; }
        client *c = calloc(1, sizeof *c);
        if (!c) { close(s); continue; }
        c->sock = s; c->c2s.fd = c->s2c.fd = -1;
        pthread_t t;
        if (pthread_create(&t, NULL, client_fn, c)) { close(s); free(c); continue; }
        pthread_detach(t);
    }
}
