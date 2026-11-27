#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct InstanceDispatch {
    void *key;
    VkInstance instance;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkDestroyInstance destroy_instance;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR get_caps;
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR get_caps2;
    struct InstanceDispatch *next;
} InstanceDispatch;
typedef struct DeviceDispatch {
    void *key;
    VkDevice device;
    PFN_vkGetDeviceProcAddr gdpa;
    PFN_vkDestroyDevice destroy_device;
    PFN_vkCreateSwapchainKHR create_swapchain;
    struct DeviceDispatch *next;
} DeviceDispatch;
static InstanceDispatch *instances;
static DeviceDispatch *devices;
static int layer_enabled(void) {
    const char *v = getenv("LODOS_FG");
    return v && (!strcmp(v, "1") || !strcmp(v, "true") || !strcmp(v, "yes"));
}
static float scale_factor(void) {
    const char *p = getenv("LODOS_FG_PRESET");
    if (p && !strcmp(p, "quality")) return 1.5f;
    if (p && !strcmp(p, "performance")) return 2.0f;
    return 1.7f;
}
static void *dispatch_key(const void *handle) { return handle ? *(void *const *)handle : NULL; }
static InstanceDispatch *find_instance(const void *handle) {
    void *key = dispatch_key(handle);
    for (InstanceDispatch *d = instances; d; d = d->next) if (d->key == key) return d;
    return NULL;
}
static DeviceDispatch *find_device(const void *handle) {
    void *key = dispatch_key(handle);
    for (DeviceDispatch *d = devices; d; d = d->next) if (d->key == key) return d;
    return NULL;
}
static void scale_extent(VkExtent2D *e, VkExtent2D min) {
    float k = scale_factor();
    uint32_t w = (uint32_t)((float)e->width / k), h = (uint32_t)((float)e->height / k);
    e->width = w < min.width ? min.width : w;
    e->height = h < min.height ? min.height : h;
}
static VkResult VKAPI_CALL layer_caps(VkPhysicalDevice p, VkSurfaceKHR s, VkSurfaceCapabilitiesKHR *c) {
    InstanceDispatch *d = find_instance(p);
    if (!d || !d->get_caps) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = d->get_caps(p, s, c);
    if (r == VK_SUCCESS && layer_enabled()) {
        if (c->currentExtent.width != UINT32_MAX) scale_extent(&c->currentExtent, c->minImageExtent);
        if (c->maxImageExtent.width != UINT32_MAX) scale_extent(&c->maxImageExtent, c->minImageExtent);
    }
    return r;
}
static VkResult VKAPI_CALL layer_caps2(VkPhysicalDevice p, const VkPhysicalDeviceSurfaceInfo2KHR *info, VkSurfaceCapabilities2KHR *c) {
    InstanceDispatch *d = find_instance(p);
    if (!d || !d->get_caps2) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = d->get_caps2(p, info, c);
    if (r == VK_SUCCESS && layer_enabled()) {
        VkSurfaceCapabilitiesKHR *s = &c->surfaceCapabilities;
        if (s->currentExtent.width != UINT32_MAX) scale_extent(&s->currentExtent, s->minImageExtent);
        if (s->maxImageExtent.width != UINT32_MAX) scale_extent(&s->maxImageExtent, s->minImageExtent);
    }
    return r;
}
static VkResult VKAPI_CALL layer_create_instance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out) {
    VkLayerInstanceCreateInfo *chain = (VkLayerInstanceCreateInfo *)ci->pNext;
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO)) chain = (VkLayerInstanceCreateInfo *)chain->pNext;
    if (!chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    VkLayerInstanceLink *link = chain->u.pLayerInfo;
    PFN_vkGetInstanceProcAddr gipa = link->pfnNextGetInstanceProcAddr;
    chain->u.pLayerInfo = link->pNext;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(VK_NULL_HANDLE, "vkCreateInstance");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = create(ci, a, out);
    if (r != VK_SUCCESS) return r;
    InstanceDispatch *d = calloc(1, sizeof(*d));
    if (!d) return VK_ERROR_OUT_OF_HOST_MEMORY;
    d->key = dispatch_key(*out); d->instance = *out; d->gipa = gipa;
    d->gdpa = (PFN_vkGetDeviceProcAddr)gipa(*out, "vkGetDeviceProcAddr");
    d->destroy_instance = (PFN_vkDestroyInstance)gipa(*out, "vkDestroyInstance");
    d->get_caps = (PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)gipa(*out, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    d->get_caps2 = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)gipa(*out, "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
    if (!d->gdpa || !d->destroy_instance) {
        PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)gipa(*out, "vkDestroyInstance");
        if (destroy) destroy(*out, a);
        free(d);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    d->next = instances; instances = d;
    return r;
}
static void VKAPI_CALL layer_destroy_instance(VkInstance i, const VkAllocationCallbacks *a) {
    InstanceDispatch **it = &instances;
    while (*it && (*it)->key != dispatch_key(i)) it = &(*it)->next;
    if (*it) {
        InstanceDispatch *d = *it;
        *it = d->next;
        PFN_vkDestroyInstance destroy = d->destroy_instance;
        free(d);
        if (destroy) destroy(i, a);
    }
}
static VkResult VKAPI_CALL layer_create_device(VkPhysicalDevice p, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out) {
    InstanceDispatch *id = find_instance(p);
    if (!id) return VK_ERROR_INITIALIZATION_FAILED;
    VkLayerDeviceCreateInfo *chain = (VkLayerDeviceCreateInfo *)ci->pNext;
    while (chain && !(chain->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO && chain->function == VK_LAYER_LINK_INFO)) chain = (VkLayerDeviceCreateInfo *)chain->pNext;
    if (!chain || !chain->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
    VkLayerDeviceLink *link = chain->u.pLayerInfo;
    PFN_vkGetInstanceProcAddr gipa = link->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr gdpa = link->pfnNextGetDeviceProcAddr;
    chain->u.pLayerInfo = link->pNext;
    PFN_vkCreateDevice create = (PFN_vkCreateDevice)gipa(id->instance, "vkCreateDevice");
    if (!create) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = create(p, ci, a, out);
    if (r != VK_SUCCESS) return r;
    DeviceDispatch *d = calloc(1, sizeof(*d));
    if (!d) return VK_ERROR_OUT_OF_HOST_MEMORY;
    d->key = dispatch_key(*out); d->device = *out; d->gdpa = gdpa;
    d->destroy_device = (PFN_vkDestroyDevice)gdpa(*out, "vkDestroyDevice");
    d->create_swapchain = (PFN_vkCreateSwapchainKHR)gdpa(*out, "vkCreateSwapchainKHR");
    if (!d->destroy_device) {
        PFN_vkDestroyDevice destroy = (PFN_vkDestroyDevice)gdpa(*out, "vkDestroyDevice");
        if (destroy) destroy(*out, a);
        free(d);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    d->next = devices; devices = d;
    return r;
}
static void VKAPI_CALL layer_destroy_device(VkDevice dev, const VkAllocationCallbacks *a) {
    DeviceDispatch **it = &devices;
    while (*it && (*it)->key != dispatch_key(dev)) it = &(*it)->next;
    if (*it) {
        DeviceDispatch *d = *it;
        *it = d->next;
        PFN_vkDestroyDevice destroy = d->destroy_device;
        free(d);
        if (destroy) destroy(dev, a);
    }
}
static VkResult VKAPI_CALL layer_create_swapchain(VkDevice dev, const VkSwapchainCreateInfoKHR *ci, const VkAllocationCallbacks *a, VkSwapchainKHR *out) {
    DeviceDispatch *d = find_device(dev);
    if (!d || !d->create_swapchain) return VK_ERROR_INITIALIZATION_FAILED;
    static int reported;
    if (layer_enabled() && !reported) { fprintf(stderr, "LODOS_FG layer ok\n"); reported = 1; }
    return d->create_swapchain(dev, ci, a, out);
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance i, const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    if (!strcmp(name, "vkCreateInstance")) return (PFN_vkVoidFunction)layer_create_instance;
    InstanceDispatch *d = find_instance(i);
    if (!strcmp(name, "vkDestroyInstance")) return (PFN_vkVoidFunction)layer_destroy_instance;
    if (!strcmp(name, "vkCreateDevice")) return (PFN_vkVoidFunction)layer_create_device;
    if (!strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) return (PFN_vkVoidFunction)layer_caps;
    if (!strcmp(name, "vkGetPhysicalDeviceSurfaceCapabilities2KHR")) return (PFN_vkVoidFunction)layer_caps2;
    return d ? d->gipa(i, name) : NULL;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice dev, const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "vkGetDeviceProcAddr")) return (PFN_vkVoidFunction)vkGetDeviceProcAddr;
    if (!strcmp(name, "vkDestroyDevice")) return (PFN_vkVoidFunction)layer_destroy_device;
    if (!strcmp(name, "vkCreateSwapchainKHR")) return (PFN_vkVoidFunction)layer_create_swapchain;
    DeviceDispatch *d = find_device(dev);
    return d ? d->gdpa(dev, name) : NULL;
}
VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v) {
    if (!v) return VK_ERROR_INITIALIZATION_FAILED;
    if (v->loaderLayerInterfaceVersion > 2) v->loaderLayerInterfaceVersion = 2;
    v->pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
    v->pfnGetDeviceProcAddr = vkGetDeviceProcAddr;
    v->pfnGetPhysicalDeviceProcAddr = NULL;
    return VK_SUCCESS;
}
