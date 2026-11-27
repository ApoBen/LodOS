#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static PFN_vkGetInstanceProcAddr next_gipa;
static PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR next_caps;
static int layer_enabled(void) {
 const char *v=getenv("LODOS_FG");
 return v && (!strcmp(v,"1") || !strcmp(v,"true") || !strcmp(v,"yes"));
}
static float scale_factor(void) {
 const char *p=getenv("LODOS_FG_PRESET");
 if(p && !strcmp(p,"quality")) return 1.5f;
 if(p && !strcmp(p,"performance")) return 2.0f;
 return 1.7f;
}
static VkResult VKAPI_CALL layer_caps(VkPhysicalDevice p,VkSurfaceKHR s,VkSurfaceCapabilitiesKHR *c) {
 VkResult r=next_caps(p,s,c); if(r || !layer_enabled()) return r;
 float k=scale_factor();
 if(c->currentExtent.width!=UINT32_MAX) { c->currentExtent.width=(uint32_t)(c->currentExtent.width/k); c->currentExtent.height=(uint32_t)(c->currentExtent.height/k); }
 if(c->maxImageExtent.width!=UINT32_MAX) { c->maxImageExtent.width=(uint32_t)(c->maxImageExtent.width/k); c->maxImageExtent.height=(uint32_t)(c->maxImageExtent.height/k); }
 return r;
}
static VkResult VKAPI_CALL layer_create(const VkInstanceCreateInfo *ci,const VkAllocationCallbacks *a,VkInstance *out) {
 VkLayerInstanceCreateInfo *link=(void*)ci->pNext;
 while(link && !(link->sType==VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO && link->function==VK_LAYER_LINK_INFO)) link=(void*)link->pNext;
 if(!link || !link->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;
 VkLayerInstanceLink *li=link->u.pLayerInfo; next_gipa=li->pfnNextGetInstanceProcAddr;
 PFN_vkCreateInstance create=(PFN_vkCreateInstance)next_gipa(VK_NULL_HANDLE,"vkCreateInstance");
 link->u.pLayerInfo=li->pNext;
 VkResult r=create(ci,a,out); if(!r) next_caps=(PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR)next_gipa(*out,"vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
 return r;
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance i,const char *name) {
 if(!strcmp(name,"vkGetInstanceProcAddr")) return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
 if(!strcmp(name,"vkCreateInstance")) return (PFN_vkVoidFunction)layer_create;
 if(!strcmp(name,"vkGetPhysicalDeviceSurfaceCapabilitiesKHR")) return (PFN_vkVoidFunction)layer_caps;
 return next_gipa ? next_gipa(i,name) : NULL;
}
VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *v) {
 if(v->loaderLayerInterfaceVersion>2) v->loaderLayerInterfaceVersion=2;
 v->pfnGetInstanceProcAddr=vkGetInstanceProcAddr;
 v->pfnGetDeviceProcAddr=(PFN_vkGetDeviceProcAddr)vkGetInstanceProcAddr;
 v->pfnGetPhysicalDeviceProcAddr=(PFN_GetPhysicalDeviceProcAddr)vkGetInstanceProcAddr;
 return VK_SUCCESS;
}
