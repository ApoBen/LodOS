/* LodOS T010: smoke test for libvulkan_lodos_mali.so (bionic-vulkan-wrapper ICD).
 * dlopen wrapper -> vk_icdGetInstanceProcAddr -> instance/phys dev; print deviceName,
 * textureCompressionBC; upload one 4x4 BC1 block, copy decoded image back, verify. */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DIE(...) do { printf(__VA_ARGS__); printf("\nBC1_DECODE=fail\n"); exit(1); } while (0)
#define VKC(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) DIE("%s -> %d", #x, r_); } while (0)

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance inst;
static VkDevice dev;
#define IFN(n) PFN_##n n = (PFN_##n)gipa(inst, #n); if (!n) DIE("missing " #n)
#define DFN(n) PFN_##n n = (PFN_##n)gdpa(dev, #n); if (!n) DIE("missing " #n)

static uint32_t find_mem(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags want)
{
   for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
      if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & want) == want) return i;
   DIE("no memory type bits=%x want=%x", bits, want);
}

int main(int argc, char **argv)
{
   const char *lib = argc > 1 ? argv[1] : "libvulkan_lodos_mali.so";
   setvbuf(stdout, NULL, _IONBF, 0);
   void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
   if (!h) DIE("dlopen %s: %s", lib, dlerror());
   VkResult (*neg)(uint32_t *) = (VkResult (*)(uint32_t *))dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
   if (neg) { uint32_t v = 5; neg(&v); printf("icdInterfaceVersion=%u\n", v); }
   gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
   if (!gipa) gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
   if (!gipa) DIE("no GetInstanceProcAddr");

   PFN_vkCreateInstance vkCreateInstance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
   if (!vkCreateInstance) DIE("missing vkCreateInstance");
   VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "lodos-vklayer-smoke", 1, "lodos", 1, VK_API_VERSION_1_1 };
   VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
   VKC(vkCreateInstance(&ici, NULL, &inst));

   IFN(vkEnumeratePhysicalDevices); IFN(vkGetPhysicalDeviceProperties); IFN(vkGetPhysicalDeviceFeatures);
   IFN(vkGetPhysicalDeviceQueueFamilyProperties); IFN(vkGetPhysicalDeviceMemoryProperties);
   IFN(vkGetPhysicalDeviceFormatProperties); IFN(vkCreateDevice);
   gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
   if (!gdpa) DIE("missing vkGetDeviceProcAddr");

   uint32_t n = 1; VkPhysicalDevice pd;
   VkResult er = vkEnumeratePhysicalDevices(inst, &n, &pd);
   if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || n == 0) DIE("no physical device (%d)", er);
   VkPhysicalDeviceProperties props; vkGetPhysicalDeviceProperties(pd, &props);
   VkPhysicalDeviceFeatures feats; vkGetPhysicalDeviceFeatures(pd, &feats);
   printf("deviceName=%s\n", props.deviceName);
   printf("apiVersion=%u.%u.%u driverVersion=0x%x\n", VK_VERSION_MAJOR(props.apiVersion),
          VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion), props.driverVersion);
   printf("textureCompressionBC=%d\n", feats.textureCompressionBC ? 1 : 0);
   VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(pd, VK_FORMAT_BC1_RGB_UNORM_BLOCK, &fp);
   printf("BC1_RGB optimalTilingFeatures=0x%x\n", fp.optimalTilingFeatures);

   uint32_t qn = 0; vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, NULL);
   VkQueueFamilyProperties qfp[16]; if (qn > 16) qn = 16;
   vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qfp);
   uint32_t qf = UINT32_MAX;
   for (uint32_t i = 0; i < qn; i++) if (qfp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qf = i; break; }
   if (qf == UINT32_MAX) DIE("no graphics queue");

   float prio = 1.0f;
   VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qf, 1, &prio };
   VkPhysicalDeviceFeatures en = { 0 };
   en.textureCompressionBC = feats.textureCompressionBC;
   VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 0, NULL, &en };
   VKC(vkCreateDevice(pd, &dci, NULL, &dev));
   printf("device=ok\n");

   DFN(vkGetDeviceQueue); DFN(vkCreateBuffer); DFN(vkGetBufferMemoryRequirements); DFN(vkAllocateMemory);
   DFN(vkBindBufferMemory); DFN(vkCreateImage); DFN(vkGetImageMemoryRequirements); DFN(vkBindImageMemory);
   DFN(vkMapMemory); DFN(vkUnmapMemory); DFN(vkCreateCommandPool); DFN(vkAllocateCommandBuffers);
   DFN(vkBeginCommandBuffer); DFN(vkEndCommandBuffer); DFN(vkCmdPipelineBarrier); DFN(vkCmdCopyBufferToImage);
   DFN(vkCmdCopyImageToBuffer); DFN(vkCreateFence); DFN(vkQueueSubmit); DFN(vkWaitForFences);
   DFN(vkDeviceWaitIdle); DFN(vkDestroyDevice);
   PFN_vkDestroyInstance vkDestroyInstance = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");

   VkQueue q; vkGetDeviceQueue(dev, qf, 0, &q);
   VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   const VkMemoryPropertyFlags HV = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

   /* BC1 block: c0=red(0xF800) > c1=blue(0x001F) -> 4-colour mode; row r uses index r. */
   const uint8_t block[8] = { 0x00, 0xF8, 0x1F, 0x00, 0x00, 0x55, 0xAA, 0xFF };
   const int expect[4][3] = { {255, 0, 0}, {0, 0, 255}, {170, 0, 85}, {85, 0, 170} };

   VkBuffer src, dst; VkDeviceMemory srcm, dstm;
   VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 256,
      VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
   VKC(vkCreateBuffer(dev, &bci, NULL, &src));
   bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
   VKC(vkCreateBuffer(dev, &bci, NULL, &dst));
   VkMemoryRequirements mr;
   vkGetBufferMemoryRequirements(dev, src, &mr);
   VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, find_mem(&mp, mr.memoryTypeBits, HV) };
   VKC(vkAllocateMemory(dev, &mai, NULL, &srcm)); VKC(vkBindBufferMemory(dev, src, srcm, 0));
   vkGetBufferMemoryRequirements(dev, dst, &mr);
   mai.allocationSize = mr.size; mai.memoryTypeIndex = find_mem(&mp, mr.memoryTypeBits, HV);
   VKC(vkAllocateMemory(dev, &mai, NULL, &dstm)); VKC(vkBindBufferMemory(dev, dst, dstm, 0));
   void *p;
   VKC(vkMapMemory(dev, srcm, 0, VK_WHOLE_SIZE, 0, &p)); memset(p, 0, 256); memcpy(p, block, 8); vkUnmapMemory(dev, srcm);
   VKC(vkMapMemory(dev, dstm, 0, VK_WHOLE_SIZE, 0, &p)); memset(p, 0xCD, 256); vkUnmapMemory(dev, dstm);

   VkImage img; VkDeviceMemory imgm;
   VkImageCreateInfo imci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D,
      VK_FORMAT_BC1_RGB_UNORM_BLOCK, { 4, 4, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
      VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
   VKC(vkCreateImage(dev, &imci, NULL, &img));
   vkGetImageMemoryRequirements(dev, img, &mr);
   mai.allocationSize = mr.size; mai.memoryTypeIndex = find_mem(&mp, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
   VKC(vkAllocateMemory(dev, &mai, NULL, &imgm)); VKC(vkBindImageMemory(dev, img, imgm, 0));

   VkCommandPool pool; VkCommandBuffer cb;
   VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, qf };
   VKC(vkCreateCommandPool(dev, &pci, NULL, &pool));
   VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
   VKC(vkAllocateCommandBuffers(dev, &cai, &cb));
   VkCommandBufferBeginInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
   VKC(vkBeginCommandBuffer(cb, &cbi));
   VkImageSubresourceRange rng = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
   VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, rng };
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
   VkBufferImageCopy reg = { 0, 0, 0, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { 4, 4, 1 } };
   vkCmdCopyBufferToImage(cb, src, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &reg);
   b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT;
   b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
   b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
   /* Emulated BC1 image is backed by R8G8B8A8: 4x4 texels -> 64 bytes RGBA. */
   vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, 1, &reg);
   VkBufferMemoryBarrier hb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
      VK_ACCESS_HOST_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, dst, 0, VK_WHOLE_SIZE };
   vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &hb, 0, NULL);
   VKC(vkEndCommandBuffer(cb));
   VkFence fence; VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
   VKC(vkCreateFence(dev, &fci, NULL, &fence));
   VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL };
   VKC(vkQueueSubmit(q, 1, &si, fence));
   VKC(vkWaitForFences(dev, 1, &fence, VK_TRUE, 5000000000ull));
   vkDeviceWaitIdle(dev);

   VKC(vkMapMemory(dev, dstm, 0, VK_WHOLE_SIZE, 0, &p));
   const uint8_t *px = p; int ok = 1;
   for (int y = 0; y < 4; y++) {
      printf("row%d:", y);
      for (int x = 0; x < 4; x++) {
         const uint8_t *t = px + (y * 4 + x) * 4;
         printf(" %02x%02x%02x%02x", t[0], t[1], t[2], t[3]);
         for (int c = 0; c < 3; c++) if (abs(t[c] - expect[y][c]) > 8) ok = 0;
         if (t[3] != 255) ok = 0;
      }
      printf("\n");
   }
   vkUnmapMemory(dev, dstm);
   printf("BC1_DECODE=%s\n", ok ? "ok" : "fail");
   vkDestroyDevice(dev, NULL);
   if (vkDestroyInstance) vkDestroyInstance(inst, NULL);
   return ok ? 0 : 1;
}
