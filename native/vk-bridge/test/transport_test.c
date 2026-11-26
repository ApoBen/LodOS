/* transport_test: fork server/client over socketpair; rings (memfd) passed via SCM_RIGHTS.
 * 1) 100k msgs random size 1..64KiB (seed fixed) c2s, server replies checksum s2c.
 * 2) FDPASS: client sends memfd, server writes pattern, client verifies.
 * 3) LAT: 64B roundtrip p50/p99. */
#define _GNU_SOURCE
#include "../transport/ring.h"
#include "../transport/sock.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define NMSGS 100000
#define MAXSZ 65536
#define LAT_N 20000
#define LAT_WARM 2000
#define FD_SZ 65536

enum { CMD_BULK = 1, CMD_LAT = 2, CMD_FD = 3, CMD_QUIT = 4 };

#define DIE(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)
#define CHK(x) do { long _r = (long)(x); if (_r < 0) DIE("%s:%d %s -> %ld (%s)", __FILE__, __LINE__, #x, _r, strerror((int)-_r)); } while (0)

static uint64_t xs(uint64_t *s) { uint64_t x = *s; x ^= x << 13; x ^= x >> 7; x ^= x << 17; return *s = x; }

static uint64_t csum(const uint8_t *p, uint32_t n) {
    uint64_t h = 0xcbf29ce484222325ull ^ n;
    uint32_t i = 0;
    for (; i + 8 <= n; i += 8) { uint64_t w; memcpy(&w, p + i, 8); h = (h ^ w) * 0x100000001b3ull; h ^= h >> 29; }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

static double now_s(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static int cmp_u64(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }

static void server(int s) {
    int fds[SOCK_MAX_FDS], nfds;
    uint8_t cmd[16];
    /* receive the two ring fds */
    CHK(sock_recv(s, cmd, sizeof cmd, fds, &nfds));
    if (nfds != 2) DIE("server: expected 2 ring fds, got %d", nfds);
    ring c2s, s2c;
    CHK(ring_attach(&c2s, fds[0]));
    CHK(ring_attach(&s2c, fds[1]));
    uint8_t *buf = malloc(MAXSZ);
    for (;;) {
        ssize_t n = sock_recv(s, cmd, sizeof cmd, fds, &nfds);
        CHK(n);
        if (n == 0 || cmd[0] == CMD_QUIT) break;
        if (cmd[0] == CMD_BULK || cmd[0] == CMD_LAT) {
            uint32_t cnt; memcpy(&cnt, cmd + 4, 4);
            for (uint32_t i = 0; i < cnt; i++) {
                int64_t len = ring_pop(&c2s, buf, MAXSZ); CHK(len);
                if (cmd[0] == CMD_BULK) {
                    uint64_t c = csum(buf, (uint32_t)len);
                    CHK(ring_push(&s2c, &c, 8));
                } else {
                    CHK(ring_push(&s2c, buf, (uint32_t)len));
                }
            }
        } else if (cmd[0] == CMD_FD) {
            if (nfds != 1) DIE("server: FD cmd without fd");
            uint8_t *p = mmap(NULL, FD_SZ, PROT_READ | PROT_WRITE, MAP_SHARED, fds[0], 0);
            if (p == MAP_FAILED) DIE("server mmap: %s", strerror(errno));
            for (uint32_t i = 0; i < FD_SZ; i++) p[i] = (uint8_t)(i * 31u + 7u);
            munmap(p, FD_SZ);
            close(fds[0]);
            CHK(sock_send(s, "ok", 2, NULL, 0));
        }
    }
    free(buf);
    ring_destroy(&c2s); ring_destroy(&s2c);
}

static void client(int s) {
    ring c2s, s2c;
    CHK(ring_create(&c2s, 0));
    CHK(ring_create(&s2c, 0));
    int rfds[2] = { c2s.fd, s2c.fd };
    uint8_t cmd[16] = { 0 };
    CHK(sock_send(s, "rings", 5, rfds, 2));

    /* 1) bulk */
    uint8_t *buf = malloc(MAXSZ + 8);
    uint64_t seed = 0x1234567890abcdefull;
    uint32_t cnt = NMSGS;
    cmd[0] = CMD_BULK; memcpy(cmd + 4, &cnt, 4);
    CHK(sock_send(s, cmd, sizeof cmd, NULL, 0));
    uint64_t bytes = 0;
    double t0 = now_s();
    for (uint32_t i = 0; i < cnt; i++) {
        uint32_t len = (uint32_t)(xs(&seed) % MAXSZ) + 1;
        for (uint32_t j = 0; j < len; j += 8) { uint64_t w = xs(&seed); memcpy(buf + j, &w, 8); }
        CHK(ring_push(&c2s, buf, len));
        uint64_t want = csum(buf, len), got;
        int64_t n = ring_pop(&s2c, &got, 8); CHK(n);
        if (n != 8 || got != want) DIE("bulk mismatch at msg %u len %u", i, len);
        bytes += len;
    }
    double dt = now_s() - t0;
    printf("TRANSPORT ok msgs=%u bytes=%" PRIu64 " time=%.3fs thr=%.1fMB/s (lockstep, incl gen+csum)\n",
           cnt, bytes, dt, bytes / dt / 1e6);

    /* 2) fd pass */
    int mfd = (int)syscall(SYS_memfd_create, "vkb-fdpass", 1u);
    CHK(mfd < 0 ? -errno : 0);
    CHK(ftruncate(mfd, FD_SZ) < 0 ? -errno : 0);
    cmd[0] = CMD_FD;
    CHK(sock_send(s, cmd, sizeof cmd, &mfd, 1));
    int nf; char ok[4];
    ssize_t r = sock_recv(s, ok, sizeof ok, NULL, &nf); CHK(r);
    if (r != 2 || memcmp(ok, "ok", 2)) DIE("fdpass: bad reply");
    uint8_t *p = mmap(NULL, FD_SZ, PROT_READ, MAP_SHARED, mfd, 0);
    if (p == MAP_FAILED) DIE("client mmap: %s", strerror(errno));
    for (uint32_t i = 0; i < FD_SZ; i++)
        if (p[i] != (uint8_t)(i * 31u + 7u)) DIE("fdpass: pattern mismatch at %u", i);
    munmap(p, FD_SZ); close(mfd);
    printf("FDPASS ok bytes=%d\n", FD_SZ);

    /* 3) latency 64B roundtrip */
    uint64_t *lat = malloc(sizeof(uint64_t) * LAT_N);
    cnt = LAT_N + LAT_WARM;
    cmd[0] = CMD_LAT; memcpy(cmd + 4, &cnt, 4);
    CHK(sock_send(s, cmd, sizeof cmd, NULL, 0));
    uint8_t m[64], back[64];
    memset(m, 0xab, sizeof m);
    for (uint32_t i = 0; i < cnt; i++) {
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        CHK(ring_push(&c2s, m, 64));
        int64_t n = ring_pop(&s2c, back, 64); CHK(n);
        clock_gettime(CLOCK_MONOTONIC, &b);
        if (n != 64 || memcmp(m, back, 64)) DIE("lat: echo mismatch");
        if (i >= LAT_WARM)
            lat[i - LAT_WARM] = (uint64_t)(b.tv_sec - a.tv_sec) * 1000000000ull + (uint64_t)(b.tv_nsec - a.tv_nsec);
    }
    qsort(lat, LAT_N, sizeof *lat, cmp_u64);
    printf("LAT_US p50=%.2f p99=%.2f max=%.2f n=%d\n",
           lat[LAT_N / 2] / 1e3, lat[LAT_N * 99 / 100] / 1e3, lat[LAT_N - 1] / 1e3, LAT_N);

    cmd[0] = CMD_QUIT;
    CHK(sock_send(s, cmd, sizeof cmd, NULL, 0));
    free(lat); free(buf);
    ring_destroy(&c2s); ring_destroy(&s2c);
}

int main(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    int sv[2];
    CHK(sock_pair(sv));
    pid_t pid = fork();
    if (pid < 0) DIE("fork: %s", strerror(errno));
    if (pid == 0) { close(sv[0]); server(sv[1]); _exit(0); }
    close(sv[1]);
    client(sv[0]);
    close(sv[0]);
    int st;
    if (waitpid(pid, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st)) DIE("server failed (st=%d)", st);
    return 0;
}
