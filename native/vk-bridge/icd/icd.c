/* LodOS guest glibc Vulkan ICD v0/v1 (Loader-ICD interface v5).
 * Forwards Vulkan calls over vk-bridge (memfd SPSC rings + AF_UNIX SCM_RIGHTS) to host vkb_server.
 * Dispatchable handles wrap u64 bridge IDs in heap structs with VK_LOADER_DATA at offset 0 (R12). */
#define _GNU_SOURCE
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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
    VkDeviceQueueCreateFlags flags;
    uint32_t qfi, qi;
    icd_queue *next;
};

struct icd_cmdbuf {
    VK_LOADER_DATA loader_data;
    uint64_t id;
    VkCommandPool pool;
    VkCommandBufferLevel level;
    icd_cmdbuf *prev, *next;
};

struct icd_device {
    VK_LOADER_DATA loader_data;
    uint64_t id;
    icd_pdev *pdev;
    icd_queue *queues;
    icd_cmdbuf *cmdbufs;
};

typedef struct {
    VkDescriptorUpdateTemplateType type;
    VkPipelineBindPoint bind_point;
    VkPipelineLayout layout;
    uint32_t set;
    uint32_t entry_count;
    VkDescriptorUpdateTemplateEntry entries[];
} icd_desc_template;

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
    static pthread_mutex_t log_mu = PTHREAD_MUTEX_INITIALIZER;
    static int checked, gdpa_on, all_on;
    static char seen[1024][80];
    static size_t nseen;
    if (!name) return;
    pthread_mutex_lock(&log_mu);
    if (!checked) {
        const char *e = getenv("LODOS_VK_LOG_MISSING");
        gdpa_on = (!e || e[0] != '0'); /* GDPA misses logged by default unless LODOS_VK_LOG_MISSING=0 */
        all_on = (e && e[0] == '1');
        checked = 1;
    }
    int want = (strcmp(where, "GDPA") == 0) ? gdpa_on : all_on;
    if (want) {
        int dup = 0;
        for (size_t i = 0; i < nseen; i++) {
            if (strcmp(seen[i], name) == 0) { dup = 1; break; }
        }
        if (!dup) {
            if (nseen < sizeof(seen) / sizeof(seen[0])) {
                snprintf(seen[nseen++], sizeof(seen[0]), "%s", name);
            }
            fprintf(stderr, "[lodos-icd] missing %s: %s\n", where, name);
        }
    }
    pthread_mutex_unlock(&log_mu);
}

static int icd_dev_ext_supported(const char *name);

/* Instance extensions whose entrypoints this ICD supports (WSI comes in S2). */
static int icd_inst_ext_supported(const char *name) {
    static const char *const k_exts[] = {
        VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        VK_KHR_DEVICE_GROUP_CREATION_EXTENSION_NAME,
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

#define PD_RAW(pd) ((VkPhysicalDevice)(uintptr_t)((icd_pdev *)(pd))->id)
#define DEV_RAW(d) ((VkDevice)(uintptr_t)((icd_device *)(d))->id)
#define QUEUE_RAW(q) ((VkQueue)(uintptr_t)((icd_queue *)(q))->id)
#define CMDBUF_RAW(cb) ((cb) ? (VkCommandBuffer)(uintptr_t)((icd_cmdbuf *)(cb))->id : VK_NULL_HANDLE)

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
        if (app.apiVersion > VK_API_VERSION_1_3)
            app.apiVersion = VK_API_VERSION_1_3;
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

static VkResult icd_ensure_pdevs_locked(icd_instance *inst) {
    if (inst->pdevs_queried) return VK_SUCCESS;
    vkbc *c = icd_conn_locked();
    if (!c) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t n = 0;
    VkResult r = vkbc_vkEnumeratePhysicalDevices(c, (VkInstance)(uintptr_t)inst->id, &n, NULL);
    if (r < 0) return r;
    if (n > 0) {
        VkPhysicalDevice *raw = calloc(n, sizeof(*raw));
        icd_pdev *pds = calloc(n, sizeof(*pds));
        if (!raw || !pds) {
            free(raw); free(pds);
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
        r = vkbc_vkEnumeratePhysicalDevices(c, (VkInstance)(uintptr_t)inst->id, &n, raw);
        if (r < 0) {
            free(raw); free(pds);
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
    return VK_SUCCESS;
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkEnumeratePhysicalDevices(
    VkInstance instance, uint32_t *pPhysicalDeviceCount, VkPhysicalDevice *pPhysicalDevices) {
    if (!instance || !pPhysicalDeviceCount) return VK_ERROR_INITIALIZATION_FAILED;
    icd_instance *inst = (icd_instance *)instance;

    pthread_mutex_lock(&g_mu);
    VkResult r = icd_ensure_pdevs_locked(inst);
    pthread_mutex_unlock(&g_mu);
    if (r < 0) return r;

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

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkEnumeratePhysicalDeviceGroups(
    VkInstance instance, uint32_t *pPhysicalDeviceGroupCount,
    VkPhysicalDeviceGroupProperties *pPhysicalDeviceGroupProperties) {
    if (!instance || !pPhysicalDeviceGroupCount) return VK_ERROR_INITIALIZATION_FAILED;
    icd_instance *inst = (icd_instance *)instance;

    pthread_mutex_lock(&g_mu);
    VkResult r = icd_ensure_pdevs_locked(inst);
    if (r >= 0) {
        vkbc *c = icd_conn_locked();
        r = c ? vkbc_vkEnumeratePhysicalDeviceGroups(c, (VkInstance)(uintptr_t)inst->id,
                                                     pPhysicalDeviceGroupCount, pPhysicalDeviceGroupProperties)
              : VK_ERROR_INITIALIZATION_FAILED;
    }
    pthread_mutex_unlock(&g_mu);
    if (r >= 0 && pPhysicalDeviceGroupProperties) {
        for (uint32_t i = 0; i < *pPhysicalDeviceGroupCount; i++) {
            VkPhysicalDeviceGroupProperties *gp = &pPhysicalDeviceGroupProperties[i];
            for (uint32_t j = 0; j < gp->physicalDeviceCount && j < VK_MAX_DEVICE_GROUP_SIZE; j++) {
                uint64_t raw_id = (uint64_t)(uintptr_t)gp->physicalDevices[j];
                VkPhysicalDevice wrapped = VK_NULL_HANDLE;
                for (uint32_t k = 0; k < inst->npdevs; k++) {
                    if (inst->pdevs[k].id == raw_id) {
                        wrapped = (VkPhysicalDevice)&inst->pdevs[k];
                        break;
                    }
                }
                gp->physicalDevices[j] = wrapped;
            }
        }
    }
    return r;
}

/* ---- PhysicalDevice custom commands ---- */

static VKAPI_ATTR void VKAPI_CALL icd_vkGetPhysicalDeviceProperties(
    VkPhysicalDevice pd, VkPhysicalDeviceProperties *pProperties) {
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkGetPhysicalDeviceProperties(c, PD_RAW(pd), pProperties);
    pthread_mutex_unlock(&g_mu);
    if (pProperties && pProperties->apiVersion > VK_API_VERSION_1_3)
        pProperties->apiVersion = VK_MAKE_API_VERSION(0, 1, 3, VK_API_VERSION_PATCH(pProperties->apiVersion));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkGetPhysicalDeviceProperties2(
    VkPhysicalDevice pd, VkPhysicalDeviceProperties2 *pProperties) {
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkGetPhysicalDeviceProperties2(c, PD_RAW(pd), pProperties);
    pthread_mutex_unlock(&g_mu);
    if (pProperties && pProperties->properties.apiVersion > VK_API_VERSION_1_3)
        pProperties->properties.apiVersion =
            VK_MAKE_API_VERSION(0, 1, 3, VK_API_VERSION_PATCH(pProperties->properties.apiVersion));
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
    if (!pPropertyCount) return VK_ERROR_INITIALIZATION_FAILED;

    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (!c) { pthread_mutex_unlock(&g_mu); return VK_ERROR_DEVICE_LOST; }
    uint32_t raw_n = 0;
    VkResult r = vkbc_vkEnumerateDeviceExtensionProperties(c, PD_RAW(pd), NULL, &raw_n, NULL);
    VkExtensionProperties *raw = NULL;
    if (r == VK_SUCCESS && raw_n > 0) {
        raw = calloc(raw_n, sizeof(*raw));
        if (!raw) { pthread_mutex_unlock(&g_mu); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        r = vkbc_vkEnumerateDeviceExtensionProperties(c, PD_RAW(pd), NULL, &raw_n, raw);
    }
    pthread_mutex_unlock(&g_mu);
    if (r < 0) { free(raw); return r; }

    VkExtensionProperties *filt = raw_n ? calloc(raw_n, sizeof(*filt)) : NULL;
    if (raw_n && !filt) { free(raw); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    uint32_t fn = 0;
    for (uint32_t i = 0; i < raw_n; i++) {
        if (icd_dev_ext_supported(raw[i].extensionName))
            filt[fn++] = raw[i];
    }
    free(raw);

    if (!pProperties) {
        *pPropertyCount = fn;
        free(filt);
        return VK_SUCCESS;
    }
    uint32_t copy_n = *pPropertyCount < fn ? *pPropertyCount : fn;
    if (copy_n) memcpy(pProperties, filt, copy_n * sizeof(filt[0]));
    *pPropertyCount = copy_n;
    free(filt);
    return copy_n < fn ? VK_INCOMPLETE : VK_SUCCESS;
}

/* ---- Custom Device, Queue, Memory, CommandBuffer & DescriptorTemplate commands ---- */

static void cmdbuf_unlink_free(icd_device *dev, icd_cmdbuf *cb) {
    if (cb->prev) cb->prev->next = cb->next;
    else dev->cmdbufs = cb->next;
    if (cb->next) cb->next->prev = cb->prev;
    free(cb);
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkCreateDevice(
    VkPhysicalDevice pd, const VkDeviceCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDevice *pDevice) {
    if (!pd || !pCreateInfo || !pDevice) return VK_ERROR_INITIALIZATION_FAILED;

    for (uint32_t i = 0; i < pCreateInfo->enabledExtensionCount; i++) {
        const char *en = pCreateInfo->ppEnabledExtensionNames[i];
        if (!icd_dev_ext_supported(en))
            return VK_ERROR_EXTENSION_NOT_PRESENT;
    }

    VkDeviceCreateInfo dci = *pCreateInfo;
    /* vkr_in_pnext_enc strips guest loader VkLayerDeviceCreateInfo while preserving feature chains */
    dci.enabledLayerCount = 0;
    dci.ppEnabledLayerNames = NULL;

    /* If VkDeviceGroupDeviceCreateInfo is in pNext, unwrap its VkPhysicalDevice array */
    VkBaseOutStructure *cloned_head = NULL, *cloned_tail = NULL;
    VkPhysicalDevice *dg_pds = NULL;
    int has_dg = 0;
    for (const VkBaseInStructure *n = (const VkBaseInStructure *)dci.pNext; n; n = n->pNext) {
        if (n->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO) { has_dg = 1; break; }
    }
    if (has_dg) {
        for (const VkBaseInStructure *n = (const VkBaseInStructure *)dci.pNext; n; n = n->pNext) {
            if (n->sType == VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO) {
                const VkDeviceGroupDeviceCreateInfo *src = (const VkDeviceGroupDeviceCreateInfo *)n;
                VkDeviceGroupDeviceCreateInfo * copy = calloc(1, sizeof(*copy));
                if (!copy) { free(dg_pds); return VK_ERROR_OUT_OF_HOST_MEMORY; }
                *copy = *src;
                copy->pNext = NULL;
                if (src->physicalDeviceCount > 0 && src->pPhysicalDevices) {
                    dg_pds = calloc(src->physicalDeviceCount, sizeof(*dg_pds));
                    if (!dg_pds) { free(copy); return VK_ERROR_OUT_OF_HOST_MEMORY; }
                    for (uint32_t i = 0; i < src->physicalDeviceCount; i++)
                        dg_pds[i] = PD_RAW(src->pPhysicalDevices[i]);
                    copy->pPhysicalDevices = dg_pds;
                }
                VkBaseOutStructure * cur = (VkBaseOutStructure *)copy;
                if (cloned_tail) cloned_tail->pNext = cur; else cloned_head = cur;
                cloned_tail = cur;
                /* preserve remainder of chain after DeviceGroup node */
                cur->pNext = (VkBaseOutStructure *)(uintptr_t)n->pNext;
                break;
            }
            /* shallow-clone prefix node header is unsafe for arbitrary size; only strip single-GPU dg */
            if (((const VkDeviceGroupDeviceCreateInfo *)n)->sType != VK_STRUCTURE_TYPE_DEVICE_GROUP_DEVICE_CREATE_INFO) {
                cloned_head = NULL;
                break;
            }
        }
        if (cloned_head) dci.pNext = cloned_head;
    }

    icd_device *dev = calloc(1, sizeof(*dev));
    if (!dev) { free(cloned_head); free(dg_pds); return VK_ERROR_OUT_OF_HOST_MEMORY; }
    set_loader_magic_value(dev);

    VkDevice raw_dev = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    VkResult r = c ? vkbc_vkCreateDevice(c, PD_RAW(pd), &dci, pAllocator, &raw_dev) : VK_ERROR_DEVICE_LOST;
    pthread_mutex_unlock(&g_mu);
    free(cloned_head);
    free(dg_pds);
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

static void icd_get_queue_common(
    VkDevice device, VkDeviceQueueCreateFlags flags,
    uint32_t queueFamilyIndex, uint32_t queueIndex, int use_v2, VkQueue *pQueue) {
    if (!device || !pQueue) return;
    icd_device *dev = (icd_device *)device;
    pthread_mutex_lock(&g_mu);
    for (icd_queue *q = dev->queues; q; q = q->next) {
        if (q->flags == flags && q->qfi == queueFamilyIndex && q->qi == queueIndex) {
            *pQueue = (VkQueue)q;
            pthread_mutex_unlock(&g_mu);
            return;
        }
    }
    vkbc *c = icd_conn_locked();
    VkQueue raw_q = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_DEVICE_LOST;
    if (c) {
        if (use_v2) {
            VkDeviceQueueInfo2 qi2 = {
                .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,
                .flags = flags,
                .queueFamilyIndex = queueFamilyIndex,
                .queueIndex = queueIndex,
            };
            r = vkbc_vkGetDeviceQueue2(c, DEV_RAW(device), &qi2, &raw_q);
        } else {
            r = vkbc_vkGetDeviceQueue(c, DEV_RAW(device), queueFamilyIndex, queueIndex, &raw_q);
        }
    }
    if (r == VK_SUCCESS && raw_q) {
        icd_queue *q = calloc(1, sizeof(*q));
        if (q) {
            set_loader_magic_value(q);
            q->id = (uint64_t)(uintptr_t)raw_q;
            q->flags = flags;
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

static VKAPI_ATTR void VKAPI_CALL icd_vkGetDeviceQueue(
    VkDevice device, uint32_t queueFamilyIndex, uint32_t queueIndex, VkQueue *pQueue) {
    icd_get_queue_common(device, 0, queueFamilyIndex, queueIndex, 0, pQueue);
}

static VKAPI_ATTR void VKAPI_CALL icd_vkGetDeviceQueue2(
    VkDevice device, const VkDeviceQueueInfo2 *pQueueInfo, VkQueue *pQueue) {
    if (!pQueueInfo) { if (pQueue) *pQueue = VK_NULL_HANDLE; return; }
    icd_get_queue_common(device, pQueueInfo->flags, pQueueInfo->queueFamilyIndex,
                         pQueueInfo->queueIndex, pQueueInfo->flags != 0, pQueue);
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkQueueSubmit(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo *pSubmits, VkFence fence) {
    if (!queue) return VK_ERROR_DEVICE_LOST;
    VkSubmitInfo stack_subs[4];
    VkCommandBuffer stack_cbs[16];
    VkSubmitInfo *subs = NULL;
    VkCommandBuffer *cbs = NULL;
    int heap_subs = 0, heap_cbs = 0;
    if (submitCount > 0 && pSubmits) {
        uint32_t total_cb = 0;
        for (uint32_t i = 0; i < submitCount; i++) total_cb += pSubmits[i].commandBufferCount;
        if (submitCount <= 4) subs = stack_subs;
        else { subs = calloc(submitCount, sizeof(*subs)); heap_subs = 1; }
        if (!subs) return VK_ERROR_OUT_OF_HOST_MEMORY;
        if (total_cb > 0) {
            if (total_cb <= 16) cbs = stack_cbs;
            else { cbs = calloc(total_cb, sizeof(*cbs)); heap_cbs = 1; }
            if (!cbs) { if (heap_subs) free(subs); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        }
        uint32_t off = 0;
        for (uint32_t i = 0; i < submitCount; i++) {
            subs[i] = pSubmits[i];
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
    if (heap_cbs) free(cbs);
    if (heap_subs) free(subs);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkQueueSubmit2(
    VkQueue queue, uint32_t submitCount, const VkSubmitInfo2 *pSubmits, VkFence fence) {
    if (!queue) return VK_ERROR_DEVICE_LOST;
    VkSubmitInfo2 stack_subs[4];
    VkCommandBufferSubmitInfo stack_cbis[16];
    VkSubmitInfo2 *subs = NULL;
    VkCommandBufferSubmitInfo *cbis = NULL;
    int heap_subs = 0, heap_cbis = 0;
    if (submitCount > 0 && pSubmits) {
        uint32_t total_cb = 0;
        for (uint32_t i = 0; i < submitCount; i++) total_cb += pSubmits[i].commandBufferInfoCount;
        if (submitCount <= 4) subs = stack_subs;
        else { subs = calloc(submitCount, sizeof(*subs)); heap_subs = 1; }
        if (!subs) return VK_ERROR_OUT_OF_HOST_MEMORY;
        if (total_cb > 0) {
            if (total_cb <= 16) cbis = stack_cbis;
            else { cbis = calloc(total_cb, sizeof(*cbis)); heap_cbis = 1; }
            if (!cbis) { if (heap_subs) free(subs); return VK_ERROR_OUT_OF_HOST_MEMORY; }
        }
        uint32_t off = 0;
        for (uint32_t i = 0; i < submitCount; i++) {
            subs[i] = pSubmits[i];
            if (subs[i].commandBufferInfoCount > 0 && subs[i].pCommandBufferInfos) {
                for (uint32_t j = 0; j < subs[i].commandBufferInfoCount; j++) {
                    cbis[off + j] = subs[i].pCommandBufferInfos[j];
                    cbis[off + j].commandBuffer = CMDBUF_RAW(cbis[off + j].commandBuffer);
                }
                subs[i].pCommandBufferInfos = &cbis[off];
                off += subs[i].commandBufferInfoCount;
            }
        }
    }
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    VkResult r = c ? vkbc_vkQueueSubmit2(c, QUEUE_RAW(queue), submitCount, subs, fence) : VK_ERROR_DEVICE_LOST;
    pthread_mutex_unlock(&g_mu);
    if (heap_cbis) free(cbis);
    if (heap_subs) free(subs);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkAllocateMemory(
    VkDevice device, const VkMemoryAllocateInfo *pAllocateInfo,
    const VkAllocationCallbacks *pAllocator, VkDeviceMemory *pMemory) {
    int imp_fd = -1;
    if (pAllocateInfo) {
        for (const VkBaseInStructure *n = (const VkBaseInStructure *)pAllocateInfo->pNext; n; n = n->pNext) {
            if (n->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR) {
                const VkImportMemoryFdInfoKHR *imp = (const VkImportMemoryFdInfoKHR *)n;
                if (imp->handleType != 0 && imp->fd >= 0) imp_fd = imp->fd;
                break;
            }
        }
    }
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    VkResult r = VK_ERROR_DEVICE_LOST;
    if (c) {
        if (imp_fd >= 0 && vkbc_send_fd(c, 'I', imp_fd) < 0) {
            r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
        } else {
            r = vkbc_vkAllocateMemory(c, DEV_RAW(device), pAllocateInfo, pAllocator, pMemory);
            if (r == VK_SUCCESS && imp_fd >= 0) close(imp_fd);
        }
    }
    pthread_mutex_unlock(&g_mu);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkMapMemory(
    VkDevice device, VkDeviceMemory memory, VkDeviceSize offset,
    VkDeviceSize size, VkMemoryMapFlags flags, void **ppData) {
    ICD_CALL_RES(vkbc_vkMapMemory(c_, DEV_RAW(device), memory, offset, size, flags, ppData));
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkGetMemoryFdKHR(
    VkDevice device, const VkMemoryGetFdInfoKHR *pGetFdInfo, int *pFd) {
    if (!pGetFdInfo || !pFd) return VK_ERROR_INITIALIZATION_FAILED;
    int dummy_fd = -1;
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    VkResult r = c ? vkbc_vkGetMemoryFdKHR(c, DEV_RAW(device), pGetFdInfo, &dummy_fd) : VK_ERROR_DEVICE_LOST;
    if (r == VK_SUCCESS) {
        int fd = vkbc_recv_fd(c, 'F');
        if (fd < 0) r = VK_ERROR_OUT_OF_HOST_MEMORY;
        else *pFd = fd;
    }
    pthread_mutex_unlock(&g_mu);
    return r;
}

static VKAPI_ATTR VkResult VKAPI_CALL icd_vkGetMemoryFdPropertiesKHR(
    VkDevice device, VkExternalMemoryHandleTypeFlagBits handleType,
    int fd, VkMemoryFdPropertiesKHR *pMemoryFdProperties) {
    if (fd < 0) return VK_ERROR_INVALID_EXTERNAL_HANDLE;
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    VkResult r = VK_ERROR_DEVICE_LOST;
    if (c) {
        if (vkbc_send_fd(c, 'P', fd) < 0) r = VK_ERROR_INVALID_EXTERNAL_HANDLE;
        else r = vkbc_vkGetMemoryFdPropertiesKHR(c, DEV_RAW(device), handleType, fd, pMemoryFdProperties);
    }
    pthread_mutex_unlock(&g_mu);
    return r;
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
            wraps[i]->level = ai.level;
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
    if (!commandBuffer || !pBeginInfo) return VK_ERROR_INITIALIZATION_FAILED;
    icd_cmdbuf *cb = (icd_cmdbuf *)commandBuffer;
    VkCommandBufferBeginInfo bi = *pBeginInfo;
    VkCommandBufferInheritanceInfo inh;
    if (cb->level == VK_COMMAND_BUFFER_LEVEL_PRIMARY) {
        bi.pInheritanceInfo = NULL;
    } else if (bi.pInheritanceInfo && !(bi.flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT)) {
        inh = *bi.pInheritanceInfo;
        inh.renderPass = VK_NULL_HANDLE;
        inh.framebuffer = VK_NULL_HANDLE;
        bi.pInheritanceInfo = &inh;
    }
    ICD_CALL_RES(vkbc_vkBeginCommandBuffer(c_, CMDBUF_RAW(commandBuffer), &bi));
}

static VKAPI_ATTR void VKAPI_CALL icd_vkCmdExecuteCommands(
    VkCommandBuffer commandBuffer, uint32_t commandBufferCount, const VkCommandBuffer *pCommandBuffers) {
    if (!commandBuffer || !commandBufferCount || !pCommandBuffers) return;
    VkCommandBuffer stack_raw[16];
    VkCommandBuffer *raw = commandBufferCount <= 16 ? stack_raw : calloc(commandBufferCount, sizeof(*raw));
    if (!raw) return;
    for (uint32_t i = 0; i < commandBufferCount; i++) raw[i] = CMDBUF_RAW(pCommandBuffers[i]);
    pthread_mutex_lock(&g_mu);
    vkbc *c = icd_conn_locked();
    if (c) (void)vkbc_vkCmdExecuteCommands(c, CMDBUF_RAW(commandBuffer), commandBufferCount, raw);
    pthread_mutex_unlock(&g_mu);
    if (raw != stack_raw) free(raw);
}

/* Client-side VkDescriptorUpdateTemplate expansion into VkWriteDescriptorSet[] */
static VKAPI_ATTR VkResult VKAPI_CALL icd_vkCreateDescriptorUpdateTemplate(
    VkDevice device, const VkDescriptorUpdateTemplateCreateInfo *pCreateInfo,
    const VkAllocationCallbacks *pAllocator, VkDescriptorUpdateTemplate *pDescriptorUpdateTemplate) {
    (void)device; (void)pAllocator;
    if (!pCreateInfo || !pDescriptorUpdateTemplate) return VK_ERROR_INITIALIZATION_FAILED;
    uint32_t n = pCreateInfo->descriptorUpdateEntryCount;
    icd_desc_template *t = calloc(1, sizeof(*t) + (size_t)n * sizeof(t->entries[0]));
    if (!t) return VK_ERROR_OUT_OF_HOST_MEMORY;
    t->type = pCreateInfo->templateType;
    t->bind_point = pCreateInfo->pipelineBindPoint;
    t->layout = pCreateInfo->pipelineLayout;
    t->set = pCreateInfo->set;
    t->entry_count = n;
    if (n && pCreateInfo->pDescriptorUpdateEntries)
        memcpy(t->entries, pCreateInfo->pDescriptorUpdateEntries, (size_t)n * sizeof(t->entries[0]));
    *pDescriptorUpdateTemplate = (VkDescriptorUpdateTemplate)(uintptr_t)t;
    return VK_SUCCESS;
}

static VKAPI_ATTR void VKAPI_CALL icd_vkDestroyDescriptorUpdateTemplate(
    VkDevice device, VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    const VkAllocationCallbacks *pAllocator) {
    (void)device; (void)pAllocator;
    free((void *)(uintptr_t)descriptorUpdateTemplate);
}

static void icd_apply_desc_template(
    VkDevice device, VkCommandBuffer cmdbuf, const icd_desc_template *t,
    VkDescriptorSet dstSet, VkPipelineLayout layout, uint32_t set, const void *pData) {
    if (!t || !t->entry_count || (!pData && t->entry_count)) return;
    const uint8_t *base = (const uint8_t *)pData;
    uint32_t total_img = 0, total_buf = 0, total_tex = 0, total_iub = 0;
    for (uint32_t i = 0; i < t->entry_count; i++) {
        const VkDescriptorUpdateTemplateEntry *e = &t->entries[i];
        if (!e->descriptorCount) continue;
        switch (e->descriptorType) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            total_img += e->descriptorCount;
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            total_buf += e->descriptorCount;
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            total_tex += e->descriptorCount;
            break;
        case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
            total_iub++;
            break;
        default:
            break;
        }
    }

    VkWriteDescriptorSet stack_wr[32];
    VkDescriptorImageInfo stack_img[64];
    VkDescriptorBufferInfo stack_buf[64];
    VkBufferView stack_tex[64];
    VkWriteDescriptorSetInlineUniformBlock stack_iub[16];

    VkWriteDescriptorSet *wr = t->entry_count <= 32 ? stack_wr : calloc(t->entry_count, sizeof(*wr));
    VkDescriptorImageInfo *imgs = total_img <= 64 ? stack_img : calloc(total_img, sizeof(*imgs));
    VkDescriptorBufferInfo *bufs = total_buf <= 64 ? stack_buf : calloc(total_buf, sizeof(*bufs));
    VkBufferView *texs = total_tex <= 64 ? stack_tex : calloc(total_tex, sizeof(*texs));
    VkWriteDescriptorSetInlineUniformBlock *iubs = total_iub <= 16 ? stack_iub : calloc(total_iub, sizeof(*iubs));
    if (!wr || !imgs || !bufs || !texs || !iubs) goto cleanup;

    uint32_t nwr = 0, o_img = 0, o_buf = 0, o_tex = 0, o_iub = 0;
    for (uint32_t i = 0; i < t->entry_count; i++) {
        const VkDescriptorUpdateTemplateEntry *e = &t->entries[i];
        if (!e->descriptorCount) continue;
        VkWriteDescriptorSet w = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = dstSet,
            .dstBinding = e->dstBinding,
            .dstArrayElement = e->dstArrayElement,
            .descriptorCount = e->descriptorCount,
            .descriptorType = e->descriptorType,
        };
        switch (e->descriptorType) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            w.pImageInfo = &imgs[o_img];
            for (uint32_t j = 0; j < e->descriptorCount; j++)
                memcpy(&imgs[o_img++], base + e->offset + (size_t)j * e->stride, sizeof(VkDescriptorImageInfo));
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            w.pBufferInfo = &bufs[o_buf];
            for (uint32_t j = 0; j < e->descriptorCount; j++)
                memcpy(&bufs[o_buf++], base + e->offset + (size_t)j * e->stride, sizeof(VkDescriptorBufferInfo));
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            w.pTexelBufferView = &texs[o_tex];
            for (uint32_t j = 0; j < e->descriptorCount; j++)
                memcpy(&texs[o_tex++], base + e->offset + (size_t)j * e->stride, sizeof(VkBufferView));
            break;
        case VK_DESCRIPTOR_TYPE_INLINE_UNIFORM_BLOCK:
            iubs[o_iub] = (VkWriteDescriptorSetInlineUniformBlock){
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_INLINE_UNIFORM_BLOCK,
                .dataSize = e->descriptorCount,
                .pData = base + e->offset,
            };
            w.pNext = &iubs[o_iub++];
            break;
        default:
            continue;
        }
        wr[nwr++] = w;
    }

    if (nwr > 0) {
        pthread_mutex_lock(&g_mu);
        vkbc *c = icd_conn_locked();
        if (c) {
            if (cmdbuf)
                (void)vkbc_vkCmdPushDescriptorSet(c, CMDBUF_RAW(cmdbuf), t->bind_point, layout, set, nwr, wr);
            else
                (void)vkbc_vkUpdateDescriptorSets(c, DEV_RAW(device), nwr, wr, 0, NULL);
        }
        pthread_mutex_unlock(&g_mu);
    }

cleanup:
    if (wr != stack_wr) free(wr);
    if (imgs != stack_img) free(imgs);
    if (bufs != stack_buf) free(bufs);
    if (texs != stack_tex) free(texs);
    if (iubs != stack_iub) free(iubs);
}

static VKAPI_ATTR void VKAPI_CALL icd_vkUpdateDescriptorSetWithTemplate(
    VkDevice device, VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    VkDescriptorSet descriptorSet, const void *pData) {
    const icd_desc_template *t = (const icd_desc_template *)(uintptr_t)descriptorUpdateTemplate;
    icd_apply_desc_template(device, VK_NULL_HANDLE, t, descriptorSet, VK_NULL_HANDLE, 0, pData);
}

static VKAPI_ATTR void VKAPI_CALL icd_vkCmdPushDescriptorSetWithTemplate(
    VkCommandBuffer commandBuffer, VkDescriptorUpdateTemplate descriptorUpdateTemplate,
    VkPipelineLayout layout, uint32_t set, const void *pData) {
    const icd_desc_template *t = (const icd_desc_template *)(uintptr_t)descriptorUpdateTemplate;
    icd_apply_desc_template(VK_NULL_HANDLE, commandBuffer, t, VK_NULL_HANDLE,
                            layout ? layout : (t ? t->layout : VK_NULL_HANDLE), set, pData);
}

/* ---- ProcAddr tables & generated ICD thunks ---- */

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

#include "vkb_icd_gen.inc"

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
