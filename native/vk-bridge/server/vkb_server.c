/* vk-bridge server v0/v1: listens on AF_UNIX @lodos-vk (R13: shell uid can't bind path sockets),
 * one thread per client. Client sends "VKB1" + 2 ring fds (c2s, s2c); server loops
 * ring_pop -> vkr_srv_dispatch (decode, call real driver, encode) -> ring_push.
 * Mapped memory (v0 shadow): host vkAllocateMemory+vkMapMemory paired with a memfd shared with the
 * client via SCM_RIGHTS on c->sock; bytes synced on map/flush/invalidate/unmap + submit/wait.
 * usage: vkb_server [-s name] [-l libpath] */
#define _GNU_SOURCE
#include "vkb_rpc.h"
#include "../transport/ring.h"
#include "../transport/sock.h"
#include <dlfcn.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

#define ARENA_SZ (8u << 20)

static void *g_lib;
static PFN_vkGetInstanceProcAddr g_gipa;
static PFN_vkGetDeviceProcAddr g_gdpa;
static vkr_srv_fns g_real_fns;
static vkr_srv_fns g_fns;
static pthread_mutex_t g_load_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_missing_fns = 1;
#ifdef VKB_CMD_HAS_vkMapMemory
static PFN_vkMapMemory g_real_vkMapMemory;
#endif

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

#ifdef VKB_CMD_HAS_vkMapMemory
typedef struct {
    VkDevice dev;
    VkDeviceMemory mem;
    VkDeviceSize alloc_size;
    void *host_ptr;
    void *shm_ptr;
    size_t shm_map_len;
    VkDeviceSize map_offset;
    VkDeviceSize map_size;
} srv_mem;
#endif

typedef struct {
    int sock;
    ring c2s, s2c;
    atomic_int done;
    pthread_t watcher;
#ifdef VKB_CMD_HAS_vkMapMemory
    srv_mem *mems;
    uint32_t nmems, capmems;
#endif
} client;

static _Thread_local client *t_cli;

static void *getproc(const char *name, void *ud) {
    VkInstance inst = (VkInstance)ud;
    void *p = inst ? NULL : dlsym(g_lib, name);
    if (!p && g_gipa) p = (void *)g_gipa(inst, name);
    if (!p && g_gipa && inst) {
        static const char *const sfx[] = { "KHR", "EXT" };
        for (size_t i = 0; i < 2 && !p; i++) {
            char alt[96];
            int n = snprintf(alt, sizeof alt, "%s%s", name, sfx[i]);
            if (n > 0 && (size_t)n < sizeof alt) p = (void *)g_gipa(inst, alt);
        }
    }
    return p;
}

static void *getdevproc(const char *name, void *ud) {
    VkDevice dev = (VkDevice)ud;
    if (!dev || !g_gdpa) return NULL;
    void *p = (void *)g_gdpa(dev, name);
    if (!p) {
        static const char *const sfx[] = { "KHR", "EXT" };
        for (size_t i = 0; i < 2 && !p; i++) {
            char alt[96];
            int n = snprintf(alt, sizeof alt, "%s%s", name, sfx[i]);
            if (n > 0 && (size_t)n < sizeof alt) p = (void *)g_gdpa(dev, alt);
        }
    }
    return p;
}

static void srv_install_wrappers(void);

#ifdef VKB_CMD_HAS_vkMapMemory
static srv_mem *srv_mem_find(client *c, VkDevice dev, VkDeviceMemory mem) {
    if (!c || !mem) return NULL;
    for (uint32_t i = 0; i < c->nmems; i++)
        if (c->mems[i].mem == mem && c->mems[i].dev == dev) return &c->mems[i];
    return NULL;
}

static void srv_sync_range(srv_mem *m, VkDeviceSize roff, VkDeviceSize rsz, int to_host) {
    if (!m || !m->shm_ptr || !m->host_ptr || !m->map_size) return;
    if (rsz == VK_WHOLE_SIZE) rsz = roff <= m->alloc_size ? m->alloc_size - roff : 0;
    VkDeviceSize lo = roff > m->map_offset ? roff : m->map_offset;
    VkDeviceSize hi_r = roff + rsz, hi_m = m->map_offset + m->map_size;
    VkDeviceSize hi = hi_r < hi_m ? hi_r : hi_m;
    if (hi <= lo) return;
    size_t off = (size_t)(lo - m->map_offset), len = (size_t)(hi - lo);
    if (to_host) memcpy((uint8_t *)m->host_ptr + off, (const uint8_t *)m->shm_ptr + off, len);
    else memcpy((uint8_t *)m->shm_ptr + off, (const uint8_t *)m->host_ptr + off, len);
}

static void srv_sync_all(client *c, int to_host) {
    if (!c) return;
    for (uint32_t i = 0; i < c->nmems; i++) {
        srv_mem *m = &c->mems[i];
        if (m->shm_ptr && m->host_ptr && m->map_size) {
            if (to_host) memcpy(m->host_ptr, m->shm_ptr, (size_t)m->map_size);
            else memcpy(m->shm_ptr, m->host_ptr, (size_t)m->map_size);
        }
    }
}

static VKAPI_ATTR VkResult VKAPI_CALL srv_wrap_vkAllocateMemory(
    VkDevice dev, const VkMemoryAllocateInfo *pInfo,
    const VkAllocationCallbacks *pAlloc, VkDeviceMemory *pMem) {
    client *c = t_cli;
    int imp_fd = -1;
    if (c && pInfo) {
        for (VkBaseOutStructure *n = (VkBaseOutStructure *)(uintptr_t)pInfo->pNext; n; n = n->pNext) {
            if (n->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR) {
                VkImportMemoryFdInfoKHR *imp = (VkImportMemoryFdInfoKHR *)n;
                if (imp->handleType != 0 && imp->fd >= 0) {
                    char tag = 0; int fds[SOCK_MAX_FDS], nf = 0;
                    ssize_t sn = sock_recv(c->sock, &tag, 1, fds, &nf);
                    if (sn != 1 || tag != 'I' || nf != 1) {
                        for (int i = 0; i < nf; i++) close(fds[i]);
                        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
                    }
                    imp_fd = fds[0];
                    imp->fd = imp_fd;
                }
                break;
            }
        }
    }
    VkResult r = g_real_fns.vkAllocateMemory(dev, pInfo, pAlloc, pMem);
    if (r != VK_SUCCESS && imp_fd >= 0) close(imp_fd);
    if (r == VK_SUCCESS && c && pMem && *pMem) {
        srv_mem *m = srv_mem_find(c, dev, *pMem);
        if (!m) {
            for (uint32_t i = 0; i < c->nmems; i++)
                if (!c->mems[i].mem) { m = &c->mems[i]; break; }
        }
        if (!m) {
            if (c->nmems == c->capmems) {
                uint32_t nc = c->capmems ? c->capmems * 2 : 16;
                srv_mem *nm = realloc(c->mems, nc * sizeof *nm);
                if (!nm) abort();
                c->mems = nm; c->capmems = nc;
            }
            m = &c->mems[c->nmems++];
        }
        *m = (srv_mem){ .dev = dev, .mem = *pMem, .alloc_size = pInfo ? pInfo->allocationSize : 0 };
    }
    return r;
}

static VKAPI_ATTR void VKAPI_CALL srv_wrap_vkUnmapMemory(VkDevice dev, VkDeviceMemory mem) {
    srv_mem *m = srv_mem_find(t_cli, dev, mem);
    if (m && m->shm_ptr) {
        if (m->host_ptr && m->map_size) memcpy(m->host_ptr, m->shm_ptr, (size_t)m->map_size);
        munmap(m->shm_ptr, m->shm_map_len);
        m->shm_ptr = NULL; m->host_ptr = NULL;
        m->shm_map_len = 0; m->map_offset = 0; m->map_size = 0;
    }
    g_real_fns.vkUnmapMemory(dev, mem);
}

static VKAPI_ATTR void VKAPI_CALL srv_wrap_vkFreeMemory(
    VkDevice dev, VkDeviceMemory mem, const VkAllocationCallbacks *pAlloc) {
    srv_mem *m = srv_mem_find(t_cli, dev, mem);
    if (m) {
        if (m->shm_ptr) munmap(m->shm_ptr, m->shm_map_len);
        *m = (srv_mem){ 0 };
    }
    g_real_fns.vkFreeMemory(dev, mem, pAlloc);
}

static VKAPI_ATTR void VKAPI_CALL srv_wrap_vkDestroyDevice(
    VkDevice dev, const VkAllocationCallbacks *pAlloc) {
    client *c = t_cli;
    if (c) {
        for (uint32_t i = 0; i < c->nmems; i++) {
            if (c->mems[i].dev == dev) {
                if (c->mems[i].shm_ptr) munmap(c->mems[i].shm_ptr, c->mems[i].shm_map_len);
                c->mems[i] = (srv_mem){ 0 };
            }
        }
    }
    g_real_fns.vkDestroyDevice(dev, pAlloc);
}

static VKAPI_ATTR VkResult VKAPI_CALL srv_wrap_vkFlushMappedMemoryRanges(
    VkDevice dev, uint32_t count, const VkMappedMemoryRange *pRanges) {
    if (pRanges)
        for (uint32_t i = 0; i < count; i++)
            srv_sync_range(srv_mem_find(t_cli, dev, pRanges[i].memory), pRanges[i].offset, pRanges[i].size, 1);
    return g_real_fns.vkFlushMappedMemoryRanges(dev, count, pRanges);
}

static VKAPI_ATTR VkResult VKAPI_CALL srv_wrap_vkInvalidateMappedMemoryRanges(
    VkDevice dev, uint32_t count, const VkMappedMemoryRange *pRanges) {
    VkResult r = g_real_fns.vkInvalidateMappedMemoryRanges(dev, count, pRanges);
    if (r >= 0 && pRanges)
        for (uint32_t i = 0; i < count; i++)
            srv_sync_range(srv_mem_find(t_cli, dev, pRanges[i].memory), pRanges[i].offset, pRanges[i].size, 0);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL srv_wrap_vkQueueSubmit(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence) {
    srv_sync_all(t_cli, 1);
    return g_real_fns.vkQueueSubmit(queue, submitCount, pSubmits, fence);
}

static VKAPI_ATTR VkResult VKAPI_CALL srv_wrap_vkQueueWaitIdle(VkQueue queue) {
    VkResult r = g_real_fns.vkQueueWaitIdle(queue);
    if (r == VK_SUCCESS) srv_sync_all(t_cli, 0);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL srv_wrap_vkDeviceWaitIdle(VkDevice dev) {
    VkResult r = g_real_fns.vkDeviceWaitIdle(dev);
    if (r == VK_SUCCESS) srv_sync_all(t_cli, 0);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL srv_wrap_vkWaitForFences(
    VkDevice dev, uint32_t count, const VkFence *pFences, VkBool32 waitAll, uint64_t timeout) {
    VkResult r = g_real_fns.vkWaitForFences(dev, count, pFences, waitAll, timeout);
    if (r == VK_SUCCESS) srv_sync_all(t_cli, 0);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL srv_wrap_vkGetFenceStatus(VkDevice dev, VkFence fence) {
    VkResult r = g_real_fns.vkGetFenceStatus(dev, fence);
    if (r == VK_SUCCESS) srv_sync_all(t_cli, 0);
    return r;
}

#ifdef VKB_HAS_vkQueueSubmit2
static VKAPI_ATTR VkResult VKAPI_CALL srv_wrap_vkQueueSubmit2(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo2 *pSubmits, VkFence fence) {
    srv_sync_all(t_cli, 1);
    return g_real_fns.vkQueueSubmit2(queue, submitCount, pSubmits, fence);
}
#endif

#ifdef VKB_HAS_vkWaitSemaphores
static VKAPI_ATTR VkResult VKAPI_CALL srv_wrap_vkWaitSemaphores(
    VkDevice dev, const VkSemaphoreWaitInfo *pWaitInfo, uint64_t timeout) {
    VkResult r = g_real_fns.vkWaitSemaphores(dev, pWaitInfo, timeout);
    if (r == VK_SUCCESS) srv_sync_all(t_cli, 0);
    return r;
}
#endif

#ifdef VKB_HAS_vkGetSemaphoreCounterValue
static VKAPI_ATTR VkResult VKAPI_CALL srv_wrap_vkGetSemaphoreCounterValue(
    VkDevice dev, VkSemaphore semaphore, uint64_t *pValue) {
    VkResult r = g_real_fns.vkGetSemaphoreCounterValue(dev, semaphore, pValue);
    if (r == VK_SUCCESS) srv_sync_all(t_cli, 0);
    return r;
}
#endif

#ifdef VKB_HAS_vkGetMemoryFdKHR
static VKAPI_ATTR VkResult VKAPI_CALL srv_wrap_vkGetMemoryFdKHR(
    VkDevice dev, const VkMemoryGetFdInfoKHR *pGetFdInfo, int *pFd) {
    client *c = t_cli;
    if (!c || !pFd) return VK_ERROR_INITIALIZATION_FAILED;
    int host_fd = -1;
    VkResult r = g_real_fns.vkGetMemoryFdKHR(dev, pGetFdInfo, &host_fd);
    if (r == VK_SUCCESS) {
        char tag = 'F';
        ssize_t sn = sock_send(c->sock, &tag, 1, &host_fd, 1);
        close(host_fd);
        if (sn != 1) return VK_ERROR_OUT_OF_HOST_MEMORY;
        *pFd = 0;
    }
    return r;
}
#endif

#ifdef VKB_HAS_vkGetMemoryFdPropertiesKHR
static VKAPI_ATTR VkResult VKAPI_CALL srv_wrap_vkGetMemoryFdPropertiesKHR(
    VkDevice dev, VkExternalMemoryHandleTypeFlagBits handleType,
    int fd, VkMemoryFdPropertiesKHR *pProps) {
    client *c = t_cli;
    if (!c || fd < 0) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    char tag = 0; int fds[SOCK_MAX_FDS], nf = 0;
    ssize_t sn = sock_recv(c->sock, &tag, 1, fds, &nf);
    if (sn != 1 || tag != 'P' || nf != 1) {
        for (int i = 0; i < nf; i++) close(fds[i]);
        return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    }
    VkResult r = g_real_fns.vkGetMemoryFdPropertiesKHR(dev, handleType, fds[0], pProps);
    close(fds[0]);
    return r;
}
#endif

#ifdef VKB_HAS_vkGetEventStatus
static VKAPI_ATTR VkResult VKAPI_CALL srv_wrap_vkGetEventStatus(VkDevice dev, VkEvent event) {
    VkResult r = g_real_fns.vkGetEventStatus(dev, event);
    if (r == VK_EVENT_SET) srv_sync_all(t_cli, 0);
    return r;
}
#endif

static size_t srv_vkMapMemory(client *c, htab *ht, const uint8_t *req, size_t len, uint8_t *rep, size_t cap) {
    vkr_rd r = { req, len, 0, 0, NULL, ht };
    vkr_wr w = { rep, cap, 0, 0, ht };
    (void)vkr_get_u32(&r);
    int ok1 = 1, ok2 = 1;
    VkDevice dev = (VkDevice)(uintptr_t)vkr_h_in(ht, VK_OBJECT_TYPE_DEVICE, vkr_get_u64(&r), &ok1);
    VkDeviceMemory mem = (VkDeviceMemory)(uintptr_t)vkr_h_in(ht, VK_OBJECT_TYPE_DEVICE_MEMORY, vkr_get_u64(&r), &ok2);
    VkDeviceSize offset = 0, size = 0;
    VkMemoryMapFlags flags = 0;
    vkr_get(&r, &offset, sizeof offset);
    vkr_get(&r, &size, sizeof size);
    vkr_get(&r, &flags, sizeof flags);
    vkr_put_u32(&w, VKB_CMD_vkMapMemory);
    if (r.err || !ok1 || !ok2 || !g_real_vkMapMemory) {
        VkResult e = (!ok1 || !ok2 || r.err) ? VK_ERROR_UNKNOWN : VK_ERROR_FEATURE_NOT_PRESENT;
        vkr_put(&w, &e, sizeof e);
        return w.err ? 0 : w.off;
    }
    srv_mem *m = srv_mem_find(c, dev, mem);
    if (!m || m->shm_ptr || offset > m->alloc_size || (size != VK_WHOLE_SIZE && size > m->alloc_size - offset)) {
        VkResult e = VK_ERROR_MEMORY_MAP_FAILED;
        vkr_put(&w, &e, sizeof e);
        return w.err ? 0 : w.off;
    }
    VkDeviceSize actual = (size == VK_WHOLE_SIZE) ? (m->alloc_size - offset) : size;
    void *host_ptr = NULL;
    VkResult res = g_real_vkMapMemory(dev, mem, offset, size, flags, &host_ptr);
    if (res < 0) {
        vkr_put(&w, &res, sizeof res);
        return w.err ? 0 : w.off;
    }
    size_t mlen = actual ? (size_t)actual : 1;
    int fd = (int)syscall(SYS_memfd_create, "vkb-mem", 1u /*MFD_CLOEXEC*/);
    void *shm_ptr = MAP_FAILED;
    if (fd < 0 || ftruncate(fd, (off_t)mlen) < 0 ||
        (shm_ptr = mmap(NULL, mlen, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
        if (fd >= 0) close(fd);
        if (g_real_fns.vkUnmapMemory) g_real_fns.vkUnmapMemory(dev, mem);
        VkResult e = VK_ERROR_OUT_OF_HOST_MEMORY;
        vkr_put(&w, &e, sizeof e);
        return w.err ? 0 : w.off;
    }
    if (actual && host_ptr) memcpy(shm_ptr, host_ptr, (size_t)actual);
    char tag = 'M';
    ssize_t sn = sock_send(c->sock, &tag, 1, &fd, 1);
    close(fd);
    if (sn != 1) {
        munmap(shm_ptr, mlen);
        if (g_real_fns.vkUnmapMemory) g_real_fns.vkUnmapMemory(dev, mem);
        VkResult e = VK_ERROR_MEMORY_MAP_FAILED;
        vkr_put(&w, &e, sizeof e);
        return w.err ? 0 : w.off;
    }
    m->host_ptr = host_ptr;
    m->shm_ptr = shm_ptr;
    m->shm_map_len = mlen;
    m->map_offset = offset;
    m->map_size = actual;
    vkr_put(&w, &res, sizeof res);
    vkr_put_u64(&w, (uint64_t)actual);
    return w.err ? 0 : w.off;
}
#endif /* VKB_CMD_HAS_vkMapMemory */

#ifdef VKB_HAS_vkCreateInstance
static VKAPI_ATTR VkResult VKAPI_CALL srv_wrap_vkCreateInstance(
    const VkInstanceCreateInfo *pCI, const VkAllocationCallbacks *pAlloc, VkInstance *pInst) {
    VkResult r = g_real_fns.vkCreateInstance(pCI, pAlloc, pInst);
    if (r == VK_SUCCESS && pInst && *pInst && g_gipa) {
        pthread_mutex_lock(&g_load_mu);
        if (!g_gdpa) g_gdpa = (PFN_vkGetDeviceProcAddr)g_gipa(*pInst, "vkGetDeviceProcAddr");
        if (g_missing_fns > 0) {
            g_missing_fns = vkr_srv_load(&g_real_fns, getproc, *pInst);
#ifdef VKB_CMD_HAS_vkMapMemory
            if (!g_real_vkMapMemory)
                g_real_vkMapMemory = (PFN_vkMapMemory)getproc("vkMapMemory", *pInst);
#endif
            srv_install_wrappers();
        }
        pthread_mutex_unlock(&g_load_mu);
    }
    return r;
}
#endif

#ifdef VKB_HAS_vkCreateDevice
static VKAPI_ATTR VkResult VKAPI_CALL srv_wrap_vkCreateDevice(
    VkPhysicalDevice pd, const VkDeviceCreateInfo *pCI,
    const VkAllocationCallbacks *pAlloc, VkDevice *pDev) {
    VkResult r = g_real_fns.vkCreateDevice(pd, pCI, pAlloc, pDev);
    if (r == VK_SUCCESS && pDev && *pDev && g_gdpa) {
        pthread_mutex_lock(&g_load_mu);
        if (g_missing_fns > 0) {
            g_missing_fns = vkr_srv_load(&g_real_fns, getdevproc, *pDev);
            srv_install_wrappers();
        }
        pthread_mutex_unlock(&g_load_mu);
    }
    return r;
}
#endif

static void srv_install_wrappers(void) {
    vkr_srv_fns tmp = g_real_fns;
#ifdef VKB_HAS_vkCreateInstance
    if (g_real_fns.vkCreateInstance) tmp.vkCreateInstance = srv_wrap_vkCreateInstance;
#endif
#ifdef VKB_HAS_vkCreateDevice
    if (g_real_fns.vkCreateDevice) tmp.vkCreateDevice = srv_wrap_vkCreateDevice;
#endif
#ifdef VKB_CMD_HAS_vkMapMemory
    if (g_real_fns.vkDestroyDevice) tmp.vkDestroyDevice = srv_wrap_vkDestroyDevice;
    if (g_real_fns.vkAllocateMemory) tmp.vkAllocateMemory = srv_wrap_vkAllocateMemory;
    if (g_real_fns.vkFreeMemory) tmp.vkFreeMemory = srv_wrap_vkFreeMemory;
    if (g_real_fns.vkUnmapMemory) tmp.vkUnmapMemory = srv_wrap_vkUnmapMemory;
    if (g_real_fns.vkFlushMappedMemoryRanges) tmp.vkFlushMappedMemoryRanges = srv_wrap_vkFlushMappedMemoryRanges;
    if (g_real_fns.vkInvalidateMappedMemoryRanges) tmp.vkInvalidateMappedMemoryRanges = srv_wrap_vkInvalidateMappedMemoryRanges;
    if (g_real_fns.vkQueueSubmit) tmp.vkQueueSubmit = srv_wrap_vkQueueSubmit;
    if (g_real_fns.vkQueueWaitIdle) tmp.vkQueueWaitIdle = srv_wrap_vkQueueWaitIdle;
    if (g_real_fns.vkDeviceWaitIdle) tmp.vkDeviceWaitIdle = srv_wrap_vkDeviceWaitIdle;
    if (g_real_fns.vkWaitForFences) tmp.vkWaitForFences = srv_wrap_vkWaitForFences;
    if (g_real_fns.vkGetFenceStatus) tmp.vkGetFenceStatus = srv_wrap_vkGetFenceStatus;
#ifdef VKB_HAS_vkQueueSubmit2
    if (g_real_fns.vkQueueSubmit2) tmp.vkQueueSubmit2 = srv_wrap_vkQueueSubmit2;
#endif
#ifdef VKB_HAS_vkWaitSemaphores
    if (g_real_fns.vkWaitSemaphores) tmp.vkWaitSemaphores = srv_wrap_vkWaitSemaphores;
#endif
#ifdef VKB_HAS_vkGetSemaphoreCounterValue
    if (g_real_fns.vkGetSemaphoreCounterValue) tmp.vkGetSemaphoreCounterValue = srv_wrap_vkGetSemaphoreCounterValue;
#endif
#ifdef VKB_HAS_vkGetEventStatus
    if (g_real_fns.vkGetEventStatus) tmp.vkGetEventStatus = srv_wrap_vkGetEventStatus;
#endif
#ifdef VKB_HAS_vkGetMemoryFdKHR
    if (g_real_fns.vkGetMemoryFdKHR) tmp.vkGetMemoryFdKHR = srv_wrap_vkGetMemoryFdKHR;
#endif
#ifdef VKB_HAS_vkGetMemoryFdPropertiesKHR
    if (g_real_fns.vkGetMemoryFdPropertiesKHR) tmp.vkGetMemoryFdPropertiesKHR = srv_wrap_vkGetMemoryFdPropertiesKHR;
#endif
#endif
    g_fns = tmp;
}

/* socket EOF = client gone (crash or close): unblock the worker by pushing BYE into c2s.
 * Uses poll(POLLRDHUP) rather than sock_recv so client->server SCM_RIGHTS messages are never consumed here. */
static void *watch_fn(void *arg) {
    client *c = arg;
    for (;;) {
        struct pollfd pfd = { .fd = c->sock, .events = POLLRDHUP };
        int rc = poll(&pfd, 1, -1);
        if (rc < 0 && errno == EINTR) continue;
        if (rc < 0 || (pfd.revents & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL))) break;
    }
    if (!atomic_load(&c->done)) { uint32_t bye = VKR_CMD_BYE; ring_push(&c->c2s, &bye, 4); }
    return NULL;
}

static void *client_fn(void *arg) {
    client *c = arg;
    t_cli = c;
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
    const char *tr_env = getenv("LODOS_VK_TRACE");
    int trace = tr_env && tr_env[0] == '1';
    for (;;) {
        int64_t len = ring_pop(&c->c2s, req, reqcap);
        if (len < 0) { fprintf(stderr, "vkb_server: ring_pop %lld\n", (long long)len); break; }
        uint32_t cmd = 0;
        if (len >= 4) memcpy(&cmd, req, 4);
        if (cmd == VKR_CMD_BYE) break;
        vkr_arena a = { arena_mem, ARENA_SZ, 0 };
#ifdef VKB_CMD_HAS_vkMapMemory
        size_t rl = (cmd == VKB_CMD_vkMapMemory)
            ? srv_vkMapMemory(c, &ht, req, (size_t)len, rep, repcap)
            : vkr_srv_dispatch(&g_fns, &ht, &a, req, (size_t)len, rep, repcap);
#else
        size_t rl = vkr_srv_dispatch(&g_fns, &ht, &a, req, (size_t)len, rep, repcap);
#endif
        if (!rl) { /* unknown cmd / malformed header: reply cmd + VK_ERROR_FEATURE_NOT_PRESENT */
            VkResult e = VK_ERROR_FEATURE_NOT_PRESENT;
            memcpy(rep, &cmd, 4); memcpy(rep + 4, &e, 4); rl = 8;
        }
        if (trace) {
            int32_t rres = 0;
            if (rl >= 8) memcpy(&rres, rep + 4, 4);
            fprintf(stderr, "vkb_trace[%llu]: %s (%u) -> %d (rl=%zu)\n",
                    (unsigned long long)ncalls,
                    cmd < VKB_CMD_COUNT ? vkb_cmd_names[cmd] : "?", cmd, (int)rres, rl);
        }
        ring_push(&c->s2c, rep, (uint32_t)rl);
        ncalls++;
    }
    atomic_store(&c->done, 1);
    shutdown(c->sock, SHUT_RDWR);
    pthread_join(c->watcher, NULL);
#ifdef VKB_CMD_HAS_vkMapMemory
    for (uint32_t i = 0; i < c->nmems; i++)
        if (c->mems[i].shm_ptr) munmap(c->mems[i].shm_ptr, c->mems[i].shm_map_len);
    free(c->mems);
#endif
    /* leaked devices/instances of a dead/sloppy client */
#ifdef VKB_HAS_vkDestroyDevice
    for (uint32_t i = 0; i < ht.n; i++)
        if (ht.e[i].t == VK_OBJECT_TYPE_DEVICE && ht.e[i].h && g_real_fns.vkDestroyDevice)
            g_real_fns.vkDestroyDevice((VkDevice)(uintptr_t)ht.e[i].h, NULL);
#endif
    for (uint32_t i = 0; i < ht.n; i++)
        if (ht.e[i].t == VK_OBJECT_TYPE_INSTANCE && ht.e[i].h && g_real_fns.vkDestroyInstance)
            g_real_fns.vkDestroyInstance((VkInstance)(uintptr_t)ht.e[i].h, NULL);
    fprintf(stderr, "vkb_server: client done, %llu calls\n", (unsigned long long)ncalls);
    free(ht.e); free(req); free(rep); free(arena_mem);
    ring_destroy(&c->c2s); ring_destroy(&c->s2c);
    close(c->sock); free(c);
    return NULL;
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
    int miss = vkr_srv_load(&g_real_fns, getproc, NULL);
    g_missing_fns = miss;
#ifdef VKB_CMD_HAS_vkMapMemory
    g_real_vkMapMemory = (PFN_vkMapMemory)getproc("vkMapMemory", NULL);
    if (!g_real_vkMapMemory) miss++;
#endif
    srv_install_wrappers();
    if (miss) fprintf(stderr, "vkb_server: note: %d fns deferred to instance GIPA in %s\n", miss, lib);

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
