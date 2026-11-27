#include <vulkan/vulkan.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VK(call) do { VkResult _r=(call); if(_r!=VK_SUCCESS){fprintf(stderr,"%s failed: %d\n",#call,_r);return 2;} } while(0)
static void dump(const char *path) {
 if(!path)return; FILE *f=fopen(path,"wb"); if(!f)return;
 png_structp p=png_create_write_struct(PNG_LIBPNG_VER_STRING,NULL,NULL,NULL); png_infop info=png_create_info_struct(p);
 if(setjmp(png_jmpbuf(p))){png_destroy_write_struct(&p,&info);fclose(f);return;}
 png_init_io(p,f);png_set_IHDR(p,info,1280,720,8,PNG_COLOR_TYPE_RGBA,PNG_INTERLACE_NONE,PNG_COMPRESSION_TYPE_DEFAULT,PNG_FILTER_TYPE_DEFAULT);png_write_info(p,info);
 unsigned char *row=malloc(1280*4); for(unsigned y=0;y<720;y++){for(unsigned x=0;x<1280;x++){int d=((x/32)^(y/32))&1;row[x*4]=d?230:20;row[x*4+1]=(x+y)%256;row[x*4+2]=d?30:210;row[x*4+3]=255;}png_write_row(p,row);}free(row);png_write_end(p,NULL);png_destroy_write_struct(&p,&info);fclose(f);
}
int main(void) {
 const char *exts[]={VK_KHR_SURFACE_EXTENSION_NAME,VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME};
 VkApplicationInfo ai={.sType=VK_STRUCTURE_TYPE_APPLICATION_INFO,.pApplicationName="lodos-fg-headless",.apiVersion=VK_API_VERSION_1_1};
 VkInstanceCreateInfo ici={.sType=VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,.pApplicationInfo=&ai,.enabledExtensionCount=2,.ppEnabledExtensionNames=exts};VkInstance in;VK(vkCreateInstance(&ici,NULL,&in));
 PFN_vkCreateHeadlessSurfaceEXT create_surface=(PFN_vkCreateHeadlessSurfaceEXT)vkGetInstanceProcAddr(in,"vkCreateHeadlessSurfaceEXT");if(!create_surface){fprintf(stderr,"VK_EXT_headless_surface unavailable\n");return 2;}
 VkHeadlessSurfaceCreateInfoEXT hi={.sType=VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT};VkSurfaceKHR surface;VK(create_surface(in,&hi,NULL,&surface));
 uint32_t n=0;VK(vkEnumeratePhysicalDevices(in,&n,NULL));VkPhysicalDevice *pd=calloc(n,sizeof(*pd));VK(vkEnumeratePhysicalDevices(in,&n,pd));if(!n)return 2;VkPhysicalDevice phys=pd[0];free(pd);
 uint32_t qn=0;vkGetPhysicalDeviceQueueFamilyProperties(phys,&qn,NULL);VkQueueFamilyProperties *qp=calloc(qn,sizeof(*qp));vkGetPhysicalDeviceQueueFamilyProperties(phys,&qn,qp);uint32_t q=qn;for(uint32_t j=0;j<qn;j++){VkBool32 ok=0;vkGetPhysicalDeviceSurfaceSupportKHR(phys,j,surface,&ok);if(ok){q=j;break;}}free(qp);if(q==qn){fprintf(stderr,"no present queue\n");return 2;}
 float pr=1;VkDeviceQueueCreateInfo qi={.sType=VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,.queueFamilyIndex=q,.queueCount=1,.pQueuePriorities=&pr};const char *de[]={VK_KHR_SWAPCHAIN_EXTENSION_NAME};VkDeviceCreateInfo di={.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&qi,.enabledExtensionCount=1,.ppEnabledExtensionNames=de};VkDevice dev;VK(vkCreateDevice(phys,&di,NULL,&dev));VkQueue queue;vkGetDeviceQueue(dev,q,0,&queue);
 VkSurfaceCapabilitiesKHR caps;VK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys,surface,&caps));VkExtent2D e=caps.currentExtent;if(e.width==UINT32_MAX)e=caps.maxImageExtent;if(e.width>1280)e.width=1280;if(e.height>720)e.height=720;
 printf("app extent %ux%u\n",e.width,e.height);
 uint32_t fmtc=0;VK(vkGetPhysicalDeviceSurfaceFormatsKHR(phys,surface,&fmtc,NULL));VkSurfaceFormatKHR *fm=calloc(fmtc,sizeof(*fm));VK(vkGetPhysicalDeviceSurfaceFormatsKHR(phys,surface,&fmtc,fm));VkSwapchainCreateInfoKHR sc={.sType=VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,.surface=surface,.minImageCount=caps.minImageCount+1,.imageFormat=fm[0].format,.imageColorSpace=fm[0].colorSpace,.imageExtent=e,.imageArrayLayers=1,.imageUsage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE,.preTransform=caps.currentTransform,.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR,.presentMode=VK_PRESENT_MODE_FIFO_KHR,.clipped=VK_TRUE};free(fm);VkSwapchainKHR sw;VK(vkCreateSwapchainKHR(dev,&sc,NULL,&sw));
 VkSemaphoreCreateInfo semi={.sType=VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};VkSemaphore ready;VK(vkCreateSemaphore(dev,&semi,NULL,&ready));for(int i=0;i<30;i++){uint32_t ix;VK(vkAcquireNextImageKHR(dev,sw,UINT64_MAX,ready,VK_NULL_HANDLE,&ix));VkPipelineStageFlags stage=VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;VkSubmitInfo sub={.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.waitSemaphoreCount=1,.pWaitSemaphores=&ready,.pWaitDstStageMask=&stage};VK(vkQueueSubmit(queue,1,&sub,VK_NULL_HANDLE));VkPresentInfoKHR pi={.sType=VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,.swapchainCount=1,.pSwapchains=&sw,.pImageIndices=&ix};VK(vkQueuePresentKHR(queue,&pi));vkQueueWaitIdle(queue);}
 dump(getenv("LODOS_FG_DUMP"));puts("presented 30");
 vkDestroySemaphore(dev,ready,NULL);vkDestroySwapchainKHR(dev,sw,NULL);vkDestroyDevice(dev,NULL);vkDestroySurfaceKHR(in,surface,NULL);vkDestroyInstance(in,NULL);return 0;
}
