/* LodOS guest glibc Vulkan ICD v0 (Loader-ICD interface v5).
 * Forwards Vulkan calls over vk-bridge (memfd SPSC rings + AF_UNIX SCM_RIGHTS) to host vkb_server.
 * Dispatchable handles wrap u64 bridge IDs in heap structs with VK_LOADER_DATA at offset 0 (R12). */
#define _GNU_SOURCE
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_icd.h>
#include "vkbc.h"

#define ICD_IFACE_VERSION 5u

typedef struct icd_instance icd_instance;
typedef struct icd_pdev icd_pdev;
typedef struct icd_device icd_device;
typedef struct icd_queue icd_queue;
typedef struct icd_cmdbuf icd_cmdbuf;

struct icd_pdev {
    VK_LOADER_DATA loader_data;
    uint64_t id;
    icd_instance *inst;
};

struct icd_instance {
    VK_LOADER_DATA loader_data;
    uint64_t id;
    icd_pdev *pdevs;
    uint32_t npdevs;
    int pdevs_queried;
};

struct icd_queue {
    VK_LOADER_DATA loader_data;
    uint64_t id;
    uint32_t qfi, qi;
    icd_queue *next;
};

struct icd_cmdbuf {
    VK_LOADER_DATA loader_data;
    uint64_t id;
    VkCommandPool pool;
    icd_cmdbuf *prev, *next;
};

struct icd_device {
    VK_LOADER_DATA loader_data;
    uint64_t id;
    icd_pdev *pdev;
    icd_queue *queues;
    icd_cmdbuf *cmdbufs;
};

_Static_assert(offsetof(icd_instance, loader_data) == 0, "VK_LOADER_DATA must be at offset 0");
_Static_assert(offsetof(icd_pdev, loader_data) == 0, "VK_LOADER_DATA must be at offset 0");
_Static_assert(offsetof(icd_device, loader_data) == 0, "VK_LOADER_DATA must be at offset 0");
_Static_assert(offsetof(icd_queue, loader_data) == 0, "VK_LOADER_DATA must be at offset 0");
_Static_assert(offsetof(icd_cmdbuf, loader_data) == 0, "VK_LOADER_DATA must be at offset 0");

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static vkbc *g_conn;

static vkbc *icd_conn_locked(void) {
    if (!g_conn) {
        const char *sock = getenv("LODOS_VK_SOCK");
        if (!sock || !*sock) sock = "@lodos-vk-g";
        g_conn = vkbc_connect(sock);
    }
    return g_conn;
}

__attribute__((destructor)) static void icd_fini(void) {
    pthread_mutex_lock(&g_mu);
    if (g_conn) {
        vkbc_disconnect(g_conn);
        g_conn = NULL;
    }
    pthread_mutex_unlock(&g_mu);
}

static void log_missing(const char *where, const char *name) {
    static int checked, enabled;
    if (!checked) {
        const char *e = getenv("LODOS_VK_LOG_MISSING");
        enabled = e && e[0] == '1';
        checked = 1;
    }
    if (enabled && name) fprintf(stderr, "[lodos-icd] missing %s: %s\n", where, name);
}

/* Instance extensions whose entrypoints this v0 ICD supports (WSI comes in S2). */
static int icd_inst_ext_supported(const char *name) {
    static const char *const k_exts[] = {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_EXTERNAL_MEMORY_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_SEMAPHORE_CAPABILITIES_EXTENSION_NAME,
        VK_KHR_EXTERNAL_FENCE_CAPABILITIES_EXTENSION_NAME,
    };
    for (size_t i = 0; i < sizeof(k_exts) / sizeof(k_exts[0]); i++)
        if (strcmp(name, k_exts[i]) == 0) return 1;
    return 0;
}

/* Extensions terminated by the guest Khronos loader itself (strip before host CreateInstance). */
static int icd_loader_only_ext(const char *name) {
    static const char *const k_ldr[] = {
        "VK_EXT_debug_report",
        "VK_EXT_debug_utils",
        "VK_EXT_layer_settings",
        "VK_KHR_portability_enumeration",
        "VK_LUNARG_direct_driver_loading",
    };
    for (size_t i = 0; i < sizeof(k_ldr) / sizeof(k_ldr[0]); i++)
        if (strcmp(name, k_ldr[i]) == 0) return 1;
    return 0;
}

/* ---- Global & Instance commands ---- */

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkEnumerateInstanceVersion(uint32_t *pApiVersion) {
    if (!pApiVersion) return VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    VkResult r = c ? vkbc_vkEnumerateInstanceVersion(c, pApiVersion) : VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_unlock(&g_mu);
    if (r == VK_SUCCESS && *pApiVersion > VK_API_VERSION_1_3)
        *pApiVersion = VK_MAKE_API_VERSION(0, 1, 3, VK_API_VERSION_PATCH(*pApiVersion));
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkEnumerateInstanceLayerProperties(
    uint32_t *pPropertyCount, VkLayerProperties *pProperties) {
    (void)pProperties;
    if (pPropertyCount) *pPropertyCount = 0;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkEnumerateInstanceExtensionProperties(
    const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties) {
    if (pLayerName) return VK_ERROR_LAYER_NOT_PRESENT;
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;

    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (!c) { pthread_mutex_unlock(&g_mu); return VK_ERROR_INITIALIZATION_FAILED; }
    uint32_t raw_n = 0;
    VkResult r = vkbc_vkEnumerateInstanceExtensionProperties(c, NULL, &raw_n, NULL);
    VkExtensionProperties *raw = NULL;
    if (r == VK_SUCCESS && raw_n > 0) {
        raw = calloc(raw_n, sizeof(*raw));
        if (!raw) { pthread_mutex_unlock(&g_mu); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        r = vkbc_vkEnumerateInstanceExtensionProperties(c, NULL, &raw_n, raw);
    }
    pthread_mutex_unlock(&g_mu);
    if (r < 0) { free(raw); return r; }

    VkExtensionProperties filt[16];
    uint32_t fn = 0;
    for (uint32_t i = 0; i < raw_n && fn < 16; i++)
        if (icd_inst_ext_supported(raw[i].extensionName))
            filt[fn++] = raw[i];
    free(raw);

    if (!pProperties) {
        *pPropertyCount = fn;
        return VK_SUCCESS;
    }
    uint32_t copy_n = *pPropertyCount < fn ? *pPropertyCount : fn;
    if (copy_n) memcpy(pProperties, filt, copy_n * sizeof(filt[0]));
    *pPropertyCount = copy_n;
    return copy_n < fn ? VK_INCOMPLETE : VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkCreateInstance(
    const VkInstanceCreateInfo *pCreateInfo, const VkAllocationCallbacks *pAllocator, VkInstance *pInstance) {
    if (!pCreateInfo || !pInstance) return VK_ERROR_INITIALIZATION_FAILED;

    const char **exts = NULL;
    uint32_t ext_n = 0;
    if (pCreateInfo->enabledExtensionCount > 0) {
        exts = calloc(pCreateInfo->enabledExtensionCount, sizeof(*exts));
        if (!exts) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
            const char *en = pCreateInfo->ppEnabledExtensionNames[i];
            if (icd_inst_ext_supported(en)) exts[ext_n++] = en;
            else if (!icd_loader_only_ext(en)) { free(exts); return VK_ERROR_EXTENSION_NOT_PRESENT; }
        }
    }

    VkApplicationInfo app;
    VkInstanceCreateInfo ci = *pCreateInfo;
    ci.pNext = NULL; /* strip guest loader VkLayerInstanceCreateInfo + debug callbacks */
    ci.flags &= ~VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    if (ci.pApplicationInfo) {
        app = *ci.pApplicationInfo;
        app.pNext = NULL;
        ci.pApplicationInfo = &app;
    }
    ci.enabledLayerCount = 0;
    ci.ppEnabledLayerNames = NULL;
    ci.enabledExtensionCount = ext_n;
    ci.ppEnabledExtensionNames = ext_n ? exts : NULL;

    icd_instance *inst = calloc(1, sizeof(*inst));
    if (!inst) { free(exts); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    set_loader_magic_value(inst);

    VkInstance raw_inst = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    VkResult r = c ? vkbc_vkCreateInstance(c, &ci, pAllocator, &raw_inst) : VK_ERROR_INITIALIZATION_FAILED;
    pthread_mutex_unlock(&g_mu);
    free(exts);
    if (r != VK_SUCCESS) { free(inst); return r; }

    inst->id = (uint64_t)(uintptr_t)raw_inst;
    *pInstance = (VkInstance)inst;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL icd_vkDestroyInstance(
    VkInstance instance, const VkAllocationCallbacks *pAllocator) {
    if (!instance) return;
    icd_instance *inst = (icd_instance *)instance;
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkDestroyInstance(c, (VkInstance)(uintptr_t)inst->id, pAllocator);
    pthread_mutex_unlock(&g_mu);
    free(inst->pdevs);
    free(inst);
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkEnumeratePhysicalDevices(
    VkInstance instance, uint32_t *pPhysicalDeviceCount, VkPhysicalDevice *pPhysicalDevices) {
    if (!instance || !pPhysicalDeviceCount) return VK_ERROR_INITIALIZATION_FAILED;
    icd_instance *inst = (icd_instance *)instance;

    pthread_mutex_lock(&g_mu);
    if (!inst->pdevs_queried) {
        vkbc *c = icd_conn_locked();
        if (!c) { pthread_mutex_unlock(&g_mu); return VK_ERROR_INITIALIZATION_FAILED; }
        uint32_t n = 0;
        VkResult r = vkbc_vkEnumeratePhysicalDevices(c, (VkInstance)(uintptr_t)inst->id, &n, NULL);
        if (r < 0) { pthread_mutex_unlock(&g_mu); return r; }
        if (n > 0) {
            VkPhysicalDevice *raw = calloc(n, sizeof(*raw));
            icd_pdev *pds = calloc(n, sizeof(*pds));
            if (!raw || !pds) {
                free(raw); free(pds);
                pthread_mutex_unlock(&g_mu);
                return VK_ERROR_OUT_OF_HOST_MEMORY;
            }
            r = vkbc_vkEnumeratePhysicalDevices(c, (VkInstance)(uintptr_t)inst->id, &n, raw);
            if (r < 0) {
                free(raw); free(pds);
                pthread_mutex_unlock(&g_mu);
                return r;
            }
            for (uint32_t i = 0; i < n; i++) {
                set_loader_magic_value(&pds[i]);
                pds[i].id = (uint64_t)(uintptr_t)raw[i];
                pds[i].inst = inst;
            }
            free(raw);
            inst->pdevs = pds;
            inst->npdevs = n;
        }
        inst->pdevs_queried = 1;
    }
    pthread_mutex_unlock(&g_mu);

    if (!pPhysicalDevices) {
        *pPhysicalDeviceCount = inst->npdevs;
        return VK_SUCCESS;
    }
    uint32_t m = *pPhysicalDeviceCount < inst->npdevs ? *pPhysicalDeviceCount : inst->npdevs;
    for (uint32_t i = 0; i < m; i++)
        pPhysicalDevices[i] = (VkPhysicalDevice)&inst->pdevs[i];
    *pPhysicalDeviceCount = m;
    return m < inst->npdevs ? VK_INCOMPLETE : VK_SUCCESS;
}

/* ---- PhysicalDevice commands ---- */

#define PD_RAW(pd) ((VkPhysicalDevice)(uintptr_t)((icd_pdev *)(pd))->id)

static VKAPI_ATTR void VKAPI_CALL icd_vkGetPhysicalDeviceProperties(
    VkPhysicalDevice pd, VkPhysicalDeviceProperties *pProperties) {
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkGetPhysicalDeviceProperties(c, PD_RAW(pd), pProperties);
    pthread_mutex_unlock(&g_mu);
}

static VKAPI_ATTR void VKAPI_CALL icd_vkGetPhysicalDeviceFeatures(
    VkPhysicalDevice pd, VkPhysicalDeviceFeatures *pFeatures) {
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkGetPhysicalDeviceFeatures(c, PD_RAW(pd), pFeatures);
    pthread_mutex_unlock(&g_mu);
}

static VKAPI_ATTR void VKAPI_CALL icd_vkGetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties *pMemoryProperties) {
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkGetPhysicalDeviceMemoryProperties(c, PD_RAW(pd), pMemoryProperties);
    pthread_mutex_unlock(&g_mu);
}

static VKAPI_ATTR void VKAPI_CALL icd_vkGetPhysicalDeviceQueueFamilyProperties(
    VkPhysicalDevice pd, uint32_t *pCount, VkQueueFamilyProperties *pProps) {
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkGetPhysicalDeviceQueueFamilyProperties(c, PD_RAW(pd), pCount, pProps);
    pthread_mutex_unlock(&g_mu);
}

static VKAPI_ATTR void VKAPI_CALL icd_vkGetPhysicalDeviceFormatProperties(
    VkPhysicalDevice pd, VkFormat format, VkFormatProperties *pFormatProperties) {
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkGetPhysicalDeviceFormatProperties(c, PD_RAW(pd), format, pFormatProperties);
    pthread_mutex_unlock(&g_mu);
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkGetPhysicalDeviceImageFormatProperties(
    VkPhysicalDevice pd, VkFormat format, VkImageType type, VkImageTiling tiling,
    VkImageUsageFlags usage, VkImageCreateFlags flags, VkImageFormatProperties *pProps) {
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    VkResult r = c ? vkbc_vkGetPhysicalDeviceImageFormatProperties(c, PD_RAW(pd), format, type, tiling, usage, flags, pProps)
                   : VK_ERROR_DEVICE_LOST;
    pthread_mutex_unlock(&g_mu);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL icd_vkGetPhysicalDeviceSparseImageFormatProperties(
    VkPhysicalDevice pd, VkFormat format, VkImageType type, VkSampleCountFlagBits samples,
    VkImageUsageFlags usage, VkImageTiling tiling, uint32_t *pCount, VkSparseImageFormatProperties *pProps) {
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkGetPhysicalDeviceSparseImageFormatProperties(c, PD_RAW(pd), format, type, samples, usage, tiling, pCount, pProps);
    pthread_mutex_unlock(&g_mu);
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkEnumerateDeviceLayerProperties(
    VkPhysicalDevice pd, uint32_t *pPropertyCount, VkLayerProperties *pProperties) {
    (void)pd; (void)pProperties;
    if (pPropertyCount) *pPropertyCount = 0;
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkEnumerateDeviceExtensionProperties(
    VkPhysicalDevice pd, const char *pLayerName, uint32_t *pPropertyCount, VkExtensionProperties *pProperties) {
    if (pLayerName) return VK_ERROR_LAYER_NOT_PRESENT;
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    VkResult r = c ? vkbc_vkEnumerateDeviceExtensionProperties(c, PD_RAW(pd), NULL, pPropertyCount, pProperties)
                   : VK_ERROR_DEVICE_LOST;
    pthread_mutex_unlock(&g_mu);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL icd_vkGetPhysicalDeviceProperties2(
    VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *pProperties) {
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkGetPhysicalDeviceProperties2(c, PD_RAW(pd), pProperties);
    pthread_mutex_unlock(&g_mu);
}

static VKAPI_ATTR void VKAPI_CALL icd_vkGetPhysicalDeviceFeatures2(
    VkPhysicalDevice pd, VkPhysicalDeviceFeatures2 *pFeatures) {
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkGetPhysicalDeviceFeatures2(c, PD_RAW(pd), pFeatures);
    pthread_mutex_unlock(&g_mu);
}

static VKAPI_ATTR void VKAPI_CALL icd_vkGetPhysicalDeviceMemoryProperties2(
    VkPhysicalDevice pd, VkPhysicalDeviceMemoryProperties2 *pMemoryProperties) {
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkGetPhysicalDeviceMemoryProperties2(c, PD_RAW(pd), pMemoryProperties);
    pthread_mutex_unlock(&g_mu);
}

static VKAPI_ATTR void VKAPI_CALL icd_vkGetPhysicalDeviceFormatProperties2(
    VkPhysicalDevice pd, VkFormat format, VkFormatProperties2 *pFormatProperties) {
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkGetPhysicalDeviceFormatProperties2(c, PD_RAW(pd), format, pFormatProperties);
    pthread_mutex_unlock(&g_mu);
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkGetPhysicalDeviceImageFormatProperties2(
    VkPhysicalDevice pd, const VkPhysicalDeviceImageFormatInfo2 *pInfo, VkImageFormatProperties2 *pProps) {
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    VkResult r = c ? vkbc_vkGetPhysicalDeviceImageFormatProperties2(c, PD_RAW(pd), pInfo, pProps)
                   : VK_ERROR_DEVICE_LOST;
    pthread_mutex_unlock(&g_mu);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL icd_vkGetPhysicalDeviceQueueFamilyProperties2(
    VkPhysicalDevice pd, uint32_t *pCount, VkQueueFamilyProperties2 *pProps) {
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkGetPhysicalDeviceQueueFamilyProperties2(c, PD_RAW(pd), pCount, pProps);
    pthread_mutex_unlock(&g_mu);
}

static VKAPI_ATTR void VKAPI_CALL icd_vkGetPhysicalDeviceSparseImageFormatProperties2(
    VkPhysicalDevice pd, const VkPhysicalDeviceSparseImageFormatInfo2 *pInfo,
    uint32_t *pCount, VkSparseImageFormatProperties2 *pProps) {
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkGetPhysicalDeviceSparseImageFormatProperties2(c, PD_RAW(pd), pInfo, pCount, pProps);
    pthread_mutex_unlock(&g_mu);
}

static VKAPI_ATTR void VKAPI_CALL icd_vkGetPhysicalDeviceExternalBufferProperties(
    VkPhysicalDevice pd, const VkPhysicalDeviceExternalBufferInfo *pInfo, VkExternalBufferProperties *pProps) {
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkGetPhysicalDeviceExternalBufferProperties(c, PD_RAW(pd), pInfo, pProps);
    pthread_mutex_unlock(&g_mu);
}

static VKAPI_ATTR void VKAPI_CALL icd_vkGetPhysicalDeviceExternalFenceProperties(
    VkPhysicalDevice pd, const VkPhysicalDeviceExternalFenceInfo *pInfo, VkExternalFenceProperties *pProps) {
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkGetPhysicalDeviceExternalFenceProperties(c, PD_RAW(pd), pInfo, pProps);
    pthread_mutex_unlock(&g_mu);
}

static VKAPI_ATTR void VKAPI_CALL icd_vkGetPhysicalDeviceExternalSemaphoreProperties(
    VkPhysicalDevice pd, const VkPhysicalDeviceExternalSemaphoreInfo *pInfo, VkExternalSemaphoreProperties *pProps) {
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkGetPhysicalDeviceExternalSemaphoreProperties(c, PD_RAW(pd), pInfo, pProps);
    pthread_mutex_unlock(&g_mu);
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkGetPhysicalDeviceToolProperties(
    VkPhysicalDevice pd, uint32_t *pToolCount, VkPhysicalDeviceToolProperties *pToolProperties) {
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    VkResult r = c ? vkbc_vkGetPhysicalDeviceToolProperties(c, PD_RAW(pd), pToolCount, pToolProperties)
                   : VK_ERROR_DEVICE_LOST;
    pthread_mutex_unlock(&g_mu);
    return r;
}

/* ---- Device commands ---- */

#define DEV_RAW(d) ((VkDevice)(uintptr_t)((icd_device *)(d))->id)
#define QUEUE_RAW(q) ((VkQueue)(uintptr_t)((icd_queue *)(q))->id)
#define CMDBUF_RAW(cb) ((cb) ? (VkCommandBuffer)(uintptr_t)((icd_cmdbuf *)(cb))->id : VK_NULL_HANDLE)

static void cmdbuf_unlink_free(icd_device *dev, icd_cmdbuf *cb) {
    if (cb->prev) cb->prev->next = cb->next;
    else dev->cmdbufs = cb->next;
    if (cb->next) cb->next->prev = cb->prev;
    free(cb);
}

#define ICD_CALL_RES(expr) do { \
    pthread_mutex_lock(&g_mu); \
    vkbc *c_ = icd_conn_locked(); \
    VkResult r_ = c_ ? (expr) : VK_ERROR_DEVICE_LOST; \
    pthread_mutex_unlock(&g_mu); \
    return r_; \
} while (0)

#define ICD_CALL_VOID(expr) do { \
    pthread_mutex_lock(&g_mu); \
    vkbc *c_ = icd_conn_locked(); \
    if (c_) (void)(expr); \
    pthread_mutex_unlock(&g_mu); \
} while (0)

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkCreateDevice(
    VkPhysicalDevice pd, const VkDeviceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
    if (!pd || !pCreateInfo || !pDevice) return VK_ERROR_INITIALIZATION_FAILED;

    VkDeviceCreateInfo dci = *pCreateInfo;
    /* Extract VkPhysicalDeviceFeatures2 if passed in pNext; strip guest loader VkLayerDeviceCreateInfo */
    for (const VkBaseInStructure *s = (const VkBaseInStructure *)pCreateInfo->pNext; s; s = s->pNext) {
        if (s->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 && !dci.pEnabledFeatures)
            dci.pEnabledFeatures = &((const VkPhysicalDeviceFeatures2 *)s)->features;
    }
    dci.pNext = NULL;
    dci.enabledLayerCount = 0;
    dci.ppEnabledLayerNames = NULL;

    VkDeviceQueueCreateInfo *qcis = NULL;
    if (dci.queueCreateInfoCount > 0 && dci.pQueueCreateInfos) {
        qcis = calloc(dci.queueCreateInfoCount, sizeof(*qcis));
        if (!qcis) return VK_ERROR_OUT_OF_HOST_MEMORY;
        for (uint32_t i = 0; i < dci.queueCreateInfoCount; i++) {
            qcis[i] = dci.pQueueCreateInfos[i];
            qcis[i].pNext = NULL;
        }
        dci.pQueueCreateInfos = qcis;
    }

    icd_device *dev = calloc(1, sizeof(*dev));
    if (!dev) { free(qcis); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    set_loader_magic_value(dev);

    VkDevice raw_dev = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    VkResult r = c ? vkbc_vkCreateDevice(c, PD_RAW(pd), &dci, pAllocator, &raw_dev) : VK_ERROR_DEVICE_LOST;
    pthread_mutex_unlock(&g_mu);
    free(qcis);
    if (r != VK_SUCCESS) { free(dev); return r; }

    dev->id = (uint64_t)(uintptr_t)raw_dev;
    dev->pdev = (icd_pdev *)pd;
    *pDevice = (VkDevice)dev;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL icd_vkDestroyDevice(
    VkDevice device, const VkAllocationCallbacks *pAllocator) {
    if (!device) return;
    icd_device *dev = (icd_device *)device;
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkDestroyDevice(c, DEV_RAW(device), pAllocator);
    while (dev->cmdbufs) cmdbuf_unlink_free(dev, dev->cmdbufs);
    pthread_mutex_unlock(&g_mu);
    icd_queue *q = dev->queues;
    while (q) {
        icd_queue *nx = q->next;
        free(q);
        q = nx;
    }
    free(dev);
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkDeviceWaitIdle(VkDevice device) {
    ICD_CALL_RES(vkbc_vkDeviceWaitIdle(c_, DEV_RAW(device)));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkGetDeviceQueue(
    VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue) {
    if (!device || !pQueue) return;
    icd_device *dev = (icd_device *)device;
    pthread_mutex_lock(&g_mu);
    for (icd_queue *q = dev->queues; q; q = q->next) {
        if (q->qfi == queueFamilyIndex && q->qi == queueIndex) {
            *pQueue = (VkQueue)q;
            pthread_mutex_unlock(&g_mu);
            return;
        }
    }
    vkbc *c = icd_conn_locked();
    VkQueue raw_q = VK_NULL_HANDLE;
    VkResult r = c ? vkbc_vkGetDeviceQueue(c, DEV_RAW(device), queueFamilyIndex, queueIndex, &raw_q)
                   : VK_ERROR_DEVICE_LOST;
    if (r == VK_SUCCESS && raw_q) {
        icd_queue *q = calloc(1, sizeof(*q));
        if (q) {
            set_loader_magic_value(q);
            q->id = (uint64_t)(uintptr_t)raw_q;
            q->qfi = queueFamilyIndex;
            q->qi = queueIndex;
            q->next = dev->queues;
            dev->queues = q;
            *pQueue = (VkQueue)q;
            pthread_mutex_unlock(&g_mu);
            return;
        }
    }
    *pQueue = VK_NULL_HANDLE;
    pthread_mutex_unlock(&g_mu);
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkQueueSubmit(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence) {
    if (!queue) return VK_ERROR_DEVICE_LOST;
    VkSubmitInfo *subs = NULL;
    VkCommandBuffer *cbs = NULL;
    if (submitCount > 0 && pSubmits) {
        uint32_t total_cb = 0;
        for (uint32_t i = 0; i < submitCount; i++) total_cb += pSubmits[i].commandBufferCount;
        subs = calloc(submitCount, sizeof(*subs));
        if (!subs) return VK_ERROR_OUT_OF_HOST_MEMORY;
        if (total_cb > 0) {
            cbs = calloc(total_cb, sizeof(*cbs));
            if (!cbs) { free(subs); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        }
        uint32_t off = 0;
        for (uint32_t i = 0; i < submitCount; i++) {
            subs[i] = pSubmits[i];
            subs[i].pNext = NULL;
            if (subs[i].commandBufferCount > 0 && subs[i].pCommandBuffers) {
                for (uint32_t j = 0; j < subs[i].commandBufferCount; j++)
                    cbs[off + j] = CMDBUF_RAW(subs[i].pCommandBuffers[j]);
                subs[i].pCommandBuffers = &cbs[off];
                off += subs[i].commandBufferCount;
            }
        }
    }
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    VkResult r = c ? vkbc_vkQueueSubmit(c, QUEUE_RAW(queue), submitCount, subs, fence) : VK_ERROR_DEVICE_LOST;
    pthread_mutex_unlock(&g_mu);
    free(cbs);
    free(subs);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkQueueWaitIdle(VkQueue queue) {
    if (!queue) return VK_ERROR_DEVICE_LOST;
    ICD_CALL_RES(vkbc_vkQueueWaitIdle(c_, QUEUE_RAW(queue)));
}

/* ---- Sync: Fence & Semaphore ---- */

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkCreateFence(
    VkDevice device, const VkFenceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkFence *pFence) {
    if (!pCreateInfo) return VK_ERROR_INITIALIZATION_FAILED;
    VkFenceCreateInfo ci = *pCreateInfo;
    ci.pNext = NULL;
    ICD_CALL_RES(vkbc_vkCreateFence(c_, DEV_RAW(device), &ci, pAllocator, pFence));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkDestroyFence(
    VkDevice device, VkFence fence, const VkAllocationCallbacks *pAllocator) {
    if (!fence) return;
    ICD_CALL_VOID(vkbc_vkDestroyFence(c_, DEV_RAW(device), fence, pAllocator));
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkResetFences(
    VkDevice device, uint32_t fenceCount, const VkFence *pFences) {
    ICD_CALL_RES(vkbc_vkResetFences(c_, DEV_RAW(device), fenceCount, pFences));
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkGetFenceStatus(VkDevice device, VkFence fence) {
    ICD_CALL_RES(vkbc_vkGetFenceStatus(c_, DEV_RAW(device), fence));
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkWaitForFences(
    VkDevice device, uint32_t fenceCount, const VkFence *pFences, VkBool32 waitAll, uint64_t timeout) {
    ICD_CALL_RES(vkbc_vkWaitForFences(c_, DEV_RAW(device), fenceCount, pFences, waitAll, timeout));
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkCreateSemaphore(
    VkDevice device, const VkSemaphoreCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkSemaphore *pSemaphore) {
    if (!pCreateInfo) return VK_ERROR_INITIALIZATION_FAILED;
    VkSemaphoreCreateInfo ci = *pCreateInfo;
    ci.pNext = NULL;
    ICD_CALL_RES(vkbc_vkCreateSemaphore(c_, DEV_RAW(device), &ci, pAllocator, pSemaphore));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkDestroySemaphore(
    VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks *pAllocator) {
    if (!semaphore) return;
    ICD_CALL_VOID(vkbc_vkDestroySemaphore(c_, DEV_RAW(device), semaphore, pAllocator));
}

/* ---- Memory ---- */

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkAllocateMemory(
    VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo,
    const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory) {
    if (!pAllocateInfo) return VK_ERROR_INITIALIZATION_FAILED;
    VkMemoryAllocateInfo ai = *pAllocateInfo;
    ai.pNext = NULL;
    ICD_CALL_RES(vkbc_vkAllocateMemory(c_, DEV_RAW(device), &ai, pAllocator, pMemory));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkFreeMemory(
    VkDevice device, VkDeviceMemory memory, const VkAllocationCallbacks *pAllocator) {
    if (!memory) return;
    ICD_CALL_VOID(vkbc_vkFreeMemory(c_, DEV_RAW(device), memory, pAllocator));
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkMapMemory(
    VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
    VkDeviceSize size, VkMemoryMapFlags flags, void **ppData) {
    ICD_CALL_RES(vkbc_vkMapMemory(c_, DEV_RAW(device), memory, offset, size, flags, ppData));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkUnmapMemory(VkDevice device, VkDeviceMemory memory) {
    if (!memory) return;
    ICD_CALL_VOID(vkbc_vkUnmapMemory(c_, DEV_RAW(device), memory));
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkFlushMappedMemoryRanges(
    VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange *pMemoryRanges) {
    ICD_CALL_RES(vkbc_vkFlushMappedMemoryRanges(c_, DEV_RAW(device), memoryRangeCount, pMemoryRanges));
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkInvalidateMappedMemoryRanges(
    VkDevice device, uint32_t memoryRangeCount, const VkMappedMemoryRange *pMemoryRanges) {
    ICD_CALL_RES(vkbc_vkInvalidateMappedMemoryRanges(c_, DEV_RAW(device), memoryRangeCount, pMemoryRanges));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkGetDeviceMemoryCommitment(
    VkDevice device, VkDeviceMemory memory, VkDeviceSize *pCommittedMemoryInBytes) {
    ICD_CALL_VOID(vkbc_vkGetDeviceMemoryCommitment(c_, DEV_RAW(device), memory, pCommittedMemoryInBytes));
}

/* ---- Buffers & Images ---- */

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkCreateBuffer(
    VkDevice device, const VkBufferCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer) {
    if (!pCreateInfo) return VK_ERROR_INITIALIZATION_FAILED;
    VkBufferCreateInfo bci = *pCreateInfo;
    bci.pNext = NULL;
    ICD_CALL_RES(vkbc_vkCreateBuffer(c_, DEV_RAW(device), &bci, pAllocator, pBuffer));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkDestroyBuffer(
    VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *pAllocator) {
    if (!buffer) return;
    ICD_CALL_VOID(vkbc_vkDestroyBuffer(c_, DEV_RAW(device), buffer, pAllocator));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkGetBufferMemoryRequirements(
    VkDevice device, VkBuffer buffer, VkMemoryRequirements *pMemoryRequirements) {
    ICD_CALL_VOID(vkbc_vkGetBufferMemoryRequirements(c_, DEV_RAW(device), buffer, pMemoryRequirements));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkGetBufferMemoryRequirements2(
    VkDevice device, const VkBufferMemoryRequirementsInfo2 *pInfo, VkMemoryRequirements2 *pMemoryRequirements) {
    ICD_CALL_VOID(vkbc_vkGetBufferMemoryRequirements2(c_, DEV_RAW(device), pInfo, pMemoryRequirements));
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkBindBufferMemory(
    VkDevice device, VkBuffer buffer, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
    ICD_CALL_RES(vkbc_vkBindBufferMemory(c_, DEV_RAW(device), buffer, memory, memoryOffset));
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkBindBufferMemory2(
    VkDevice device, uint32_t bindInfoCount, const VkBindBufferMemoryInfo *pBindInfos) {
    ICD_CALL_RES(vkbc_vkBindBufferMemory2(c_, DEV_RAW(device), bindInfoCount, pBindInfos));
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkCreateImage(
    VkDevice device, const VkImageCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkImage *pImage) {
    if (!pCreateInfo) return VK_ERROR_INITIALIZATION_FAILED;
    VkImageCreateInfo ici = *pCreateInfo;
    ici.pNext = NULL;
    ICD_CALL_RES(vkbc_vkCreateImage(c_, DEV_RAW(device), &ici, pAllocator, pImage));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkDestroyImage(
    VkDevice device, VkImage image, const VkAllocationCallbacks *pAllocator) {
    if (!image) return;
    ICD_CALL_VOID(vkbc_vkDestroyImage(c_, DEV_RAW(device), image, pAllocator));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkGetImageMemoryRequirements(
    VkDevice device, VkImage image, VkMemoryRequirements *pMemoryRequirements) {
    ICD_CALL_VOID(vkbc_vkGetImageMemoryRequirements(c_, DEV_RAW(device), image, pMemoryRequirements));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkGetImageMemoryRequirements2(
    VkDevice device, const VkImageMemoryRequirementsInfo2 *pInfo, VkMemoryRequirements2 *pMemoryRequirements) {
    ICD_CALL_VOID(vkbc_vkGetImageMemoryRequirements2(c_, DEV_RAW(device), pInfo, pMemoryRequirements));
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkBindImageMemory(
    VkDevice device, VkImage image, VkDeviceMemory memory, VkDeviceSize memoryOffset) {
    ICD_CALL_RES(vkbc_vkBindImageMemory(c_, DEV_RAW(device), image, memory, memoryOffset));
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkBindImageMemory2(
    VkDevice device, uint32_t bindInfoCount, const VkBindImageMemoryInfo *pBindInfos) {
    ICD_CALL_RES(vkbc_vkBindImageMemory2(c_, DEV_RAW(device), bindInfoCount, pBindInfos));
}

/* ---- Shader, Descriptors, Pipelines ---- */

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkCreateShaderModule(
    VkDevice device, const VkShaderModuleCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkShaderModule *pShaderModule) {
    if (!pCreateInfo) return VK_ERROR_INITIALIZATION_FAILED;
    VkShaderModuleCreateInfo ci = *pCreateInfo;
    ci.pNext = NULL;
    ICD_CALL_RES(vkbc_vkCreateShaderModule(c_, DEV_RAW(device), &ci, pAllocator, pShaderModule));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkDestroyShaderModule(
    VkDevice device, VkShaderModule shaderModule, const VkAllocationCallbacks *pAllocator) {
    if (!shaderModule) return;
    ICD_CALL_VOID(vkbc_vkDestroyShaderModule(c_, DEV_RAW(device), shaderModule, pAllocator));
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkCreateDescriptorSetLayout(
    VkDevice device, const VkDescriptorSetLayoutCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDescriptorSetLayout *pSetLayout) {
    if (!pCreateInfo) return VK_ERROR_INITIALIZATION_FAILED;
    VkDescriptorSetLayoutCreateInfo ci = *pCreateInfo;
    ci.pNext = NULL;
    ICD_CALL_RES(vkbc_vkCreateDescriptorSetLayout(c_, DEV_RAW(device), &ci, pAllocator, pSetLayout));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkDestroyDescriptorSetLayout(
    VkDevice device, VkDescriptorSetLayout descriptorSetLayout, const VkAllocationCallbacks *pAllocator) {
    if (!descriptorSetLayout) return;
    ICD_CALL_VOID(vkbc_vkDestroyDescriptorSetLayout(c_, DEV_RAW(device), descriptorSetLayout, pAllocator));
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkCreateDescriptorPool(
    VkDevice device, const VkDescriptorPoolCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDescriptorPool *pDescriptorPool) {
    if (!pCreateInfo) return VK_ERROR_INITIALIZATION_FAILED;
    VkDescriptorPoolCreateInfo ci = *pCreateInfo;
    ci.pNext = NULL;
    ICD_CALL_RES(vkbc_vkCreateDescriptorPool(c_, DEV_RAW(device), &ci, pAllocator, pDescriptorPool));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkDestroyDescriptorPool(
    VkDevice device, VkDescriptorPool descriptorPool, const VkAllocationCallbacks *pAllocator) {
    if (!descriptorPool) return;
    ICD_CALL_VOID(vkbc_vkDestroyDescriptorPool(c_, DEV_RAW(device), descriptorPool, pAllocator));
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkResetDescriptorPool(
    VkDevice device, VkDescriptorPool descriptorPool, VkDescriptorPoolResetFlags flags) {
    ICD_CALL_RES(vkbc_vkResetDescriptorPool(c_, DEV_RAW(device), descriptorPool, flags));
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkAllocateDescriptorSets(
    VkDevice device, const VkDescriptorSetAllocateInfo *pAllocateInfo, VkDescriptorSet *pDescriptorSets) {
    if (!pAllocateInfo) return VK_ERROR_INITIALIZATION_FAILED;
    VkDescriptorSetAllocateInfo ai = *pAllocateInfo;
    ai.pNext = NULL;
    ICD_CALL_RES(vkbc_vkAllocateDescriptorSets(c_, DEV_RAW(device), &ai, pDescriptorSets));
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkFreeDescriptorSets(
    VkDevice device, VkDescriptorPool descriptorPool,
    uint32_t descriptorSetCount, const VkDescriptorSet *pDescriptorSets) {
    if (!descriptorSetCount) return VK_SUCCESS;
    ICD_CALL_RES(vkbc_vkFreeDescriptorSets(c_, DEV_RAW(device), descriptorPool, descriptorSetCount, pDescriptorSets));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkUpdateDescriptorSets(
    VkDevice device, uint32_t descriptorWriteCount, const VkWriteDescriptorSet *pDescriptorWrites,
    uint32_t descriptorCopyCount, const VkCopyDescriptorSet *pDescriptorCopies) {
    ICD_CALL_VOID(vkbc_vkUpdateDescriptorSets(c_, DEV_RAW(device), descriptorWriteCount, pDescriptorWrites, descriptorCopyCount, pDescriptorCopies));
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkCreatePipelineLayout(
    VkDevice device, const VkPipelineLayoutCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkPipelineLayout *pPipelineLayout) {
    if (!pCreateInfo) return VK_ERROR_INITIALIZATION_FAILED;
    VkPipelineLayoutCreateInfo ci = *pCreateInfo;
    ci.pNext = NULL;
    ICD_CALL_RES(vkbc_vkCreatePipelineLayout(c_, DEV_RAW(device), &ci, pAllocator, pPipelineLayout));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkDestroyPipelineLayout(
    VkDevice device, VkPipelineLayout pipelineLayout, const VkAllocationCallbacks *pAllocator) {
    if (!pipelineLayout) return;
    ICD_CALL_VOID(vkbc_vkDestroyPipelineLayout(c_, DEV_RAW(device), pipelineLayout, pAllocator));
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkCreatePipelineCache(
    VkDevice device, const VkPipelineCacheCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkPipelineCache *pPipelineCache) {
    if (!pCreateInfo) return VK_ERROR_INITIALIZATION_FAILED;
    VkPipelineCacheCreateInfo ci = *pCreateInfo;
    ci.pNext = NULL;
    ICD_CALL_RES(vkbc_vkCreatePipelineCache(c_, DEV_RAW(device), &ci, pAllocator, pPipelineCache));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkDestroyPipelineCache(
    VkDevice device, VkPipelineCache pipelineCache, const VkAllocationCallbacks *pAllocator) {
    if (!pipelineCache) return;
    ICD_CALL_VOID(vkbc_vkDestroyPipelineCache(c_, DEV_RAW(device), pipelineCache, pAllocator));
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkCreateComputePipelines(
    VkDevice device, VkPipelineCache pipelineCache, uint32_t createInfoCount,
    const VkComputePipelineCreateInfo *pCreateInfos,
    const VkAllocationCallbacks *pAllocator, VkPipeline *pPipelines) {
    ICD_CALL_RES(vkbc_vkCreateComputePipelines(c_, DEV_RAW(device), pipelineCache, createInfoCount, pCreateInfos, pAllocator, pPipelines));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkDestroyPipeline(
    VkDevice device, VkPipeline pipeline, const VkAllocationCallbacks *pAllocator) {
    if (!pipeline) return;
    ICD_CALL_VOID(vkbc_vkDestroyPipeline(c_, DEV_RAW(device), pipeline, pAllocator));
}

/* ---- Command Pools & Command Buffers ---- */

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkCreateCommandPool(
    VkDevice device, const VkCommandPoolCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkCommandPool *pCommandPool) {
    if (!pCreateInfo) return VK_ERROR_INITIALIZATION_FAILED;
    VkCommandPoolCreateInfo ci = *pCreateInfo;
    ci.pNext = NULL;
    ICD_CALL_RES(vkbc_vkCreateCommandPool(c_, DEV_RAW(device), &ci, pAllocator, pCommandPool));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkDestroyCommandPool(
    VkDevice device, VkCommandPool commandPool, const VkAllocationCallbacks *pAllocator) {
    if (!commandPool) return;
    icd_device *dev = (icd_device *)device;
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkDestroyCommandPool(c, DEV_RAW(device), commandPool, pAllocator);
    for (icd_cmdbuf *cb = dev->cmdbufs, *nx; cb; cb = nx) {
        nx = cb->next;
        if (cb->pool == commandPool) cmdbuf_unlink_free(dev, cb);
    }
    pthread_mutex_unlock(&g_mu);
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkResetCommandPool(
    VkDevice device, VkCommandPool commandPool, VkCommandPoolResetFlags flags) {
    ICD_CALL_RES(vkbc_vkResetCommandPool(c_, DEV_RAW(device), commandPool, flags));
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkAllocateCommandBuffers(
    VkDevice device, const VkCommandBufferAllocateInfo *pAllocateInfo, VkCommandBuffer *pCommandBuffers) {
    if (!device || !pAllocateInfo || !pCommandBuffers) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t n = pAllocateInfo->commandBufferCount;
    if (!n) return VK_SUCCESS;
    icd_device *dev = (icd_device *)device;
    VkCommandBufferAllocateInfo ai = *pAllocateInfo;
    ai.pNext = NULL;

    VkCommandBuffer *raw = calloc(n, sizeof(*raw));
    icd_cmdbuf **wraps = calloc(n, sizeof(*wraps));
    if (!raw || !wraps) { free(raw); free(wraps); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    for (uint32_t i = 0; i < n; i++) {
        wraps[i] = calloc(1, sizeof(**wraps));
        if (!wraps[i]) {
            for (uint32_t j = 0; j < i; j++) free(wraps[j]);
            free(wraps); free(raw);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }

    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    VkResult r = c ? vkbc_vkAllocateCommandBuffers(c, DEV_RAW(device), &ai, raw) : VK_ERROR_DEVICE_LOST;
    if (r == VK_SUCCESS) {
        for (uint32_t i = 0; i < n; i++) {
            set_loader_magic_value(wraps[i]);
            wraps[i]->id = (uint64_t)(uintptr_t)raw[i];
            wraps[i]->pool = ai.commandPool;
            wraps[i]->next = dev->cmdbufs;
            if (dev->cmdbufs) dev->cmdbufs->prev = wraps[i];
            dev->cmdbufs = wraps[i];
            pCommandBuffers[i] = (VkCommandBuffer)wraps[i];
        }
    }
    pthread_mutex_unlock(&g_mu);
    if (r != VK_SUCCESS) {
        for (uint32_t i = 0; i < n; i++) { free(wraps[i]); pCommandBuffers[i] = VK_NULL_HANDLE; }
    }
    free(wraps);
    free(raw);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL icd_vkFreeCommandBuffers(
    VkDevice device, VkCommandPool commandPool,
    uint32_t commandBufferCount, const VkCommandBuffer *pCommandBuffers) {
    if (!device || !commandBufferCount || !pCommandBuffers) return;
    icd_device *dev = (icd_device *)device;
    VkCommandBuffer *raw = calloc(commandBufferCount, sizeof(*raw));
    if (!raw) return;
    for (uint32_t i = 0; i < commandBufferCount; i++) raw[i] = CMDBUF_RAW(pCommandBuffers[i]);
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkFreeCommandBuffers(c, DEV_RAW(device), commandPool, commandBufferCount, raw);
    for (uint32_t i = 0; i < commandBufferCount; i++) {
        if (pCommandBuffers[i]) cmdbuf_unlink_free(dev, (icd_cmdbuf *)pCommandBuffers[i]);
    }
    pthread_mutex_unlock(&g_mu);
    free(raw);
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkBeginCommandBuffer(
    VkCommandBuffer commandBuffer, const VkCommandBufferBeginInfo *pBeginInfo) {
    if (!pBeginInfo) return VK_ERROR_INITIALIZATION_FAILED;
    VkCommandBufferBeginInfo bi = *pBeginInfo;
    bi.pNext = NULL;
    ICD_CALL_RES(vkbc_vkBeginCommandBuffer(c_, CMDBUF_RAW(commandBuffer), &bi));
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkEndCommandBuffer(VkCommandBuffer commandBuffer) {
    ICD_CALL_RES(vkbc_vkEndCommandBuffer(c_, CMDBUF_RAW(commandBuffer)));
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkResetCommandBuffer(
    VkCommandBuffer commandBuffer, VkCommandBufferResetFlags flags) {
    ICD_CALL_RES(vkbc_vkResetCommandBuffer(c_, CMDBUF_RAW(commandBuffer), flags));
}

/* ---- Recorded vkCmd* compute & transfer commands ---- */

static VKAPI_ATTR void VKAPI_CALL icd_vkCmdBindPipeline(
    VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipeline pipeline) {
    ICD_CALL_VOID(vkbc_vkCmdBindPipeline(c_, CMDBUF_RAW(commandBuffer), pipelineBindPoint, pipeline));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkCmdBindDescriptorSets(
    VkCommandBuffer commandBuffer, VkPipelineBindPoint pipelineBindPoint, VkPipelineLayout layout,
    uint32_t firstSet, uint32_t descriptorSetCount, const VkDescriptorSet *pDescriptorSets,
    uint32_t dynamicOffsetCount, const uint32_t *pDynamicOffsets) {
    ICD_CALL_VOID(vkbc_vkCmdBindDescriptorSets(c_, CMDBUF_RAW(commandBuffer), pipelineBindPoint, layout,
                                               firstSet, descriptorSetCount, pDescriptorSets,
                                               dynamicOffsetCount, pDynamicOffsets));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkCmdPushConstants(
    VkCommandBuffer commandBuffer, VkPipelineLayout layout, VkShaderStageFlags stageFlags,
    uint32_t offset, uint32_t size, const void *pValues) {
    ICD_CALL_VOID(vkbc_vkCmdPushConstants(c_, CMDBUF_RAW(commandBuffer), layout, stageFlags, offset, size, pValues));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkCmdDispatch(
    VkCommandBuffer commandBuffer, uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
    ICD_CALL_VOID(vkbc_vkCmdDispatch(c_, CMDBUF_RAW(commandBuffer), groupCountX, groupCountY, groupCountZ));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkCmdDispatchBase(
    VkCommandBuffer commandBuffer, uint32_t baseGroupX, uint32_t baseGroupY, uint32_t baseGroupZ,
    uint32_t groupCountX, uint32_t groupCountY, uint32_t groupCountZ) {
    ICD_CALL_VOID(vkbc_vkCmdDispatchBase(c_, CMDBUF_RAW(commandBuffer), baseGroupX, baseGroupY, baseGroupZ,
                                         groupCountX, groupCountY, groupCountZ));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkCmdDispatchIndirect(
    VkCommandBuffer commandBuffer, VkBuffer buffer, VkDeviceSize offset) {
    ICD_CALL_VOID(vkbc_vkCmdDispatchIndirect(c_, CMDBUF_RAW(commandBuffer), buffer, offset));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkCmdPipelineBarrier(
    VkCommandBuffer commandBuffer, VkPipelineStageFlags srcStageMask, VkPipelineStageFlags dstStageMask,
    VkDependencyFlags dependencyFlags, uint32_t memoryBarrierCount, const VkMemoryBarrier *pMemoryBarriers,
    uint32_t bufferMemoryBarrierCount, const VkBufferMemoryBarrier *pBufferMemoryBarriers,
    uint32_t imageMemoryBarrierCount, const VkImageMemoryBarrier *pImageMemoryBarriers) {
    ICD_CALL_VOID(vkbc_vkCmdPipelineBarrier(c_, CMDBUF_RAW(commandBuffer), srcStageMask, dstStageMask,
                                            dependencyFlags, memoryBarrierCount, pMemoryBarriers,
                                            bufferMemoryBarrierCount, pBufferMemoryBarriers,
                                            imageMemoryBarrierCount, pImageMemoryBarriers));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkCmdCopyBuffer(
    VkCommandBuffer commandBuffer, VkBuffer srcBuffer, VkBuffer dstBuffer,
    uint32_t regionCount, const VkBufferCopy *pRegions) {
    ICD_CALL_VOID(vkbc_vkCmdCopyBuffer(c_, CMDBUF_RAW(commandBuffer), srcBuffer, dstBuffer, regionCount, pRegions));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkCmdFillBuffer(
    VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize size, uint32_t data) {
    ICD_CALL_VOID(vkbc_vkCmdFillBuffer(c_, CMDBUF_RAW(commandBuffer), dstBuffer, dstOffset, size, data));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkCmdUpdateBuffer(
    VkCommandBuffer commandBuffer, VkBuffer dstBuffer, VkDeviceSize dstOffset, VkDeviceSize dataSize, const void *pData) {
    ICD_CALL_VOID(vkbc_vkCmdUpdateBuffer(c_, CMDBUF_RAW(commandBuffer), dstBuffer, dstOffset, dataSize, pData));
}

/* ---- ProcAddr tables & Loader-ICD v5 exports ---- */

typedef struct { const char *name; PFN_vkVoidFunction fn; } proc_ent;

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL icd_vkGetDeviceProcAddr(VkDevice device, const char *pName);

#define ENT(nm, fn) { nm, (PFN_vkVoidFunction)(fn) }

static const proc_ent k_global_procs[] = {
    ENT("vkGetInstanceProcAddr", vk_icdGetInstanceProcAddr),
    ENT("vkEnumerateInstanceVersion", icd_vkEnumerateInstanceVersion),
    ENT("vkEnumerateInstanceExtensionProperties", icd_vkEnumerateInstanceExtensionProperties),
    ENT("vkEnumerateInstanceLayerProperties", icd_vkEnumerateInstanceLayerProperties),
    ENT("vkCreateInstance", icd_vkCreateInstance),
};

static const proc_ent k_inst_procs[] = {
    ENT("vkDestroyInstance", icd_vkDestroyInstance),
    ENT("vkEnumeratePhysicalDevices", icd_vkEnumeratePhysicalDevices),
    ENT("vkCreateDevice", icd_vkCreateDevice),
    ENT("vkGetDeviceProcAddr", icd_vkGetDeviceProcAddr),
};

static const proc_ent k_pdev_procs[] = {
    ENT("vkGetPhysicalDeviceProperties", icd_vkGetPhysicalDeviceProperties),
    ENT("vkGetPhysicalDeviceFeatures", icd_vkGetPhysicalDeviceFeatures),
    ENT("vkGetPhysicalDeviceMemoryProperties", icd_vkGetPhysicalDeviceMemoryProperties),
    ENT("vkGetPhysicalDeviceQueueFamilyProperties", icd_vkGetPhysicalDeviceQueueFamilyProperties),
    ENT("vkGetPhysicalDeviceFormatProperties", icd_vkGetPhysicalDeviceFormatProperties),
    ENT("vkGetPhysicalDeviceImageFormatProperties", icd_vkGetPhysicalDeviceImageFormatProperties),
    ENT("vkGetPhysicalDeviceSparseImageFormatProperties", icd_vkGetPhysicalDeviceSparseImageFormatProperties),
    ENT("vkEnumerateDeviceExtensionProperties", icd_vkEnumerateDeviceExtensionProperties),
    ENT("vkEnumerateDeviceLayerProperties", icd_vkEnumerateDeviceLayerProperties),
    ENT("vkGetPhysicalDeviceProperties2", icd_vkGetPhysicalDeviceProperties2),
    ENT("vkGetPhysicalDeviceProperties2KHR", icd_vkGetPhysicalDeviceProperties2),
    ENT("vkGetPhysicalDeviceFeatures2", icd_vkGetPhysicalDeviceFeatures2),
    ENT("vkGetPhysicalDeviceFeatures2KHR", icd_vkGetPhysicalDeviceFeatures2),
    ENT("vkGetPhysicalDeviceMemoryProperties2", icd_vkGetPhysicalDeviceMemoryProperties2),
    ENT("vkGetPhysicalDeviceMemoryProperties2KHR", icd_vkGetPhysicalDeviceMemoryProperties2),
    ENT("vkGetPhysicalDeviceFormatProperties2", icd_vkGetPhysicalDeviceFormatProperties2),
    ENT("vkGetPhysicalDeviceFormatProperties2KHR", icd_vkGetPhysicalDeviceFormatProperties2),
    ENT("vkGetPhysicalDeviceImageFormatProperties2", icd_vkGetPhysicalDeviceImageFormatProperties2),
    ENT("vkGetPhysicalDeviceImageFormatProperties2KHR", icd_vkGetPhysicalDeviceImageFormatProperties2),
    ENT("vkGetPhysicalDeviceQueueFamilyProperties2", icd_vkGetPhysicalDeviceQueueFamilyProperties2),
    ENT("vkGetPhysicalDeviceQueueFamilyProperties2KHR", icd_vkGetPhysicalDeviceQueueFamilyProperties2),
    ENT("vkGetPhysicalDeviceSparseImageFormatProperties2", icd_vkGetPhysicalDeviceSparseImageFormatProperties2),
    ENT("vkGetPhysicalDeviceSparseImageFormatProperties2KHR", icd_vkGetPhysicalDeviceSparseImageFormatProperties2),
    ENT("vkGetPhysicalDeviceExternalBufferProperties", icd_vkGetPhysicalDeviceExternalBufferProperties),
    ENT("vkGetPhysicalDeviceExternalBufferPropertiesKHR", icd_vkGetPhysicalDeviceExternalBufferProperties),
    ENT("vkGetPhysicalDeviceExternalFenceProperties", icd_vkGetPhysicalDeviceExternalFenceProperties),
    ENT("vkGetPhysicalDeviceExternalFencePropertiesKHR", icd_vkGetPhysicalDeviceExternalFenceProperties),
    ENT("vkGetPhysicalDeviceExternalSemaphoreProperties", icd_vkGetPhysicalDeviceExternalSemaphoreProperties),
    ENT("vkGetPhysicalDeviceExternalSemaphorePropertiesKHR", icd_vkGetPhysicalDeviceExternalSemaphoreProperties),
    ENT("vkGetPhysicalDeviceToolProperties", icd_vkGetPhysicalDeviceToolProperties),
    ENT("vkGetPhysicalDeviceToolPropertiesEXT", icd_vkGetPhysicalDeviceToolProperties),
};

static const proc_ent k_dev_procs[] = {
    ENT("vkGetDeviceProcAddr", icd_vkGetDeviceProcAddr),
    ENT("vkDestroyDevice", icd_vkDestroyDevice),
    ENT("vkDeviceWaitIdle", icd_vkDeviceWaitIdle),
    ENT("vkGetDeviceQueue", icd_vkGetDeviceQueue),
    ENT("vkQueueSubmit", icd_vkQueueSubmit),
    ENT("vkQueueWaitIdle", icd_vkQueueWaitIdle),
    ENT("vkCreateFence", icd_vkCreateFence),
    ENT("vkDestroyFence", icd_vkDestroyFence),
    ENT("vkResetFences", icd_vkResetFences),
    ENT("vkGetFenceStatus", icd_vkGetFenceStatus),
    ENT("vkWaitForFences", icd_vkWaitForFences),
    ENT("vkCreateSemaphore", icd_vkCreateSemaphore),
    ENT("vkDestroySemaphore", icd_vkDestroySemaphore),
    ENT("vkAllocateMemory", icd_vkAllocateMemory),
    ENT("vkFreeMemory", icd_vkFreeMemory),
    ENT("vkMapMemory", icd_vkMapMemory),
    ENT("vkUnmapMemory", icd_vkUnmapMemory),
    ENT("vkFlushMappedMemoryRanges", icd_vkFlushMappedMemoryRanges),
    ENT("vkInvalidateMappedMemoryRanges", icd_vkInvalidateMappedMemoryRanges),
    ENT("vkGetDeviceMemoryCommitment", icd_vkGetDeviceMemoryCommitment),
    ENT("vkCreateBuffer", icd_vkCreateBuffer),
    ENT("vkDestroyBuffer", icd_vkDestroyBuffer),
    ENT("vkGetBufferMemoryRequirements", icd_vkGetBufferMemoryRequirements),
    ENT("vkGetBufferMemoryRequirements2", icd_vkGetBufferMemoryRequirements2),
    ENT("vkGetBufferMemoryRequirements2KHR", icd_vkGetBufferMemoryRequirements2),
    ENT("vkBindBufferMemory", icd_vkBindBufferMemory),
    ENT("vkBindBufferMemory2", icd_vkBindBufferMemory2),
    ENT("vkBindBufferMemory2KHR", icd_vkBindBufferMemory2),
    ENT("vkCreateImage", icd_vkCreateImage),
    ENT("vkDestroyImage", icd_vkDestroyImage),
    ENT("vkGetImageMemoryRequirements", icd_vkGetImageMemoryRequirements),
    ENT("vkGetImageMemoryRequirements2", icd_vkGetImageMemoryRequirements2),
    ENT("vkGetImageMemoryRequirements2KHR", icd_vkGetImageMemoryRequirements2),
    ENT("vkBindImageMemory", icd_vkBindImageMemory),
    ENT("vkBindImageMemory2", icd_vkBindImageMemory2),
    ENT("vkBindImageMemory2KHR", icd_vkBindImageMemory2),
    ENT("vkCreateShaderModule", icd_vkCreateShaderModule),
    ENT("vkDestroyShaderModule", icd_vkDestroyShaderModule),
    ENT("vkCreateDescriptorSetLayout", icd_vkCreateDescriptorSetLayout),
    ENT("vkDestroyDescriptorSetLayout", icd_vkDestroyDescriptorSetLayout),
    ENT("vkCreateDescriptorPool", icd_vkCreateDescriptorPool),
    ENT("vkDestroyDescriptorPool", icd_vkDestroyDescriptorPool),
    ENT("vkResetDescriptorPool", icd_vkResetDescriptorPool),
    ENT("vkAllocateDescriptorSets", icd_vkAllocateDescriptorSets),
    ENT("vkFreeDescriptorSets", icd_vkFreeDescriptorSets),
    ENT("vkUpdateDescriptorSets", icd_vkUpdateDescriptorSets),
    ENT("vkCreatePipelineLayout", icd_vkCreatePipelineLayout),
    ENT("vkDestroyPipelineLayout", icd_vkDestroyPipelineLayout),
    ENT("vkCreatePipelineCache", icd_vkCreatePipelineCache),
    ENT("vkDestroyPipelineCache", icd_vkDestroyPipelineCache),
    ENT("vkCreateComputePipelines", icd_vkCreateComputePipelines),
    ENT("vkDestroyPipeline", icd_vkDestroyPipeline),
    ENT("vkCreateCommandPool", icd_vkCreateCommandPool),
    ENT("vkDestroyCommandPool", icd_vkDestroyCommandPool),
    ENT("vkResetCommandPool", icd_vkResetCommandPool),
    ENT("vkAllocateCommandBuffers", icd_vkAllocateCommandBuffers),
    ENT("vkFreeCommandBuffers", icd_vkFreeCommandBuffers),
    ENT("vkBeginCommandBuffer", icd_vkBeginCommandBuffer),
    ENT("vkEndCommandBuffer", icd_vkEndCommandBuffer),
    ENT("vkResetCommandBuffer", icd_vkResetCommandBuffer),
    ENT("vkCmdBindPipeline", icd_vkCmdBindPipeline),
    ENT("vkCmdBindDescriptorSets", icd_vkCmdBindDescriptorSets),
    ENT("vkCmdPushConstants", icd_vkCmdPushConstants),
    ENT("vkCmdDispatch", icd_vkCmdDispatch),
    ENT("vkCmdDispatchBase", icd_vkCmdDispatchBase),
    ENT("vkCmdDispatchBaseKHR", icd_vkCmdDispatchBase),
    ENT("vkCmdDispatchIndirect", icd_vkCmdDispatchIndirect),
    ENT("vkCmdPipelineBarrier", icd_vkCmdPipelineBarrier),
    ENT("vkCmdCopyBuffer", icd_vkCmdCopyBuffer),
    ENT("vkCmdFillBuffer", icd_vkCmdFillBuffer),
    ENT("vkCmdUpdateBuffer", icd_vkCmdUpdateBuffer),
};

static PFN_vkVoidFunction lookup_proc(const proc_ent *tab, size_t n, const char *name) {
    for (size_t i = 0; i < n; i++)
        if (strcmp(tab[i].name, name) == 0) return tab[i].fn;
    return NULL;
}

static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL icd_vkGetDeviceProcAddr(VkDevice device, const char *pName) {
    if (!device || !pName) return NULL;
    PFN_vkVoidFunction fn = lookup_proc(k_dev_procs, sizeof(k_dev_procs) / sizeof(k_dev_procs[0]), pName);
    if (!fn) log_missing("GDPA", pName);
    return fn;
}

VKAPI_ATTR VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pVersion) {
    if (!pVersion) return VK_ERROR_INITIALIZATION_FAILED;
    if (*pVersion > ICD_IFACE_VERSION) *pVersion = ICD_IFACE_VERSION;
    return VK_SUCCESS;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance instance, const char *pName) {
    if (!pName) return NULL;
    PFN_vkVoidFunction fn = lookup_proc(k_global_procs, sizeof(k_global_procs) / sizeof(k_global_procs[0]), pName);
    if (fn) return fn;
    if (!instance) {
        log_missing("GIPA(NULL)", pName);
        return NULL;
    }
    fn = lookup_proc(k_inst_procs, sizeof(k_inst_procs) / sizeof(k_inst_procs[0]), pName);
    if (fn) return fn;
    fn = lookup_proc(k_pdev_procs, sizeof(k_pdev_procs) / sizeof(k_pdev_procs[0]), pName);
    if (fn) return fn;
    fn = lookup_proc(k_dev_procs, sizeof(k_dev_procs) / sizeof(k_dev_procs[0]), pName);
    if (fn) return fn;
    log_missing("GIPA", pName);
    return NULL;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vk_icdGetPhysicalDeviceProcAddr(VkInstance instance, const char *pName) {
    if (!instance || !pName) return NULL;
    PFN_vkVoidFunction fn = lookup_proc(k_pdev_procs, sizeof(k_pdev_procs) / sizeof(k_pdev_procs[0]), pName);
    if (!fn) log_missing("GPDPA", pName);
    return fn;
}
