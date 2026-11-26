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

struct icd_device {
    VK_LOADER_DATA loader_data;
    uint64_t id;
    icd_pdev *pdev;
    icd_queue *queues;
};

_Static_assert(offsetof(icd_instance, loader_data) == 0, "VK_LOADER_DATA must be at offset 0");
_Static_assert(offsetof(icd_pdev, loader_data) == 0, "VK_LOADER_DATA must be at offset 0");
_Static_assert(offsetof(icd_device, loader_data) == 0, "VK_LOADER_DATA must be at offset 0");
_Static_assert(offsetof(icd_queue, loader_data) == 0, "VK_LOADER_DATA must be at offset 0");

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
    pthread_mutex_unlock(&g_mu);
    icd_queue *q = dev->queues;
    while (q) {
        icd_queue *nx = q->next;
        free(q);
        q = nx;
    }
    free(dev);
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

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkCreateBuffer(
    VkDevice device, const VkBufferCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkBuffer *pBuffer) {
    if (!pCreateInfo) return VK_ERROR_INITIALIZATION_FAILED;
    VkBufferCreateInfo bci = *pCreateInfo;
    bci.pNext = NULL;
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    VkResult r = c ? vkbc_vkCreateBuffer(c, DEV_RAW(device), &bci, pAllocator, pBuffer) : VK_ERROR_DEVICE_LOST;
    pthread_mutex_unlock(&g_mu);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL icd_vkDestroyBuffer(
    VkDevice device, VkBuffer buffer, const VkAllocationCallbacks *pAllocator) {
    if (!buffer) return;
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkDestroyBuffer(c, DEV_RAW(device), buffer, pAllocator);
    pthread_mutex_unlock(&g_mu);
}

static VKAPI_ATTR void VKAPI_CALL icd_vkGetBufferMemoryRequirements(
    VkDevice device, VkBuffer buffer, VkMemoryRequirements *pMemoryRequirements) {
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkGetBufferMemoryRequirements(c, DEV_RAW(device), buffer, pMemoryRequirements);
    pthread_mutex_unlock(&g_mu);
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkCreateImage(
    VkDevice device, const VkImageCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkImage *pImage) {
    if (!pCreateInfo) return VK_ERROR_INITIALIZATION_FAILED;
    VkImageCreateInfo ici = *pCreateInfo;
    ici.pNext = NULL;
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    VkResult r = c ? vkbc_vkCreateImage(c, DEV_RAW(device), &ici, pAllocator, pImage) : VK_ERROR_DEVICE_LOST;
    pthread_mutex_unlock(&g_mu);
    return r;
}

static VKAPI_ATTR void VKAPI_CALL icd_vkDestroyImage(
    VkDevice device, VkImage image, const VkAllocationCallbacks *pAllocator) {
    if (!image) return;
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkDestroyImage(c, DEV_RAW(device), image, pAllocator);
    pthread_mutex_unlock(&g_mu);
}

static VKAPI_ATTR void VKAPI_CALL icd_vkGetImageMemoryRequirements(
    VkDevice device, VkImage image, VkMemoryRequirements *pMemoryRequirements) {
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkGetImageMemoryRequirements(c, DEV_RAW(device), image, pMemoryRequirements);
    pthread_mutex_unlock(&g_mu);
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
    ENT("vkGetDeviceQueue", icd_vkGetDeviceQueue),
    ENT("vkCreateBuffer", icd_vkCreateBuffer),
    ENT("vkDestroyBuffer", icd_vkDestroyBuffer),
    ENT("vkGetBufferMemoryRequirements", icd_vkGetBufferMemoryRequirements),
    ENT("vkCreateImage", icd_vkCreateImage),
    ENT("vkDestroyImage", icd_vkDestroyImage),
    ENT("vkGetImageMemoryRequirements", icd_vkGetImageMemoryRequirements),
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
