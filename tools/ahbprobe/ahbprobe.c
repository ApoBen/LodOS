// ahbprobe: AHardwareBuffer / dmabuf / dma_heap / Vulkan AHB-import probe. Emits JSON.
// Links -landroid (AHB API 26+); Vulkan via dlopen("libvulkan.so").
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>
#include <android/hardware_buffer.h>
#include <dlfcn.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/dma-heap.h>
#include <unistd.h>

// AHardwareBuffer_getNativeHandle is not in the public NDK headers; resolve dynamically.
typedef struct native_handle { int version, numFds, numInts; int data[0]; } native_handle_t;
typedef const native_handle_t *(*PFN_getNativeHandle)(const AHardwareBuffer *);

static FILE *o;
static int first;
#define SEP() do { if (!first) fputs(",", o); first = 0; } while (0)
static void key(const char *k) { SEP(); fprintf(o, "\n\"%s\":", k); }
static void jstr(const char *s) {
    fputc('"', o);
    for (; *s; s++) {
        if (*s == '"' || *s == '\\') fprintf(o, "\\%c", *s);
        else if ((unsigned char)*s < 0x20) fprintf(o, "\\u%04x", *s);
        else fputc(*s, o);
    }
    fputc('"', o);
}
static void kstr(const char *k, const char *v) { key(k); jstr(v ? v : ""); }
static void kint(const char *k, long long v) { key(k); fprintf(o, "%lld", v); }
static void khex(const char *k, unsigned long long v) { key(k); fprintf(o, "\"0x%llx\"", v); }
static void kbool(const char *k, int v) { key(k); fputs(v ? "true" : "false", o); }
static void knull(const char *k) { key(k); fputs("null", o); }
static void obeg(const char *k) { if (k) key(k); else SEP(); fputc('{', o); first = 1; }
static void oend(void) { fputc('}', o); first = 0; }
static void abeg(const char *k) { key(k); fputc('[', o); first = 1; }
static void aend(void) { fputc(']', o); first = 0; }
static void kvk(const char *k, VkResult r) { key(k); fprintf(o, "%d", (int)r); }

// describe an fd: stat, readlink, fdinfo (exp_name for dmabuf)
static void fd_info(const char *k, int fd) {
    obeg(k);
    kint("fd", fd);
    struct stat st;
    if (fstat(fd, &st) == 0) {
        khex("st_mode", st.st_mode); kint("st_size", st.st_size);
        kint("st_dev", (long long)st.st_dev); kint("st_ino", (long long)st.st_ino);
    } else kint("fstat_errno", errno);
    char p[64], l[256] = {0};
    snprintf(p, sizeof p, "/proc/self/fd/%d", fd);
    ssize_t n = readlink(p, l, sizeof l - 1);
    if (n >= 0) { l[n] = 0; kstr("link", l); } else kint("readlink_errno", errno);
    int dmabuf = n >= 0 && strstr(l, "dmabuf") != NULL;
    snprintf(p, sizeof p, "/proc/self/fdinfo/%d", fd);
    FILE *f = fopen(p, "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\n")] = 0;
            char *c = strchr(line, ':');
            if (!c) continue;
            *c = 0; char *v = c + 1; while (*v == ' ' || *v == '\t') v++;
            if (!strcmp(line, "exp_name") || !strcmp(line, "size") || !strcmp(line, "name") || !strcmp(line, "count")) {
                char kk[64]; snprintf(kk, sizeof kk, "fdinfo_%s", line); kstr(kk, v);
                if (!strcmp(line, "exp_name")) dmabuf = 1;
            }
        }
        fclose(f);
    }
    kbool("is_dmabuf", dmabuf);
    oend();
}

static int has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

#define W 1280
#define H 720
#define USAGE_BASE (AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE | AHARDWAREBUFFER_USAGE_GPU_COLOR_OUTPUT | AHARDWAREBUFFER_USAGE_COMPOSER_OVERLAY)

static AHardwareBuffer *try_alloc(const char *name, uint32_t fmt, uint64_t usage, int *ok_out) {
    AHardwareBuffer_Desc d = { .width = W, .height = H, .layers = 1, .format = fmt, .usage = usage };
    AHardwareBuffer *b = NULL;
    int sup = AHardwareBuffer_isSupported(&d);
    int r = AHardwareBuffer_allocate(&d, &b);
    obeg(NULL);
    kstr("name", name); kint("format", fmt); khex("usage", usage);
    kbool("isSupported", sup); kint("ret", r); kbool("ok", r == 0 && b);
    if (r == 0 && b) {
        AHardwareBuffer_Desc g; AHardwareBuffer_describe(b, &g);
        kint("stride", g.stride); khex("usage_actual", g.usage);
    }
    oend();
    if (ok_out) *ok_out = (r == 0 && b);
    return (r == 0) ? b : NULL;
}

int main(int argc, char **argv) {
    o = argc > 1 ? fopen(argv[1], "w") : stdout;
    if (!o) { perror("fopen"); return 1; }
    fputc('{', o); first = 1;
    kint("uid", getuid());
    kstr("note", "run as adb shell uid; app uid (untrusted_app SELinux) may differ for dma_heap/fd access");

    // ---- 1. AHB alloc ----
    int ok_rgba = 0, ok_bgra = 0, ok_cpu = 0;
    abeg("ahb_alloc");
    AHardwareBuffer *ahb = try_alloc("R8G8B8A8_UNORM", AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM, USAGE_BASE, &ok_rgba);
    // B8G8R8A8 is not in the public NDK enum; HAL_PIXEL_FORMAT_BGRA_8888 = 5
    AHardwareBuffer *ahb_bgra = try_alloc("B8G8R8A8(5)", 5, USAGE_BASE, &ok_bgra);
    AHardwareBuffer *ahb_cpu = try_alloc("R8G8B8A8_UNORM+CPU_READ_RARELY", AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM,
                                         USAGE_BASE | AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, &ok_cpu);
    AHardwareBuffer *ahb_x = try_alloc("R8G8B8X8_UNORM", AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM, USAGE_BASE, NULL);
    AHardwareBuffer *ahb_565 = try_alloc("R5G6B5_UNORM", AHARDWAREBUFFER_FORMAT_R5G6B5_UNORM, USAGE_BASE, NULL);
    AHardwareBuffer *ahb_f16 = try_alloc("R16G16B16A16_FLOAT", AHARDWAREBUFFER_FORMAT_R16G16B16A16_FLOAT, USAGE_BASE, NULL);
    aend();
    kbool("ahb_alloc_ok", ok_rgba);
    kbool("ahb_bgra_ok", ok_bgra);

    // ---- 2. native handle / dmabuf ----
    obeg("ahb_fd");
    PFN_getNativeHandle gnh = (PFN_getNativeHandle)dlsym(RTLD_DEFAULT, "AHardwareBuffer_getNativeHandle");
    if (!gnh) { void *la = dlopen("libandroid.so", RTLD_NOW); if (la) gnh = (PFN_getNativeHandle)dlsym(la, "AHardwareBuffer_getNativeHandle"); }
    if (!gnh) { void *ln = dlopen("libnativewindow.so", RTLD_NOW); if (ln) gnh = (PFN_getNativeHandle)dlsym(ln, "AHardwareBuffer_getNativeHandle"); }
    kbool("getNativeHandle_sym", gnh != NULL);
    if (gnh && ahb) {
        const native_handle_t *h = gnh(ahb);
        if (h) {
            kint("version", h->version); kint("numFds", h->numFds); kint("numInts", h->numInts);
            abeg("fds");
            for (int i = 0; i < h->numFds && i < 8; i++) fd_info(NULL, h->data[i]);
            aend();
            int dmi = -1;
            for (int i = 0; i < h->numFds && dmi < 0; i++) {
                char p[64], l[256] = {0}; snprintf(p, sizeof p, "/proc/self/fd/%d", h->data[i]);
                ssize_t n = readlink(p, l, sizeof l - 1);
                if (n > 0 && strstr(l, "dmabuf")) dmi = i;
            }
            kbool("fd0_is_dmabuf", dmi == 0);
            kint("dmabuf_fd_index", dmi);
        } else knull("handle");
    }
    // AHB sendHandleToUnixSocket exists (API 26) -> cross-process passing; just note symbol presence
    kbool("sendHandleToUnixSocket_sym", dlsym(RTLD_DEFAULT, "AHardwareBuffer_sendHandleToUnixSocket") != NULL);
    oend();

    // ---- 3. dma_heap ----
    abeg("dma_heap");
    DIR *dd = opendir("/dev/dma_heap");
    if (!dd) { obeg(NULL); kstr("name", "/dev/dma_heap"); kint("opendir_errno", errno); kstr("err", strerror(errno)); oend(); }
    else {
        struct dirent *e;
        while ((e = readdir(dd))) {
            if (e->d_name[0] == '.') continue;
            char p[300]; snprintf(p, sizeof p, "/dev/dma_heap/%s", e->d_name);
            int fd = open(p, O_RDONLY | O_CLOEXEC);
            int er = errno;
            obeg(NULL); kstr("name", e->d_name); kbool("open_ok", fd >= 0);
            if (fd < 0) { kint("errno", er); kstr("err", strerror(er)); } else close(fd);
            oend();
        }
        closedir(dd);
    }
    aend();
    // legacy ion
    { int fd = open("/dev/ion", O_RDONLY | O_CLOEXEC); int er = errno; obeg("ion"); kbool("open_ok", fd >= 0);
      if (fd < 0) kint("errno", er); else close(fd); oend(); }

    // ---- 4. Vulkan ----
    int vk_import_ok = 0, dmabuf_import_res = 99, ext_dmabuf = 0, mfd_res[2] = { 99, 99 }, mfd_dmabuf[2] = { 0, 0 };
    void *lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    PFN_vkGetInstanceProcAddr gipa = lib ? (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr") : NULL;
    obeg("vk");
    if (!gipa) { kstr("err", "no libvulkan"); oend(); goto vk_done; }
#define IL(name) PFN_##name name = (PFN_##name)gipa(inst, #name)
    {
    VkInstance inst = VK_NULL_HANDLE;
    IL(vkCreateInstance);
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "ahbprobe", .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    kvk("vkCreateInstance", r);
    if (r) { oend(); goto vk_done; }
    IL(vkEnumeratePhysicalDevices); IL(vkEnumerateDeviceExtensionProperties); IL(vkGetPhysicalDeviceProperties);
    IL(vkGetPhysicalDeviceQueueFamilyProperties); IL(vkCreateDevice); IL(vkGetDeviceProcAddr);
    IL(vkGetPhysicalDeviceExternalBufferProperties); IL(vkGetPhysicalDeviceExternalSemaphoreProperties);
    IL(vkGetPhysicalDeviceExternalFenceProperties); IL(vkGetPhysicalDeviceImageFormatProperties2);
    IL(vkGetPhysicalDeviceMemoryProperties);
    uint32_t n = 1; VkPhysicalDevice pd;
    r = vkEnumeratePhysicalDevices(inst, &n, &pd);
    if ((r != VK_SUCCESS && r != VK_INCOMPLETE) || !n) { kstr("err", "no physdev"); oend(); goto vk_done; }
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pd, &pp);
    kstr("device", pp.deviceName); khex("driverVersion", pp.driverVersion);
    uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, NULL);
    VkExtensionProperties *ex = calloc(ne, sizeof *ex); vkEnumerateDeviceExtensionProperties(pd, NULL, &ne, ex);
    const char *want[] = { "VK_ANDROID_external_memory_android_hardware_buffer", "VK_EXT_external_memory_dma_buf",
        "VK_KHR_external_memory_fd", "VK_KHR_external_semaphore_fd", "VK_KHR_external_fence_fd",
        "VK_EXT_queue_family_foreign", "VK_EXT_image_drm_format_modifier", "VK_ANDROID_external_format_resolve",
        "VK_EXT_external_memory_host", "VK_KHR_swapchain" };
    int have[10];
    obeg("ext");
    for (int i = 0; i < 10; i++) { have[i] = has_ext(ex, ne, want[i]); kbool(want[i], have[i]); }
    oend();
    ext_dmabuf = have[1];
    int ext_ahb = have[0], ext_memfd = have[2], ext_semfd = have[3], ext_fencefd = have[4], ext_foreign = have[5];

    // external handle capability queries
    obeg("ext_caps");
    {
        struct { const char *n; VkExternalMemoryHandleTypeFlagBits t; } bt[] = {
            { "buffer_opaque_fd", VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT },
            { "buffer_dma_buf", VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT },
            { "buffer_ahb", VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID } };
        for (int i = 0; i < 3; i++) {
            VkPhysicalDeviceExternalBufferInfo bi = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_BUFFER_INFO,
                .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, .handleType = bt[i].t };
            VkExternalBufferProperties ep = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_BUFFER_PROPERTIES };
            vkGetPhysicalDeviceExternalBufferProperties(pd, &bi, &ep);
            obeg(bt[i].n); khex("features", ep.externalMemoryProperties.externalMemoryFeatures);
            khex("compatible", ep.externalMemoryProperties.compatibleHandleTypes); oend();
        }
        struct { const char *n; VkExternalSemaphoreHandleTypeFlagBits t; } st[] = {
            { "sem_opaque_fd", VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT },
            { "sem_sync_fd", VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT } };
        for (int i = 0; i < 2; i++) {
            VkPhysicalDeviceExternalSemaphoreInfo si = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, .handleType = st[i].t };
            VkExternalSemaphoreProperties sp = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };
            vkGetPhysicalDeviceExternalSemaphoreProperties(pd, &si, &sp);
            obeg(st[i].n); khex("features", sp.externalSemaphoreFeatures); oend();
        }
        struct { const char *n; VkExternalFenceHandleTypeFlagBits t; } ft[] = {
            { "fence_opaque_fd", VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT },
            { "fence_sync_fd", VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT } };
        for (int i = 0; i < 2; i++) {
            VkPhysicalDeviceExternalFenceInfo fi = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO, .handleType = ft[i].t };
            VkExternalFenceProperties fp = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES };
            vkGetPhysicalDeviceExternalFenceProperties(pd, &fi, &fp);
            obeg(ft[i].n); khex("features", fp.externalFenceFeatures); oend();
        }
        // image format props for AHB-backed RGBA8 with transfer dst
        VkPhysicalDeviceExternalImageFormatInfo eif = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO,
            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID };
        VkPhysicalDeviceImageFormatInfo2 ifi = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, .pNext = &eif,
            .format = VK_FORMAT_R8G8B8A8_UNORM, .type = VK_IMAGE_TYPE_2D, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
        VkExternalImageFormatProperties eifp = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES };
        VkAndroidHardwareBufferUsageANDROID ahbu = { .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_USAGE_ANDROID, .pNext = &eifp };
        VkImageFormatProperties2 ifp = { .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, .pNext = ext_ahb ? (void *)&ahbu : (void *)&eifp };
        r = vkGetPhysicalDeviceImageFormatProperties2(pd, &ifi, &ifp);
        obeg("image_rgba8_ahb"); kvk("result", r);
        khex("features", eifp.externalMemoryProperties.externalMemoryFeatures);
        if (ext_ahb) khex("ahb_usage", ahbu.androidHardwareBufferUsage);
        oend();
    }
    oend();

    // device
    uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties qf[16]; if (nq > 16) nq = 16; vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qf);
    uint32_t qfi = 0; for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfi = i; break; }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo dq = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = qfi, .queueCount = 1, .pQueuePriorities = &prio };
    const char *dext[8]; uint32_t nd = 0;
    if (ext_ahb) dext[nd++] = "VK_ANDROID_external_memory_android_hardware_buffer";
    if (ext_foreign) dext[nd++] = "VK_EXT_queue_family_foreign";
    if (ext_memfd) dext[nd++] = "VK_KHR_external_memory_fd";
    if (ext_dmabuf) dext[nd++] = "VK_EXT_external_memory_dma_buf";
    if (ext_semfd) dext[nd++] = "VK_KHR_external_semaphore_fd";
    if (ext_fencefd) dext[nd++] = "VK_KHR_external_fence_fd";
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &dq,
        .enabledExtensionCount = nd, .ppEnabledExtensionNames = dext };
    VkDevice dev;
    r = vkCreateDevice(pd, &dci, NULL, &dev);
    kvk("vkCreateDevice", r);
    if (r) { oend(); goto vk_done; }
#define DL(name) PFN_##name name = (PFN_##name)vkGetDeviceProcAddr(dev, #name)
    DL(vkGetDeviceQueue); DL(vkCreateImage); DL(vkAllocateMemory); DL(vkBindImageMemory); DL(vkCreateCommandPool);
    DL(vkAllocateCommandBuffers); DL(vkBeginCommandBuffer); DL(vkEndCommandBuffer); DL(vkCmdPipelineBarrier);
    DL(vkCmdClearColorImage); DL(vkQueueSubmit); DL(vkQueueWaitIdle); DL(vkDestroyImage); DL(vkFreeMemory);
    DL(vkGetAndroidHardwareBufferPropertiesANDROID); DL(vkGetMemoryFdKHR); DL(vkCreateBuffer);
    DL(vkGetBufferMemoryRequirements); DL(vkBindBufferMemory); DL(vkDestroyBuffer); DL(vkGetMemoryAndroidHardwareBufferANDROID);
    VkQueue q; vkGetDeviceQueue(dev, qfi, 0, &q);

    // --- AHB import ---
    obeg("ahb_import");
    AHardwareBuffer *imp = ahb_cpu ? ahb_cpu : ahb;
    kstr("buffer", ahb_cpu ? "R8G8B8A8_UNORM+CPU_READ_RARELY" : "R8G8B8A8_UNORM");
    if (!ext_ahb || !vkGetAndroidHardwareBufferPropertiesANDROID || !imp) { kstr("err", "ext/func/buffer missing"); oend(); goto memfd; }
    {
        VkAndroidHardwareBufferFormatPropertiesANDROID fp = { .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID };
        VkAndroidHardwareBufferPropertiesANDROID ap = { .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, .pNext = &fp };
        r = vkGetAndroidHardwareBufferPropertiesANDROID(dev, imp, &ap);
        kvk("vkGetAndroidHardwareBufferPropertiesANDROID", r);
        kint("allocationSize", ap.allocationSize); khex("memoryTypeBits", ap.memoryTypeBits);
        kint("format", fp.format); khex("externalFormat", fp.externalFormat); khex("formatFeatures", fp.formatFeatures);
        // also BGRA if allocated
        if (ahb_bgra) {
            VkAndroidHardwareBufferFormatPropertiesANDROID fp2 = { .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_FORMAT_PROPERTIES_ANDROID };
            VkAndroidHardwareBufferPropertiesANDROID ap2 = { .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID, .pNext = &fp2 };
            VkResult r2 = vkGetAndroidHardwareBufferPropertiesANDROID(dev, ahb_bgra, &ap2);
            obeg("bgra_props"); kvk("result", r2); kint("format", fp2.format); khex("externalFormat", fp2.externalFormat);
            khex("formatFeatures", fp2.formatFeatures); oend();
        }
        if (r) { oend(); goto memfd; }

        VkExternalMemoryImageCreateInfo emi = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID };
        VkImageCreateInfo ic = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &emi, .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        VkImage img;
        r = vkCreateImage(dev, &ic, NULL, &img);
        kvk("vkCreateImage", r);
        if (r) { oend(); goto memfd; }
        uint32_t mt = 0; while (mt < 32 && !(ap.memoryTypeBits & (1u << mt))) mt++;
        VkMemoryDedicatedAllocateInfo ded = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = img };
        VkImportAndroidHardwareBufferInfoANDROID iai = { .sType = VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID,
            .pNext = &ded, .buffer = imp };
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &iai,
            .allocationSize = ap.allocationSize, .memoryTypeIndex = mt };
        VkDeviceMemory mem;
        r = vkAllocateMemory(dev, &mai, NULL, &mem);
        kvk("vkAllocateMemory_import", r); kint("memoryTypeIndex", mt);
        if (r) { vkDestroyImage(dev, img, NULL); oend(); goto memfd; }
        r = vkBindImageMemory(dev, img, mem, 0);
        kvk("vkBindImageMemory", r);
        if (r) { oend(); goto memfd; }

        // clear via cmd buffer
        VkCommandPoolCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = qfi };
        VkCommandPool pool; vkCreateCommandPool(dev, &cpi, NULL, &pool);
        VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VkCommandBuffer cb; vkAllocateCommandBuffers(dev, &cbai, &cb);
        VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        vkBeginCommandBuffer(cb, &bi);
        VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        uint32_t src_qf = ext_foreign ? VK_QUEUE_FAMILY_FOREIGN_EXT : VK_QUEUE_FAMILY_EXTERNAL;
        VkImageMemoryBarrier b1 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .srcQueueFamilyIndex = src_qf, .dstQueueFamilyIndex = qfi,
            .image = img, .subresourceRange = rng };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
        VkClearColorValue cc = { .float32 = { 1.0f, 0.0f, 1.0f, 1.0f } }; // magenta
        vkCmdClearColorImage(cb, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cc, 1, &rng);
        VkImageMemoryBarrier b2 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = 0, .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = qfi, .dstQueueFamilyIndex = src_qf, .image = img, .subresourceRange = rng };
        vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
        r = vkEndCommandBuffer(cb); kvk("vkEndCommandBuffer", r);
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
        r = vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE); kvk("vkQueueSubmit", r);
        VkResult rw = vkQueueWaitIdle(q); kvk("vkQueueWaitIdle", rw);
        int clear_ok = (r == 0 && rw == 0);
        kbool("clear_ok", clear_ok);
        // CPU readback through gralloc to prove the same memory was written
        if (ahb_cpu && clear_ok) {
            void *ptr = NULL;
            int lr = AHardwareBuffer_lock(ahb_cpu, AHARDWAREBUFFER_USAGE_CPU_READ_RARELY, -1, NULL, &ptr);
            kint("cpu_lock_ret", lr);
            if (lr == 0 && ptr) {
                AHardwareBuffer_Desc g; AHardwareBuffer_describe(ahb_cpu, &g);
                uint32_t *px = ptr;
                khex("pixel_0_0", px[0]);
                khex("pixel_mid", px[(H / 2) * g.stride + W / 2]);
                kbool("readback_magenta", px[0] == 0xffff00ffu && px[(H / 2) * g.stride + W / 2] == 0xffff00ffu);
                AHardwareBuffer_unlock(ahb_cpu, NULL);
            }
        }
        vk_import_ok = clear_ok;
        // export Vulkan-allocated memory as AHB (reverse direction)
        if (vkGetMemoryAndroidHardwareBufferANDROID) {
            VkExternalMemoryImageCreateInfo emi2 = emi;
            VkImageCreateInfo ic2 = ic; ic2.pNext = &emi2;
            VkImage img2; VkResult re = vkCreateImage(dev, &ic2, NULL, &img2);
            obeg("export_vk_to_ahb"); kvk("vkCreateImage", re);
            if (!re) {
                VkMemoryDedicatedAllocateInfo ded2 = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = img2 };
                VkExportMemoryAllocateInfo exa = { .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .pNext = &ded2,
                    .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID };
                VkMemoryAllocateInfo ma2 = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &exa, .allocationSize = 0, .memoryTypeIndex = mt };
                VkDeviceMemory m2; re = vkAllocateMemory(dev, &ma2, NULL, &m2); kvk("vkAllocateMemory", re);
                if (!re) {
                    VkMemoryGetAndroidHardwareBufferInfoANDROID gi = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, .memory = m2 };
                    // must be bound before export for dedicated image allocs
                    vkBindImageMemory(dev, img2, m2, 0);
                    AHardwareBuffer *out = NULL; re = vkGetMemoryAndroidHardwareBufferANDROID(dev, &gi, &out);
                    kvk("vkGetMemoryAndroidHardwareBufferANDROID", re);
                    if (!re && out) { AHardwareBuffer_Desc g; AHardwareBuffer_describe(out, &g);
                        kint("w", g.width); kint("h", g.height); kint("format", g.format); khex("usage", g.usage); AHardwareBuffer_release(out); }
                    vkFreeMemory(dev, m2, NULL);
                }
                vkDestroyImage(dev, img2, NULL);
            }
            oend();
        }
        vkDestroyImage(dev, img, NULL); vkFreeMemory(dev, mem, NULL);
    }
    oend();

memfd:
    // --- exportable alloc -> vkGetMemoryFdKHR ---
    obeg("memory_fd");
    if (!ext_memfd || !vkGetMemoryFdKHR) { kstr("err", "VK_KHR_external_memory_fd missing"); oend(); goto vk_end; }
    {
        struct { const char *n; VkExternalMemoryHandleTypeFlagBits t; int en; } ht[] = {
            { "opaque_fd", VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT, 1 },
            { "dma_buf", VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, ext_dmabuf } };
        VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        for (int i = 0; i < 2; i++) {
            obeg(ht[i].n);
            if (!ht[i].en) { kstr("err", "ext missing"); oend(); continue; }
            VkExternalMemoryBufferCreateInfo ebi = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = ht[i].t };
            VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ebi, .size = 1 << 20,
                .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
            VkBuffer buf; VkResult rb = vkCreateBuffer(dev, &bci, NULL, &buf); kvk("vkCreateBuffer", rb);
            if (rb) { mfd_res[i] = rb; oend(); continue; }
            VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf, &mr);
            uint32_t mt = 0;
            for (mt = 0; mt < mp.memoryTypeCount; mt++)
                if ((mr.memoryTypeBits & (1u << mt)) && (mp.memoryTypes[mt].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) break;
            if (mt == mp.memoryTypeCount) for (mt = 0; mt < 32 && !(mr.memoryTypeBits & (1u << mt)); mt++);
            VkExportMemoryAllocateInfo exa = { .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, .handleTypes = ht[i].t };
            VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &exa, .allocationSize = mr.size, .memoryTypeIndex = mt };
            VkDeviceMemory m; VkResult ra = vkAllocateMemory(dev, &ma, NULL, &m); kvk("vkAllocateMemory", ra); kint("memoryTypeIndex", mt);
            if (ra) mfd_res[i] = ra;
            if (!ra) {
                vkBindBufferMemory(dev, buf, m, 0);
                VkMemoryGetFdInfoKHR gfi = { .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, .memory = m, .handleType = ht[i].t };
                int fd = -1; VkResult rf = vkGetMemoryFdKHR(dev, &gfi, &fd); kvk("vkGetMemoryFdKHR", rf); kint("fd", fd);
                mfd_res[i] = rf;
                if (!rf && fd >= 0) { char l[256] = {0}, pp[64]; snprintf(pp, sizeof pp, "/proc/self/fd/%d", fd);
                    if (readlink(pp, l, sizeof l - 1) > 0 && strstr(l, "dmabuf")) mfd_dmabuf[i] = 1;
                    fd_info("fd_info", fd); close(fd); }
                vkFreeMemory(dev, m, NULL);
            }
            vkDestroyBuffer(dev, buf, NULL);
            oend();
        }
        // import a dma_heap/system dmabuf into Vulkan (guest-allocated buffer path)
        DL(vkGetMemoryFdPropertiesKHR);
        obeg("dma_heap_import");
        if (!ext_dmabuf || !vkGetMemoryFdPropertiesKHR) { kstr("err", "ext missing"); }
        else {
            int hfd = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
            kbool("heap_open", hfd >= 0);
            if (hfd >= 0) {
                struct dma_heap_allocation_data ad = { .len = 1 << 20, .fd_flags = O_RDWR | O_CLOEXEC };
                int ir = ioctl(hfd, DMA_HEAP_IOCTL_ALLOC, &ad); int er = errno;
                kint("alloc_ioctl", ir); if (ir) kint("alloc_errno", er);
                close(hfd);
                if (!ir) {
                    int dfd = (int)ad.fd;
                    void *mp2 = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_SHARED, dfd, 0);
                    kbool("cpu_mmap_ok", mp2 != MAP_FAILED);
                    if (mp2 != MAP_FAILED) { memset(mp2, 0xab, 4096); munmap(mp2, 1 << 20); }
                    VkMemoryFdPropertiesKHR mfp = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
                    VkResult rp = vkGetMemoryFdPropertiesKHR(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, dfd, &mfp);
                    kvk("vkGetMemoryFdPropertiesKHR", rp); khex("memoryTypeBits", mfp.memoryTypeBits);
                    VkExternalMemoryBufferCreateInfo ebi = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
                    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ebi, .size = 1 << 20,
                        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT };
                    VkBuffer buf; VkResult rb = vkCreateBuffer(dev, &bci, NULL, &buf); kvk("vkCreateBuffer", rb);
                    if (!rb && !rp && mfp.memoryTypeBits) {
                        VkMemoryRequirements mr; vkGetBufferMemoryRequirements(dev, buf, &mr);
                        uint32_t bits = mfp.memoryTypeBits & mr.memoryTypeBits, mt = 0;
                        while (mt < 32 && !(bits & (1u << mt))) mt++;
                        kint("memoryTypeIndex", mt);
                        VkImportMemoryFdInfoKHR imi = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
                            .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = dup(dfd) };
                        VkMemoryAllocateInfo ma = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imi,
                            .allocationSize = mr.size, .memoryTypeIndex = mt };
                        VkDeviceMemory m; VkResult ra = vkAllocateMemory(dev, &ma, NULL, &m);
                        kvk("vkAllocateMemory_import", ra); dmabuf_import_res = ra;
                        if (ra) close(imi.fd);
                        else {
                            VkResult rbb = vkBindBufferMemory(dev, buf, m, 0); kvk("vkBindBufferMemory", rbb);
                            if (rbb) dmabuf_import_res = rbb;
                            vkFreeMemory(dev, m, NULL);
                        }
                    }
                    if (!rb) vkDestroyBuffer(dev, buf, NULL);
                    close(dfd);
                }
            }
        }
        oend();
    }
    oend();
vk_end:
    oend(); // vk
    }
vk_done:
    // top-level summary keys (accept script)
    kbool("vk_ahb_import", vk_import_ok);
    kbool("vk_ext_dma_buf", ext_dmabuf);
    obeg("vk_memory_fd");
    kint("opaque_fd_result", mfd_res[0]); kbool("opaque_fd_is_dmabuf", mfd_dmabuf[0]);
    kint("dma_buf_result", mfd_res[1]); kbool("dma_buf_is_dmabuf", mfd_dmabuf[1]);
    kint("dma_heap_import_result", dmabuf_import_res);
    kstr("legend", "result: VkResult, 99=not attempted");
    oend();
    fputs("\n}\n", o);
    if (ahb) AHardwareBuffer_release(ahb);
    if (ahb_bgra) AHardwareBuffer_release(ahb_bgra);
    if (ahb_cpu) AHardwareBuffer_release(ahb_cpu);
    if (ahb_x) AHardwareBuffer_release(ahb_x);
    if (ahb_565) AHardwareBuffer_release(ahb_565);
    if (ahb_f16) AHardwareBuffer_release(ahb_f16);
    if (o != stdout) fclose(o);
    return 0;
}
