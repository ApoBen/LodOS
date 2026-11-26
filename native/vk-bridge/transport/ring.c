#define _GNU_SOURCE
#include "ring.h"
#include <errno.h>
#include <limits.h>
#include <linux/futex.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#define RING_MAGIC 0x56424b52u /* 'VBKR' */
#define RING_WRAP 0xffffffffu
#ifndef SPIN_ITERS
#define SPIN_ITERS 20000 /* ~tens of us; phone futex wake ~100us. power tradeoff, tune later */
#endif

struct ring_hdr {
    uint32_t magic, cap;
    _Alignas(64) _Atomic uint64_t head;      /* producer write pos (monotonic) */
    _Atomic uint32_t data_seq;               /* futex: bumped on push */
    _Atomic uint32_t data_waiters;
    _Alignas(64) _Atomic uint64_t tail;      /* consumer read pos (monotonic) */
    _Atomic uint32_t space_seq;              /* futex: bumped on pop */
    _Atomic uint32_t space_waiters;
};
_Static_assert(sizeof(struct ring_hdr) <= RING_HDR_SIZE, "hdr too big");

static int futex_wait(_Atomic uint32_t *a, uint32_t v) {
    return (int)syscall(SYS_futex, (uint32_t *)a, FUTEX_WAIT, v, NULL, NULL, 0);
}
static void futex_wake(_Atomic uint32_t *a) {
    syscall(SYS_futex, (uint32_t *)a, FUTEX_WAKE, INT_MAX, NULL, NULL, 0);
}
static inline void cpu_relax(void) {
#if defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield");
#endif
}

static int map_fd(ring *r, int fd, uint32_t cap) {
    size_t len = RING_HDR_SIZE + (size_t)cap;
    void *p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) return -errno;
    r->h = p;
    r->data = (uint8_t *)p + RING_HDR_SIZE;
    r->cap = cap;
    r->map_len = len;
    r->fd = fd;
    return 0;
}

int ring_create(ring *r, uint32_t cap) {
    if (!cap) cap = RING_DEFAULT_CAP;
    if (cap < 4096 || (cap & (cap - 1))) return -EINVAL;
    int fd = (int)syscall(SYS_memfd_create, "vkb-ring", 1u /*MFD_CLOEXEC*/);
    if (fd < 0) return -errno;
    if (ftruncate(fd, (off_t)RING_HDR_SIZE + cap) < 0) { int e = -errno; close(fd); return e; }
    int rc = map_fd(r, fd, cap);
    if (rc) { close(fd); return rc; }
    memset(r->h, 0, sizeof(*r->h));
    r->h->cap = cap;
    atomic_store(&r->h->head, 0);
    atomic_store(&r->h->tail, 0);
    __atomic_store_n(&r->h->magic, RING_MAGIC, __ATOMIC_RELEASE);
    return 0;
}

int ring_attach(ring *r, int fd) {
    struct stat st;
    if (fstat(fd, &st) < 0) return -errno;
    if ((size_t)st.st_size <= RING_HDR_SIZE) return -EINVAL;
    uint64_t cap = (uint64_t)st.st_size - RING_HDR_SIZE;
    if (cap > UINT32_MAX || (cap & (cap - 1))) return -EINVAL;
    int rc = map_fd(r, fd, (uint32_t)cap);
    if (rc) return rc;
    if (__atomic_load_n(&r->h->magic, __ATOMIC_ACQUIRE) != RING_MAGIC || r->h->cap != cap) {
        munmap(r->h, r->map_len);
        return -EINVAL;
    }
    return 0;
}

void ring_destroy(ring *r) {
    if (r->h) munmap(r->h, r->map_len);
    if (r->fd >= 0) close(r->fd);
    r->h = NULL;
    r->fd = -1;
}

uint32_t ring_max_msg(const ring *r) { return r->cap / 2 - 8; }

static inline uint32_t msg_total(uint32_t len) { return (4 + len + 7) & ~7u; }

/* wait until *cond_pos changes from 'seen' (or predicate true), using seq futex */
static void wait_on(_Atomic uint32_t *seq, _Atomic uint32_t *waiters,
                    _Atomic uint64_t *pos, uint64_t seen) {
    for (int i = 0; i < SPIN_ITERS; i++) {
        if (atomic_load_explicit(pos, memory_order_acquire) != seen) return;
        cpu_relax();
    }
    for (;;) {
        uint32_t s = atomic_load(seq);
        atomic_store(waiters, 1);
        if (atomic_load(pos) != seen) break;
        futex_wait(seq, s);
        if (atomic_load(pos) != seen) break;
    }
    atomic_store(waiters, 0);
}

static void notify(_Atomic uint32_t *seq, _Atomic uint32_t *waiters) {
    atomic_fetch_add(seq, 1);
    if (atomic_load(waiters)) futex_wake(seq);
}

int ring_push(ring *r, const void *msg, uint32_t len) {
    if (len > ring_max_msg(r)) return -EMSGSIZE;
    ring_hdr *h = r->h;
    uint64_t head = atomic_load_explicit(&h->head, memory_order_relaxed);
    uint32_t idx = (uint32_t)head & (r->cap - 1);
    uint32_t total = msg_total(len);
    uint32_t to_end = r->cap - idx;
    uint32_t need = total <= to_end ? total : to_end + total;
    for (;;) {
        uint64_t tail = atomic_load_explicit(&h->tail, memory_order_acquire);
        if (r->cap - (head - tail) >= need) break;
        wait_on(&h->space_seq, &h->space_waiters, &h->tail, tail);
    }
    if (total > to_end) {
        uint32_t w = RING_WRAP;
        memcpy(r->data + idx, &w, 4);
        head += to_end;
        idx = 0;
    }
    memcpy(r->data + idx, &len, 4);
    memcpy(r->data + idx + 4, msg, len);
    atomic_store(&h->head, head + total); /* seq_cst: pairs with waiter check */
    notify(&h->data_seq, &h->data_waiters);
    return 0;
}

static int64_t pop_impl(ring *r, void *buf, uint32_t buflen, int block) {
    ring_hdr *h = r->h;
    uint64_t tail = atomic_load_explicit(&h->tail, memory_order_relaxed);
    for (;;) {
        uint64_t head = atomic_load_explicit(&h->head, memory_order_acquire);
        if (head != tail) break;
        if (!block) return -EAGAIN;
        wait_on(&h->data_seq, &h->data_waiters, &h->head, head);
    }
    uint32_t idx = (uint32_t)tail & (r->cap - 1);
    uint32_t len;
    memcpy(&len, r->data + idx, 4);
    if (len == RING_WRAP) {
        tail += r->cap - idx;
        idx = 0;
        memcpy(&len, r->data, 4);
        /* publish skip so producer sees consistent state even if we bail */
        atomic_store(&h->tail, tail);
    }
    if (len > buflen) return -EMSGSIZE;
    memcpy(buf, r->data + idx + 4, len);
    atomic_store(&h->tail, tail + msg_total(len));
    notify(&h->space_seq, &h->space_waiters);
    return len;
}

int64_t ring_pop(ring *r, void *buf, uint32_t buflen) { return pop_impl(r, buf, buflen, 1); }
int64_t ring_trypop(ring *r, void *buf, uint32_t buflen) { return pop_impl(r, buf, buflen, 0); }
