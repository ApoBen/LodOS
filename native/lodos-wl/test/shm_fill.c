/*
 * shm_fill.c - Minimal raw-wire Wayland wl_shm + xdg_toplevel test client (T022).
 * Connects to WAYLAND_DISPLAY (absolute path or under XDG_RUNTIME_DIR),
 * creates a 512x512 XDG toplevel, fills solid 0xFF00FF00 (ARGB8888 green),
 * commits, and stays alive for 10s (--bench N optional for FPS/latency probe).
 *
 * Zero external Wayland library dependencies; builds cleanly with Android NDK clang.
 * License: GPL-3.0-or-later
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#ifdef __ANDROID__
#include <android/log.h>
#define LOGI(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); __android_log_print(ANDROID_LOG_INFO, "SHM_FILL", __VA_ARGS__); } while (0)
#define LOGE(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); __android_log_print(ANDROID_LOG_ERROR, "SHM_FILL", __VA_ARGS__); } while (0)
#else
#define LOGI(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#define LOGE(...) do { fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); } while (0)
#endif

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#define WIDTH 512
#define HEIGHT 512
#define STRIDE (WIDTH * 4)
#define POOL_SIZE (STRIDE * HEIGHT)
#define FILL_PIXEL 0xFF00FF00u /* ARGB8888 solid green */
#define WL_SHM_FORMAT_ARGB8888 0u

/* Client-allocated Wayland object IDs (1 = wl_display) */
enum {
    ID_DISPLAY      = 1,
    ID_REGISTRY     = 2,
    ID_SYNC_CB      = 3,
    ID_COMPOSITOR   = 4,
    ID_SHM          = 5,
    ID_XDG_WM_BASE  = 6,
    ID_SURFACE      = 7,
    ID_XDG_SURFACE  = 8,
    ID_XDG_TOPLEVEL = 9,
    ID_SHM_POOL     = 10,
    ID_BUFFER       = 11,
    ID_NEXT_START   = 12
};

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static int create_shm_fd(size_t size) {
    int fd = (int)syscall(__NR_memfd_create, "lodos_shm_fill", MFD_CLOEXEC);
    if (fd < 0) {
        LOGE("memfd_create failed: %s", strerror(errno));
        return -1;
    }
    if (ftruncate(fd, (off_t)size) < 0) {
        LOGE("ftruncate failed: %s", strerror(errno));
        close(fd);
        return -1;
    }
    return fd;
}

static int send_all(int sock, const void *buf, size_t len) {
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        ssize_t n = send(sock, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOGE("send failed: %s", strerror(errno));
            return -1;
        }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

static int send_with_fd(int sock, const void *buf, size_t len, int pass_fd) {
    struct iovec iov = { .iov_base = (void *)buf, .iov_len = len };
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } u;
    memset(&u, 0, sizeof(u));

    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = u.buf;
    msg.msg_controllen = sizeof(u.buf);

    struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &pass_fd, sizeof(int));

    for (;;) {
        ssize_t n = sendmsg(sock, &msg, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            LOGE("sendmsg(SCM_RIGHTS) failed: %s", strerror(errno));
            return -1;
        }
        if ((size_t)n < len) {
            return send_all(sock, (const uint8_t *)buf + n, len - (size_t)n);
        }
        return 0;
    }
}

static inline uint32_t wl_hdr(uint32_t size_bytes, uint16_t opcode) {
    return (size_bytes << 16) | (uint32_t)opcode;
}

static int send_u0(int sock, uint32_t obj, uint16_t op) {
    uint32_t w[2] = { obj, wl_hdr(8, op) };
    return send_all(sock, w, sizeof(w));
}

static int send_u1(int sock, uint32_t obj, uint16_t op, uint32_t a0) {
    uint32_t w[3] = { obj, wl_hdr(12, op), a0 };
    return send_all(sock, w, sizeof(w));
}

static int send_u2(int sock, uint32_t obj, uint16_t op, uint32_t a0, uint32_t a1) {
    uint32_t w[4] = { obj, wl_hdr(16, op), a0, a1 };
    return send_all(sock, w, sizeof(w));
}

static int send_u3(int sock, uint32_t obj, uint16_t op, uint32_t a0, uint32_t a1, uint32_t a2) {
    uint32_t w[5] = { obj, wl_hdr(20, op), a0, a1, a2 };
    return send_all(sock, w, sizeof(w));
}

static int send_u4(int sock, uint32_t obj, uint16_t op, uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3) {
    uint32_t w[6] = { obj, wl_hdr(24, op), a0, a1, a2, a3 };
    return send_all(sock, w, sizeof(w));
}

static int send_registry_bind(int sock, uint32_t name, const char *iface, uint32_t ver, uint32_t new_id) {
    uint32_t slen = (uint32_t)strlen(iface) + 1u;
    uint32_t spad = (slen + 3u) & ~3u;
    uint32_t total = 8u + 4u + 4u + spad + 4u + 4u;
    uint32_t w[32];
    if (total > sizeof(w)) return -1;
    memset(w, 0, total);
    w[0] = ID_REGISTRY;
    w[1] = wl_hdr(total, 0); /* wl_registry::bind = 0 */
    w[2] = name;
    w[3] = slen;
    memcpy(&w[4], iface, slen);
    w[4 + spad / 4] = ver;
    w[5 + spad / 4] = new_id;
    return send_all(sock, w, total);
}

static int send_string_req(int sock, uint32_t obj, uint16_t op, const char *str) {
    uint32_t slen = (uint32_t)strlen(str) + 1u;
    uint32_t spad = (slen + 3u) & ~3u;
    uint32_t total = 8u + 4u + spad;
    uint32_t w[32];
    if (total > sizeof(w)) return -1;
    memset(w, 0, total);
    w[0] = obj;
    w[1] = wl_hdr(total, op);
    w[2] = slen;
    memcpy(&w[3], str, slen);
    return send_all(sock, w, total);
}

struct wl_state {
    uint32_t comp_name, comp_ver;
    uint32_t shm_name, shm_ver;
    uint32_t xdg_name, xdg_ver;
    uint32_t wait_cb_id;
    int sync_done;
    int configured;
    int frame_done;
    uint8_t rx[65536];
    size_t rx_len;
};

static int pump_events(int sock, struct wl_state *st, int timeout_ms) {
    struct pollfd pfd = { .fd = sock, .events = POLLIN };
    int prc = poll(&pfd, 1, timeout_ms);
    if (prc < 0) {
        if (errno == EINTR) return 0;
        LOGE("poll failed: %s", strerror(errno));
        return -1;
    }
    if (prc == 0) return 0;

    if (sizeof(st->rx) - st->rx_len < 4096) {
        LOGE("rx buffer full");
        return -1;
    }

    struct iovec iov = {
        .iov_base = st->rx + st->rx_len,
        .iov_len = sizeof(st->rx) - st->rx_len
    };
    char cbuf[256];
    struct msghdr msg;
    memset(&msg, 0, sizeof(msg));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cbuf;
    msg.msg_controllen = sizeof(cbuf);

    ssize_t n = recvmsg(sock, &msg, 0);
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) return 0;
        LOGE("recvmsg failed: %s", strerror(errno));
        return -1;
    }
    if (n == 0) {
        LOGE("server closed Wayland connection");
        return -1;
    }

    /* Close any unexpected passed fds so we never leak descriptors */
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c != NULL; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
            size_t nfds = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            int *fds = (int *)CMSG_DATA(c);
            for (size_t i = 0; i < nfds; i++) close(fds[i]);
        }
    }

    st->rx_len += (size_t)n;

    size_t off = 0;
    while (st->rx_len - off >= 8) {
        uint32_t obj = 0, sz_op = 0;
        memcpy(&obj, st->rx + off, 4);
        memcpy(&sz_op, st->rx + off + 4, 4);
        uint32_t sz = sz_op >> 16;
        uint16_t op = (uint16_t)(sz_op & 0xFFFFu);
        if (sz < 8 || (sz & 3u) != 0 || sz > sizeof(st->rx)) {
            LOGE("invalid wire message obj=%u sz=%u op=%u", obj, sz, op);
            return -1;
        }
        if (st->rx_len - off < sz) break;

        const uint8_t *body = st->rx + off + 8;
        uint32_t blen = sz - 8;

        if (obj == ID_DISPLAY && op == 0 && blen >= 12) {
            uint32_t eobj = 0, ecode = 0, slen = 0;
            memcpy(&eobj, body, 4);
            memcpy(&ecode, body + 4, 4);
            memcpy(&slen, body + 8, 4);
            const char *emsg = (blen > 12 && slen > 0) ? (const char *)(body + 12) : "";
            LOGE("wl_display.error obj=%u code=%u msg=%s", eobj, ecode, emsg);
            return -1;
        } else if (obj == ID_REGISTRY && op == 0 && blen >= 12) {
            uint32_t gname = 0, slen = 0;
            memcpy(&gname, body, 4);
            memcpy(&slen, body + 4, 4);
            uint32_t spad = (slen + 3u) & ~3u;
            if (8u + spad <= blen && slen > 0) {
                const char *iface = (const char *)(body + 8);
                uint32_t gver = 0;
                memcpy(&gver, body + 8 + spad, 4);
                if (strcmp(iface, "wl_compositor") == 0) {
                    st->comp_name = gname;
                    st->comp_ver = gver;
                } else if (strcmp(iface, "wl_shm") == 0) {
                    st->shm_name = gname;
                    st->shm_ver = gver;
                } else if (strcmp(iface, "xdg_wm_base") == 0) {
                    st->xdg_name = gname;
                    st->xdg_ver = gver;
                }
            }
        } else if (obj == ID_SYNC_CB && op == 0) {
            st->sync_done = 1;
        } else if (obj == st->wait_cb_id && op == 0) {
            st->frame_done = 1;
        } else if (obj == ID_XDG_WM_BASE && op == 0 && blen >= 4) {
            uint32_t serial = 0;
            memcpy(&serial, body, 4);
            if (send_u1(sock, ID_XDG_WM_BASE, 3 /* pong */, serial) < 0) return -1;
        } else if (obj == ID_XDG_SURFACE && op == 0 && blen >= 4) {
            uint32_t serial = 0;
            memcpy(&serial, body, 4);
            if (send_u1(sock, ID_XDG_SURFACE, 4 /* ack_configure */, serial) < 0) return -1;
            st->configured = 1;
        }

        off += sz;
    }

    if (off > 0) {
        if (st->rx_len > off) {
            memmove(st->rx, st->rx + off, st->rx_len - off);
        }
        st->rx_len -= off;
    }
    return 0;
}

static int connect_wayland(const char *arg_path) {
    char full_path[256];
    const char *wd = arg_path ? arg_path : getenv("WAYLAND_DISPLAY");
    if (!wd || !wd[0]) wd = "wayland-0";

    if (wd[0] == '/') {
        snprintf(full_path, sizeof(full_path), "%s", wd);
    } else {
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        if (!xdg || !xdg[0]) {
            LOGE("relative WAYLAND_DISPLAY='%s' requires XDG_RUNTIME_DIR", wd);
            return -1;
        }
        snprintf(full_path, sizeof(full_path), "%s/%s", xdg, wd);
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(full_path) >= sizeof(addr.sun_path)) {
        LOGE("socket path too long: %s", full_path);
        return -1;
    }
    strcpy(addr.sun_path, full_path);

    for (int attempt = 0; attempt < 60; attempt++) {
        int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (sock < 0) {
            LOGE("socket() failed: %s", strerror(errno));
            return -1;
        }
        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            LOGI("connected to %s", full_path);
            return sock;
        }
        int err = errno;
        close(sock);
        if ((err == ENOENT || err == ECONNREFUSED) && attempt < 59) {
            usleep(50 * 1000);
            continue;
        }
        LOGE("connect(%s) failed: %s", full_path, strerror(err));
        return -1;
    }
    return -1;
}

int main(int argc, char **argv) {
    const char *sock_arg = NULL;
    int bench_frames = 0;
    int hold_ms = 10000;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--bench") == 0 && i + 1 < argc) {
            bench_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--hold-ms") == 0 && i + 1 < argc) {
            hold_ms = atoi(argv[++i]);
        } else if (argv[i][0] != '-') {
            sock_arg = argv[i];
        }
    }

    int sock = connect_wayland(sock_arg);
    if (sock < 0) return 1;

    struct wl_state st;
    memset(&st, 0, sizeof(st));

    /* 1. Discover globals */
    if (send_u1(sock, ID_DISPLAY, 1 /* get_registry */, ID_REGISTRY) < 0) return 1;
    if (send_u1(sock, ID_DISPLAY, 0 /* sync */, ID_SYNC_CB) < 0) return 1;

    uint64_t deadline = now_us() + 5000000ull;
    while (!st.sync_done) {
        if (now_us() > deadline) {
            LOGE("timeout waiting for wl_registry sync");
            return 1;
        }
        if (pump_events(sock, &st, 100) < 0) return 1;
    }

    if (!st.comp_name || !st.shm_name || !st.xdg_name) {
        LOGE("missing required globals (comp=%u shm=%u xdg=%u)", st.comp_name, st.shm_name, st.xdg_name);
        return 1;
    }

    /* 2. Bind wl_compositor, wl_shm, xdg_wm_base */
    uint32_t comp_ver = st.comp_ver < 4 ? st.comp_ver : 4;
    uint32_t xdg_ver = st.xdg_ver < 2 ? st.xdg_ver : 2;
    if (send_registry_bind(sock, st.comp_name, "wl_compositor", comp_ver, ID_COMPOSITOR) < 0) return 1;
    if (send_registry_bind(sock, st.shm_name, "wl_shm", 1, ID_SHM) < 0) return 1;
    if (send_registry_bind(sock, st.xdg_name, "xdg_wm_base", xdg_ver, ID_XDG_WM_BASE) < 0) return 1;

    /* 3. Create wl_surface + xdg_surface + xdg_toplevel, initial commit */
    if (send_u1(sock, ID_COMPOSITOR, 0 /* create_surface */, ID_SURFACE) < 0) return 1;
    if (send_u2(sock, ID_XDG_WM_BASE, 2 /* get_xdg_surface */, ID_XDG_SURFACE, ID_SURFACE) < 0) return 1;
    if (send_u1(sock, ID_XDG_SURFACE, 1 /* get_toplevel */, ID_XDG_TOPLEVEL) < 0) return 1;
    if (send_string_req(sock, ID_XDG_TOPLEVEL, 2 /* set_title */, "shm_fill") < 0) return 1;
    if (send_u0(sock, ID_SURFACE, 6 /* commit */) < 0) return 1;

    deadline = now_us() + 5000000ull;
    while (!st.configured) {
        if (now_us() > deadline) {
            LOGE("timeout waiting for xdg_surface.configure");
            return 1;
        }
        if (pump_events(sock, &st, 100) < 0) return 1;
    }

    /* 4. Allocate 512x512 SHM buffer and fill with 0xFF00FF00 (green) */
    int shm_fd = create_shm_fd(POOL_SIZE);
    if (shm_fd < 0) return 1;

    uint32_t *pixels = (uint32_t *)mmap(NULL, POOL_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (pixels == MAP_FAILED) {
        LOGE("mmap failed: %s", strerror(errno));
        close(shm_fd);
        return 1;
    }
    for (size_t i = 0; i < (size_t)(WIDTH * HEIGHT); i++) {
        pixels[i] = FILL_PIXEL;
    }

    uint32_t pool_req[4] = { ID_SHM, wl_hdr(16, 0 /* create_pool */), ID_SHM_POOL, POOL_SIZE };
    if (send_with_fd(sock, pool_req, sizeof(pool_req), shm_fd) < 0) return 1;
    close(shm_fd);

    uint32_t buf_req[8] = {
        ID_SHM_POOL,
        wl_hdr(32, 0 /* create_buffer */),
        ID_BUFFER,
        0u,
        WIDTH,
        HEIGHT,
        STRIDE,
        WL_SHM_FORMAT_ARGB8888
    };
    if (send_all(sock, buf_req, sizeof(buf_req)) < 0) return 1;

    /* 5. Attach, damage, request frame callback, commit */
    uint32_t next_id = ID_NEXT_START;
    uint64_t t_commit = now_us();
    st.wait_cb_id = next_id++;
    st.frame_done = 0;
    if (send_u3(sock, ID_SURFACE, 1 /* attach */, ID_BUFFER, 0, 0) < 0) return 1;
    if (send_u4(sock, ID_SURFACE, 2 /* damage */, 0, 0, WIDTH, HEIGHT) < 0) return 1;
    if (send_u1(sock, ID_SURFACE, 3 /* frame */, st.wait_cb_id) < 0) return 1;
    if (send_u0(sock, ID_SURFACE, 6 /* commit */) < 0) return 1;

    deadline = now_us() + 5000000ull;
    while (!st.frame_done) {
        if (now_us() > deadline) {
            LOGE("timeout waiting for first frame callback");
            break;
        }
        if (pump_events(sock, &st, 100) < 0) return 1;
    }
    uint64_t first_us = now_us() - t_commit;
    LOGI("committed %dx%d green (0x%08X), first_frame_rtt=%lluus",
         WIDTH, HEIGHT, FILL_PIXEL, (unsigned long long)first_us);

    /* Optional commit-loop benchmark if --bench N was passed */
    if (bench_frames > 0) {
        uint64_t t0 = now_us();
        for (int f = 0; f < bench_frames; f++) {
            st.wait_cb_id = next_id++;
            st.frame_done = 0;
            if (send_u3(sock, ID_SURFACE, 1, ID_BUFFER, 0, 0) < 0) return 1;
            if (send_u4(sock, ID_SURFACE, 2, 0, 0, WIDTH, HEIGHT) < 0) return 1;
            if (send_u1(sock, ID_SURFACE, 3, st.wait_cb_id) < 0) return 1;
            if (send_u0(sock, ID_SURFACE, 6) < 0) return 1;
            while (!st.frame_done) {
                if (pump_events(sock, &st, 1000) < 0) return 1;
            }
        }
        uint64_t dt_us = now_us() - t0;
        double fps = dt_us > 0 ? ((double)bench_frames * 1e6 / (double)dt_us) : 0.0;
        double avg_us = (double)dt_us / (double)bench_frames;
        LOGI("BENCH frames=%d total_ms=%.2f fps=%.1f avg_frame_us=%.1f",
             bench_frames, (double)dt_us / 1000.0, fps, avg_us);
    }

    /* 6. Stay alive for hold_ms (default 10s), servicing pings/events */
    uint64_t hold_until = now_us() + (uint64_t)hold_ms * 1000ull;
    while (now_us() < hold_until) {
        uint64_t rem_ms = (hold_until - now_us()) / 1000ull;
        int wait = rem_ms > 250 ? 250 : (int)rem_ms;
        if (wait <= 0) break;
        if (pump_events(sock, &st, wait) < 0) break;
    }

    munmap(pixels, POOL_SIZE);
    close(sock);
    LOGI("done");
    return 0;
}
