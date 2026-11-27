/* T047 guest-side WSI v0 (Vortek-style): VK_KHR_surface + VK_KHR_wayland_surface + VK_KHR_swapchain
 * implemented inside the guest ICD; the vk-bridge/host never sees WSI objects.
 *  - VkSurfaceKHR  = guest VkIcdSurfaceWayland (wl_display + app wl_surface).
 *  - swapchain img = normal bridged VkImage (OPTIMAL, device-local) + per-image host-visible readback
 *                    buffer (bridged memfd shadow mapping, T029) + cmdbuf (image -> buffer copy) + fence.
 *  - Acquire       = round-robin free image; sem/fence signalled via an empty bridged vkQueueSubmit.
 *  - Present       = submit(wait sems, copy) -> wait fence (server syncs shadow) -> memcpy into a wl_shm
 *                    buffer -> wl_surface attach/damage/commit -> wl_display_flush.
 * libwayland-client is dlopen'ed (no build-time dep; guest builds lack wayland headers).
 * Private wl_event_queue so we never steal the app's events. Zero-copy (AHB/dmabuf) comes later.
 * #included by icd.c after vkb_icd_gen.inc (uses the static icd_vk* thunks + icd structs). */
#include <dlfcn.h>
#include <stdarg.h>
#include <sys/mman.h>
#include <sys/syscall.h>

/* ---- minimal libwayland-client ABI (stable since 1.20) ---- */
struct wl_proxy;
struct wl_event_queue;
struct wl_message { const char *name; const char *signature; const struct wl_interface **types; };
struct wl_interface {
    const char *name; int version;
    int method_count; const struct wl_message *methods;
    int event_count; const struct wl_message *events;
};
#define WSI_WL_MARSHAL_FLAG_DESTROY 1u
#define WSI_WL_SHM_FORMAT_ARGB8888 0u
#define WSI_WL_SHM_FORMAT_XRGB8888 1u

static struct {
    int tried, ok;
    struct wl_proxy *(*marshal_flags)(struct wl_proxy *, uint32_t, const struct wl_interface *, uint32_t, uint32_t, ...);
    int (*add_listener)(struct wl_proxy *, void (**)(void), void *);
    void (*proxy_destroy)(struct wl_proxy *);
    void *(*create_wrapper)(void *);
    void (*wrapper_destroy)(void *);
    void (*set_queue)(struct wl_proxy *, struct wl_event_queue *);
    uint32_t (*get_version)(struct wl_proxy *);
    struct wl_event_queue *(*create_queue)(struct wl_display *);
    void (*queue_destroy)(struct wl_event_queue *);
    int (*roundtrip_queue)(struct wl_display *, struct wl_event_queue *);
    int (*dispatch_queue_pending)(struct wl_display *, struct wl_event_queue *);
    int (*flush)(struct wl_display *);
    const struct wl_interface *registry_if, *shm_if, *pool_if, *buffer_if;
} wl;

static pthread_mutex_t g_wsi_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_wsi_presents;

static int wsi_wl_load(void) {
    pthread_mutex_lock(&g_wsi_mu);
    if (!wl.tried) {
        wl.tried = 1;
        void *h = dlopen("libwayland-client.so.0", RTLD_NOW | RTLD_LOCAL);
        if (h) {
#define WL_SYM(dst, nm) (*(void **)&(dst) = dlsym(h, nm))
            int ok = 1;
            ok &= WL_SYM(wl.marshal_flags, "wl_proxy_marshal_flags") != NULL;
            ok &= WL_SYM(wl.add_listener, "wl_proxy_add_listener") != NULL;
            ok &= WL_SYM(wl.proxy_destroy, "wl_proxy_destroy") != NULL;
            ok &= WL_SYM(wl.create_wrapper, "wl_proxy_create_wrapper") != NULL;
            ok &= WL_SYM(wl.wrapper_destroy, "wl_proxy_wrapper_destroy") != NULL;
            ok &= WL_SYM(wl.set_queue, "wl_proxy_set_queue") != NULL;
            ok &= WL_SYM(wl.get_version, "wl_proxy_get_version") != NULL;
            ok &= WL_SYM(wl.create_queue, "wl_display_create_queue") != NULL;
            ok &= WL_SYM(wl.queue_destroy, "wl_event_queue_destroy") != NULL;
            ok &= WL_SYM(wl.roundtrip_queue, "wl_display_roundtrip_queue") != NULL;
            ok &= WL_SYM(wl.dispatch_queue_pending, "wl_display_dispatch_queue_pending") != NULL;
            ok &= WL_SYM(wl.flush, "wl_display_flush") != NULL;
            ok &= WL_SYM(wl.registry_if, "wl_registry_interface") != NULL;
            ok &= WL_SYM(wl.shm_if, "wl_shm_interface") != NULL;
            ok &= WL_SYM(wl.pool_if, "wl_shm_pool_interface") != NULL;
            ok &= WL_SYM(wl.buffer_if, "wl_buffer_interface") != NULL;
#undef WL_SYM
            wl.ok = ok;
        }
        if (!wl.ok) fprintf(stderr, "[lodos-icd] wsi: libwayland-client.so.0 unavailable (%s)\n", dlerror());
    }
    int ok = wl.ok;
    pthread_mutex_unlock(&g_wsi_mu);
    return ok;
}

__attribute__((destructor)) static void wsi_fini(void) {
    const char *e = getenv("LODOS_VK_WSI_STATS");
    if (e && e[0] == '1' && g_wsi_presents)
        fprintf(stderr, "[lodos-icd] wsi presents=%llu\n", (unsigned long long)g_wsi_presents);
}

/* ---- surface ---- */

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkCreateWaylandSurfaceKHR(
    VkInstance instance, const VkWaylandSurfaceCreateInfoKHR *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSurfaceKHR *pSurface) {
    (void)instance; (void)pAllocator;
    if (!pCreateInfo || !pSurface || !pCreateInfo->display || !pCreateInfo->surface)
        return VK_ERROR_INITIALIZATION_FAILED;
    if (!wsi_wl_load()) return VK_ERROR_INITIALIZATION_FAILED;
    VkIcdSurfaceWayland *s = calloc(1, sizeof(*s));
    if (!s) return VK_ERROR_OUT_OF_HOST_MEMORY;
    s->base.platform = VK_ICD_WSI_PLATFORM_WAYLAND;
    s->display = pCreateInfo->display;
    s->surface = pCreateInfo->surface;
    *pSurface = (VkSurfaceKHR)(uintptr_t)s;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL icd_vkDestroySurfaceKHR(
    VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks *pAllocator) {
    (void)instance; (void)pAllocator;
    free((void *)(uintptr_t)surface);
}

static VKAPI_ATTR VkBool32 VKAPI_CALL icd_vkGetPhysicalDeviceWaylandPresentationSupportKHR(
    VkPhysicalDevice pd, uint32_t qfi, struct wl_display *display) {
    (void)pd; (void)qfi; (void)display;
    return VK_TRUE; /* any queue: presentation is a guest-side readback + wl_shm commit */
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkGetPhysicalDeviceSurfaceSupportKHR(
    VkPhysicalDevice pd, uint32_t qfi, VkSurfaceKHR surface, VkBool32 *pSupported) {
    (void)pd; (void)qfi;
    if (!pSupported) return VK_ERROR_INITIALIZATION_FAILED;
    const VkIcdSurfaceBase *b = (const VkIcdSurfaceBase *)(uintptr_t)surface;
    *pSupported = (b && b->platform == VK_ICD_WSI_PLATFORM_WAYLAND) ? VK_TRUE : VK_FALSE;
    return VK_SUCCESS;
}

#define WSI_USAGE (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | \
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT | \
                   VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT)

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
    VkPhysicalDevice pd, VkSurfaceKHR surface, VkSurfaceCapabilitiesKHR *pCaps) {
    (void)pd; (void)surface;
    if (!pCaps) return VK_ERROR_INITIALIZATION_FAILED;
    memset(pCaps, 0, sizeof(*pCaps));
    pCaps->minImageCount = 2;
    pCaps->maxImageCount = 8;
    pCaps->currentExtent.width = 0xFFFFFFFFu; /* Wayland: size set by the swapchain */
    pCaps->currentExtent.height = 0xFFFFFFFFu;
    pCaps->minImageExtent.width = pCaps->minImageExtent.height = 1;
    pCaps->maxImageExtent.width = pCaps->maxImageExtent.height = 8192;
    pCaps->maxImageArrayLayers = 1;
    pCaps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pCaps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    pCaps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR | VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR |
                                     VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    pCaps->supportedUsageFlags = WSI_USAGE;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkGetPhysicalDeviceSurfaceFormatsKHR(
    VkPhysicalDevice pd, VkSurfaceKHR surface, uint32_t *pCount, VkSurfaceFormatKHR *pFormats) {
    (void)pd; (void)surface;
    static const VkSurfaceFormatKHR k_fmts[] = {
        { VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
        { VK_FORMAT_B8G8R8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
    };
    if (!pCount) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t n = sizeof(k_fmts) / sizeof(k_fmts[0]);
    if (!pFormats) { *pCount = n; return VK_SUCCESS; }
    uint32_t c = *pCount < n ? *pCount : n;
    memcpy(pFormats, k_fmts, c * sizeof(k_fmts[0]));
    *pCount = c;
    return c < n ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkGetPhysicalDeviceSurfacePresentModesKHR(
    VkPhysicalDevice pd, VkSurfaceKHR surface, uint32_t *pCount, VkPresentModeKHR *pModes) {
    (void)pd; (void)surface;
    static const VkPresentModeKHR k_modes[] = { VK_PRESENT_MODE_FIFO_KHR, VK_PRESENT_MODE_MAILBOX_KHR };
    if (!pCount) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t n = 2;
    if (!pModes) { *pCount = n; return VK_SUCCESS; }
    uint32_t c = *pCount < n ? *pCount : n;
    memcpy(pModes, k_modes, c * sizeof(k_modes[0]));
    *pCount = c;
    return c < n ? VK_INCOMPLETE : VK_SUCCESS;
}

/* ---- swapchain ---- */

typedef struct {
    VkImage image;
    VkDeviceMemory image_mem;
    VkBuffer buf;
    VkDeviceMemory buf_mem;
    void *buf_ptr;
    VkCommandBuffer cmd;
    VkFence fence;
    struct wl_proxy *wl_buffer;
    int acquired, busy;
} wsi_image;

typedef struct {
    icd_device *dev;
    VkIcdSurfaceWayland *surf;
    VkExtent2D extent;
    VkFormat format;
    uint32_t nimg, next;
    wsi_image *img;
    VkCommandPool pool;
    struct wl_event_queue *queue;
    struct wl_proxy *registry, *shm, *pool_proxy;
    int shm_fd;
    void *shm_ptr;
    size_t shm_size, img_bytes;
    int retired;
} wsi_swapchain;

static void wsi_registry_global(void *data, struct wl_proxy *reg, uint32_t name, const char *iface, uint32_t ver) {
    wsi_swapchain *sc = data;
    if (!sc->shm && strcmp(iface, "wl_shm") == 0) {
        (void)ver;
        sc->shm = wl.marshal_flags(reg, 0 /* wl_registry.bind */, wl.shm_if, 1, 0,
                                   name, wl.shm_if->name, 1u, NULL);
    }
}
static void wsi_registry_remove(void *data, struct wl_proxy *reg, uint32_t name) { (void)data; (void)reg; (void)name; }
static const struct { void (*global)(void *, struct wl_proxy *, uint32_t, const char *, uint32_t);
                      void (*remove)(void *, struct wl_proxy *, uint32_t); }
    k_registry_listener = { wsi_registry_global, wsi_registry_remove };

static void wsi_buffer_release(void *data, struct wl_proxy *buf) { (void)buf; ((wsi_image *)data)->busy = 0; }
static const struct { void (*release)(void *, struct wl_proxy *); } k_buffer_listener = { wsi_buffer_release };

static int wsi_find_memtype(icd_device *dev, uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags pref) {
    VkPhysicalDeviceMemoryProperties mp;
    icd_vkGetPhysicalDeviceMemoryProperties((VkPhysicalDevice)dev->pdev, &mp);
    for (int pass = 0; pass < 2; pass++) {
        VkMemoryPropertyFlags need = pass == 0 ? (want | pref) : want;
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & need) == need) return (int)i;
    }
    return -1;
}

static VkQueue wsi_queue(icd_device *dev) {
    pthread_mutex_lock(&g_mu);
    icd_queue *q = dev->queues;
    pthread_mutex_unlock(&g_mu);
    if (q) return (VkQueue)q;
    VkQueue vq = VK_NULL_HANDLE;
    icd_get_queue_common((VkDevice)dev, 0, 0, 0, 0, &vq);
    return vq;
}

static void wsi_swapchain_free(wsi_swapchain *sc) {
    if (!sc) return;
    VkDevice d = (VkDevice)sc->dev;
    if (sc->img) {
        for (uint32_t i = 0; i < sc->nimg; i++) {
            wsi_image *im = &sc->img[i];
            if (im->fence) {
                icd_vkWaitForFences(d, 1, &im->fence, VK_TRUE, UINT64_MAX);
                icd_vkDestroyFence(d, im->fence, NULL);
            }
            if (im->wl_buffer) wl.marshal_flags(im->wl_buffer, 0 /* wl_buffer.destroy */, NULL,
                                                wl.get_version(im->wl_buffer), WSI_WL_MARSHAL_FLAG_DESTROY);
            if (im->buf_ptr) icd_vkUnmapMemory(d, im->buf_mem);
            if (im->buf) icd_vkDestroyBuffer(d, im->buf, NULL);
            if (im->buf_mem) icd_vkFreeMemory(d, im->buf_mem, NULL);
            if (im->image) icd_vkDestroyImage(d, im->image, NULL);
            if (im->image_mem) icd_vkFreeMemory(d, im->image_mem, NULL);
        }
    }
    if (sc->pool) icd_vkDestroyCommandPool(d, sc->pool, NULL);
    if (sc->pool_proxy) wl.marshal_flags(sc->pool_proxy, 1 /* wl_shm_pool.destroy */, NULL,
                                         wl.get_version(sc->pool_proxy), WSI_WL_MARSHAL_FLAG_DESTROY);
    if (sc->shm) wl.proxy_destroy(sc->shm);
    if (sc->registry) wl.proxy_destroy(sc->registry);
    if (sc->surf && wl.ok) wl.flush(sc->surf->display);
    if (sc->queue) wl.queue_destroy(sc->queue);
    if (sc->shm_ptr) munmap(sc->shm_ptr, sc->shm_size);
    if (sc->shm_fd >= 0) close(sc->shm_fd);
    free(sc->img);
    free(sc);
}

static VkResult wsi_wl_setup(wsi_swapchain *sc) {
    struct wl_display *dpy = sc->surf->display;
    sc->queue = wl.create_queue(dpy);
    if (!sc->queue) return VK_ERROR_SURFACE_LOST_KHR;
    struct wl_proxy *wrap = wl.create_wrapper(dpy);
    if (!wrap) return VK_ERROR_SURFACE_LOST_KHR;
    wl.set_queue(wrap, sc->queue);
    sc->registry = wl.marshal_flags(wrap, 1 /* wl_display.get_registry */, wl.registry_if,
                                    wl.get_version(wrap), 0, NULL);
    wl.wrapper_destroy(wrap);
    if (!sc->registry) return VK_ERROR_SURFACE_LOST_KHR;
    wl.add_listener(sc->registry, (void (**)(void))&k_registry_listener, sc);
    if (wl.roundtrip_queue(dpy, sc->queue) < 0 || !sc->shm) return VK_ERROR_SURFACE_LOST_KHR;

    sc->img_bytes = (size_t)sc->extent.width * sc->extent.height * 4u;
    sc->shm_size = sc->img_bytes * sc->nimg;
    sc->shm_fd = (int)syscall(SYS_memfd_create, "lodos-wsi", 1u /* MFD_CLOEXEC */);
    if (sc->shm_fd < 0 || ftruncate(sc->shm_fd, (off_t)sc->shm_size) < 0) return VK_ERROR_OUT_OF_HOST_MEMORY;
    sc->shm_ptr = mmap(NULL, sc->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, sc->shm_fd, 0);
    if (sc->shm_ptr == MAP_FAILED) { sc->shm_ptr = NULL; return VK_ERROR_OUT_OF_HOST_MEMORY; }
    sc->pool_proxy = wl.marshal_flags(sc->shm, 0 /* wl_shm.create_pool */, wl.pool_if,
                                      wl.get_version(sc->shm), 0, NULL, sc->shm_fd, (int32_t)sc->shm_size);
    if (!sc->pool_proxy) return VK_ERROR_SURFACE_LOST_KHR;
    for (uint32_t i = 0; i < sc->nimg; i++) {
        wsi_image *im = &sc->img[i];
        im->wl_buffer = wl.marshal_flags(sc->pool_proxy, 0 /* wl_shm_pool.create_buffer */, wl.buffer_if,
                                         wl.get_version(sc->pool_proxy), 0, NULL,
                                         (int32_t)(sc->img_bytes * i), (int32_t)sc->extent.width,
                                         (int32_t)sc->extent.height, (int32_t)(sc->extent.width * 4u),
                                         WSI_WL_SHM_FORMAT_XRGB8888);
        if (!im->wl_buffer) return VK_ERROR_SURFACE_LOST_KHR;
        wl.add_listener(im->wl_buffer, (void (**)(void))&k_buffer_listener, im);
    }
    wl.flush(dpy);
    return VK_SUCCESS;
}

static VkResult wsi_image_setup(wsi_swapchain *sc, wsi_image *im, const VkSwapchainCreateInfoKHR *ci) {
    VkDevice d = (VkDevice)sc->dev;
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = ci->imageFormat,
        .extent = { ci->imageExtent.width, ci->imageExtent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (ci->flags & VK_SWAPCHAIN_CREATE_MUTABLE_FORMAT_BIT_KHR) ici.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    VkResult r = icd_vkCreateImage(d, &ici, NULL, &im->image);
    if (r != VK_SUCCESS) return r;
    VkMemoryRequirements mr;
    icd_vkGetImageMemoryRequirements(d, im->image, &mr);
    int mt = wsi_find_memtype(sc->dev, mr.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = mr.size, .memoryTypeIndex = (uint32_t)mt };
    if ((r = icd_vkAllocateMemory(d, &mai, NULL, &im->image_mem)) != VK_SUCCESS) return r;
    if ((r = icd_vkBindImageMemory(d, im->image, im->image_mem, 0)) != VK_SUCCESS) return r;

    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = sc->img_bytes,
                               .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    if ((r = icd_vkCreateBuffer(d, &bci, NULL, &im->buf)) != VK_SUCCESS) return r;
    icd_vkGetBufferMemoryRequirements(d, im->buf, &mr);
    mt = wsi_find_memtype(sc->dev, mr.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (mt < 0) return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = (uint32_t)mt;
    if ((r = icd_vkAllocateMemory(d, &mai, NULL, &im->buf_mem)) != VK_SUCCESS) return r;
    if ((r = icd_vkBindBufferMemory(d, im->buf, im->buf_mem, 0)) != VK_SUCCESS) return r;
    if ((r = icd_vkMapMemory(d, im->buf_mem, 0, VK_WHOLE_SIZE, 0, &im->buf_ptr)) != VK_SUCCESS) {
        im->buf_ptr = NULL;
        return r;
    }

    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .commandPool = sc->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    if ((r = icd_vkAllocateCommandBuffers(d, &cai, &im->cmd)) != VK_SUCCESS) return r;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    if ((r = icd_vkBeginCommandBuffer(im->cmd, &bi)) != VK_SUCCESS) return r;
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = im->image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    icd_vkCmdPipelineBarrier(im->cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy reg = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                              .imageExtent = { sc->extent.width, sc->extent.height, 1 } };
    icd_vkCmdCopyImageToBuffer(im->cmd, im->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im->buf, 1, &reg);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    icd_vkCmdPipelineBarrier(im->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                             0, 0, NULL, 0, NULL, 1, &b);
    VkBufferMemoryBarrier bb = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                                 .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
                                 .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                 .buffer = im->buf, .size = VK_WHOLE_SIZE };
    icd_vkCmdPipelineBarrier(im->cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                             0, 0, NULL, 1, &bb, 0, NULL);
    if ((r = icd_vkEndCommandBuffer(im->cmd)) != VK_SUCCESS) return r;
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
    return icd_vkCreateFence(d, &fci, NULL, &im->fence);
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkCreateSwapchainKHR(
    VkDevice device, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *pAllocator,
    VkSwapchainKHR *pSwapchain) {
    (void)pAllocator;
    if (!device || !ci || !pSwapchain || !ci->surface) return VK_ERROR_INITIALIZATION_FAILED;
    if (!wsi_wl_load()) return VK_ERROR_SURFACE_LOST_KHR;
    VkIcdSurfaceWayland *surf = (VkIcdSurfaceWayland *)(uintptr_t)ci->surface;
    if (surf->base.platform != VK_ICD_WSI_PLATFORM_WAYLAND) return VK_ERROR_SURFACE_LOST_KHR;
    if (ci->imageFormat != VK_FORMAT_B8G8R8A8_UNORM && ci->imageFormat != VK_FORMAT_B8G8R8A8_SRGB)
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    if (!ci->imageExtent.width || !ci->imageExtent.height) return VK_ERROR_INITIALIZATION_FAILED;
    if (ci->oldSwapchain) ((wsi_swapchain *)(uintptr_t)ci->oldSwapchain)->retired = 1;

    wsi_swapchain *sc = calloc(1, sizeof(*sc));
    if (!sc) return VK_ERROR_OUT_OF_HOST_MEMORY;
    sc->dev = (icd_device *)device;
    sc->surf = surf;
    sc->extent = ci->imageExtent;
    sc->format = ci->imageFormat;
    sc->shm_fd = -1;
    sc->nimg = ci->minImageCount < 2 ? 2 : (ci->minImageCount > 8 ? 8 : ci->minImageCount);
    if (ci->presentMode == VK_PRESENT_MODE_MAILBOX_KHR && sc->nimg < 3) sc->nimg = 3;
    sc->img = calloc(sc->nimg, sizeof(*sc->img));
    VkResult r = sc->img ? VK_SUCCESS : VK_ERROR_OUT_OF_HOST_MEMORY;
    if (r == VK_SUCCESS) r = wsi_wl_setup(sc);
    if (r == VK_SUCCESS) {
        VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
        VkQueue q = wsi_queue(sc->dev);
        if (q) pci.queueFamilyIndex = ((icd_queue *)q)->qfi;
        r = icd_vkCreateCommandPool(device, &pci, NULL, &sc->pool);
    }
    for (uint32_t i = 0; r == VK_SUCCESS && i < sc->nimg; i++) r = wsi_image_setup(sc, &sc->img[i], ci);
    if (r != VK_SUCCESS) {
        fprintf(stderr, "[lodos-icd] wsi: vkCreateSwapchainKHR %ux%u failed: %d\n",
                ci->imageExtent.width, ci->imageExtent.height, r);
        wsi_swapchain_free(sc);
        return r;
    }
    *pSwapchain = (VkSwapchainKHR)(uintptr_t)sc;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL icd_vkDestroySwapchainKHR(
    VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    wsi_swapchain_free((wsi_swapchain *)(uintptr_t)swapchain);
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkGetSwapchainImagesKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint32_t *pCount, VkImage *pImages) {
    (void)device;
    wsi_swapchain *sc = (wsi_swapchain *)(uintptr_t)swapchain;
    if (!sc || !pCount) return VK_ERROR_INITIALIZATION_FAILED;
    if (!pImages) { *pCount = sc->nimg; return VK_SUCCESS; }
    uint32_t c = *pCount < sc->nimg ? *pCount : sc->nimg;
    for (uint32_t i = 0; i < c; i++) pImages[i] = sc->img[i].image;
    *pCount = c;
    return c < sc->nimg ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkAcquireNextImageKHR(
    VkDevice device, VkSwapchainKHR swapchain, uint64_t timeout, VkSemaphore semaphore, VkFence fence,
    uint32_t *pImageIndex) {
    (void)timeout;
    wsi_swapchain *sc = (wsi_swapchain *)(uintptr_t)swapchain;
    if (!sc || !pImageIndex) return VK_ERROR_INITIALIZATION_FAILED;
    if (sc->retired) return VK_ERROR_OUT_OF_DATE_KHR;
    uint32_t idx = UINT32_MAX;
    for (uint32_t k = 0; k < sc->nimg; k++) {
        uint32_t i = (sc->next + k) % sc->nimg;
        if (!sc->img[i].acquired) { idx = i; break; }
    }
    if (idx == UINT32_MAX) return timeout ? VK_TIMEOUT : VK_NOT_READY;
    /* present is synchronous (fence waited), so the image is idle; only the app's sem/fence need signalling */
    if (semaphore || fence) {
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                            .signalSemaphoreCount = semaphore ? 1u : 0u, .pSignalSemaphores = &semaphore };
        VkResult r = icd_vkQueueSubmit(wsi_queue((icd_device *)device), 1, &si, fence);
        if (r != VK_SUCCESS) return r;
    }
    sc->img[idx].acquired = 1;
    sc->next = (idx + 1) % sc->nimg;
    *pImageIndex = idx;
    return VK_SUCCESS;
}

static VkResult wsi_present_one(VkQueue queue, wsi_swapchain *sc, uint32_t idx,
                                uint32_t nwait, const VkSemaphore *waits) {
    if (idx >= sc->nimg || !sc->img[idx].acquired) return VK_ERROR_OUT_OF_DATE_KHR;
    wsi_image *im = &sc->img[idx];
    VkDevice d = (VkDevice)sc->dev;
    VkPipelineStageFlags stages[16];
    for (uint32_t i = 0; i < nwait && i < 16; i++) stages[i] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                        .waitSemaphoreCount = nwait < 16 ? nwait : 16, .pWaitSemaphores = waits,
                        .pWaitDstStageMask = stages, .commandBufferCount = 1, .pCommandBuffers = &im->cmd };
    VkResult r = icd_vkResetFences(d, 1, &im->fence);
    if (r == VK_SUCCESS) r = icd_vkQueueSubmit(queue, 1, &si, im->fence);
    if (r == VK_SUCCESS) r = icd_vkWaitForFences(d, 1, &im->fence, VK_TRUE, UINT64_MAX); /* syncs shadow */
    im->acquired = 0;
    if (r != VK_SUCCESS) return r;

    struct wl_display *dpy = sc->surf->display;
    wl.dispatch_queue_pending(dpy, sc->queue);
    for (int tries = 0; im->busy && tries < 4; tries++)
        if (wl.roundtrip_queue(dpy, sc->queue) < 0) return VK_ERROR_SURFACE_LOST_KHR;
    memcpy((uint8_t *)sc->shm_ptr + sc->img_bytes * idx, im->buf_ptr, sc->img_bytes);

    struct wl_proxy *ws = (struct wl_proxy *)sc->surf->surface;
    uint32_t sv = wl.get_version(ws);
    wl.marshal_flags(ws, 1 /* wl_surface.attach */, NULL, sv, 0, im->wl_buffer, 0, 0);
    if (sv >= 4)
        wl.marshal_flags(ws, 9 /* wl_surface.damage_buffer */, NULL, sv, 0, 0, 0, INT32_MAX, INT32_MAX);
    else
        wl.marshal_flags(ws, 2 /* wl_surface.damage */, NULL, sv, 0, 0, 0, INT32_MAX, INT32_MAX);
    wl.marshal_flags(ws, 6 /* wl_surface.commit */, NULL, sv, 0);
    im->busy = 1;
    if (wl.flush(dpy) < 0) return VK_ERROR_SURFACE_LOST_KHR;
    __atomic_add_fetch(&g_wsi_presents, 1, __ATOMIC_RELAXED);
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR *pi) {
    if (!queue || !pi) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult worst = VK_SUCCESS;
    for (uint32_t i = 0; i < pi->swapchainCount; i++) {
        wsi_swapchain *sc = (wsi_swapchain *)(uintptr_t)pi->pSwapchains[i];
        /* app wait semaphores are consumed by the first swapchain's submit (same queue -> ordered) */
        VkResult r = wsi_present_one(queue, sc, pi->pImageIndices[i],
                                     i == 0 ? pi->waitSemaphoreCount : 0, i == 0 ? pi->pWaitSemaphores : NULL);
        if (pi->pResults) pi->pResults[i] = r;
        if (r < 0 && worst >= 0) worst = r;
    }
    return worst;
}

static int wsi_inst_ext(const char *name) {
    return strcmp(name, VK_KHR_SURFACE_EXTENSION_NAME) == 0 ||
           strcmp(name, VK_KHR_WAYLAND_SURFACE_EXTENSION_NAME) == 0;
}

static int wsi_dev_ext(const char *name) { return strcmp(name, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0; }

static const proc_ent k_wsi_inst_procs[] = {
    ENT("vkCreateWaylandSurfaceKHR", icd_vkCreateWaylandSurfaceKHR),
    ENT("vkDestroySurfaceKHR", icd_vkDestroySurfaceKHR),
};
static const proc_ent k_wsi_pdev_procs[] = {
    ENT("vkGetPhysicalDeviceWaylandPresentationSupportKHR", icd_vkGetPhysicalDeviceWaylandPresentationSupportKHR),
    ENT("vkGetPhysicalDeviceSurfaceSupportKHR", icd_vkGetPhysicalDeviceSurfaceSupportKHR),
    ENT("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", icd_vkGetPhysicalDeviceSurfaceCapabilitiesKHR),
    ENT("vkGetPhysicalDeviceSurfaceFormatsKHR", icd_vkGetPhysicalDeviceSurfaceFormatsKHR),
    ENT("vkGetPhysicalDeviceSurfacePresentModesKHR", icd_vkGetPhysicalDeviceSurfacePresentModesKHR),
};
static const proc_ent k_wsi_dev_procs[] = {
    ENT("vkCreateSwapchainKHR", icd_vkCreateSwapchainKHR),
    ENT("vkDestroySwapchainKHR", icd_vkDestroySwapchainKHR),
    ENT("vkGetSwapchainImagesKHR", icd_vkGetSwapchainImagesKHR),
    ENT("vkAcquireNextImageKHR", icd_vkAcquireNextImageKHR),
    ENT("vkQueuePresentKHR", icd_vkQueuePresentKHR),
};
