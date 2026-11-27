/* T032 PC-host offscreen triangle client via Khronos Vulkan Loader -> libvulkan_lodos.so -> vk-bridge -> host ICD.
 * Renders a red triangle on a cleared black 256x256 RGBA8 color attachment, copies to a host-visible buffer,
 * maps + invalidates, verifies center pixel red and corner pixels black, and writes out.ppm. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>

#define W 256u
#define H 256u

#define DIE(...) do { fprintf(stderr, "HOST_TRIANGLE FAIL: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)
#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) DIE("%s -> %d", #x, (int)r_); } while (0)

/* Checked-in SPIR-V fallback (Vulkan 1.0, glslangValidator -V); overridden at build time when glslangValidator is present.
 * Vertex shader (#version 450):
 *   const vec2 pos[3] = vec2[3](vec2(0.0, -0.6), vec2(0.6, 0.6), vec2(-0.6, 0.6));
 *   void main() { gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0); }
 * Fragment shader (#version 450):
 *   layout(location = 0) out vec4 outColor;
 *   void main() { outColor = vec4(1.0, 0.0, 0.0, 1.0); } */
#ifdef LODOS_TRIANGLE_VERT_SPV
#include LODOS_TRIANGLE_VERT_SPV
#else
static const uint32_t k_vert_spv[] = {
    0x07230203u, 0x00010000u, 0x0008000bu, 0x00000028u, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0007000fu, 0x00000000u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000du, 0x0000001bu, 0x00030003u,
    0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00060005u, 0x0000000bu,
    0x505f6c67u, 0x65567265u, 0x78657472u, 0x00000000u, 0x00060006u, 0x0000000bu, 0x00000000u, 0x505f6c67u,
    0x7469736fu, 0x006e6f69u, 0x00070006u, 0x0000000bu, 0x00000001u, 0x505f6c67u, 0x746e696fu, 0x657a6953u,
    0x00000000u, 0x00070006u, 0x0000000bu, 0x00000002u, 0x435f6c67u, 0x4470696cu, 0x61747369u, 0x0065636eu,
    0x00070006u, 0x0000000bu, 0x00000003u, 0x435f6c67u, 0x446c6c75u, 0x61747369u, 0x0065636eu, 0x00030005u,
    0x0000000du, 0x00000000u, 0x00060005u, 0x0000001bu, 0x565f6c67u, 0x65747265u, 0x646e4978u, 0x00007865u,
    0x00050005u, 0x0000001eu, 0x65646e69u, 0x6c626178u, 0x00000065u, 0x00030047u, 0x0000000bu, 0x00000002u,
    0x00050048u, 0x0000000bu, 0x00000000u, 0x0000000bu, 0x00000000u, 0x00050048u, 0x0000000bu, 0x00000001u,
    0x0000000bu, 0x00000001u, 0x00050048u, 0x0000000bu, 0x00000002u, 0x0000000bu, 0x00000003u, 0x00050048u,
    0x0000000bu, 0x00000003u, 0x0000000bu, 0x00000004u, 0x00040047u, 0x0000001bu, 0x0000000bu, 0x0000002au,
    0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u, 0x00000020u,
    0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u, 0x00040015u, 0x00000008u, 0x00000020u, 0x00000000u,
    0x0004002bu, 0x00000008u, 0x00000009u, 0x00000001u, 0x0004001cu, 0x0000000au, 0x00000006u, 0x00000009u,
    0x0006001eu, 0x0000000bu, 0x00000007u, 0x00000006u, 0x0000000au, 0x0000000au, 0x00040020u, 0x0000000cu,
    0x00000003u, 0x0000000bu, 0x0004003bu, 0x0000000cu, 0x0000000du, 0x00000003u, 0x00040015u, 0x0000000eu,
    0x00000020u, 0x00000001u, 0x0004002bu, 0x0000000eu, 0x0000000fu, 0x00000000u, 0x00040017u, 0x00000010u,
    0x00000006u, 0x00000002u, 0x0004002bu, 0x00000008u, 0x00000011u, 0x00000003u, 0x0004001cu, 0x00000012u,
    0x00000010u, 0x00000011u, 0x0004002bu, 0x00000006u, 0x00000013u, 0x00000000u, 0x0004002bu, 0x00000006u,
    0x00000014u, 0xbf19999au, 0x0005002cu, 0x00000010u, 0x00000015u, 0x00000013u, 0x00000014u, 0x0004002bu,
    0x00000006u, 0x00000016u, 0x3f19999au, 0x0005002cu, 0x00000010u, 0x00000017u, 0x00000016u, 0x00000016u,
    0x0005002cu, 0x00000010u, 0x00000018u, 0x00000014u, 0x00000016u, 0x0006002cu, 0x00000012u, 0x00000019u,
    0x00000015u, 0x00000017u, 0x00000018u, 0x00040020u, 0x0000001au, 0x00000001u, 0x0000000eu, 0x0004003bu,
    0x0000001au, 0x0000001bu, 0x00000001u, 0x00040020u, 0x0000001du, 0x00000007u, 0x00000012u, 0x00040020u,
    0x0000001fu, 0x00000007u, 0x00000010u, 0x0004002bu, 0x00000006u, 0x00000022u, 0x3f800000u, 0x00040020u,
    0x00000026u, 0x00000003u, 0x00000007u, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u, 0x00000003u,
    0x000200f8u, 0x00000005u, 0x0004003bu, 0x0000001du, 0x0000001eu, 0x00000007u, 0x0004003du, 0x0000000eu,
    0x0000001cu, 0x0000001bu, 0x0003003eu, 0x0000001eu, 0x00000019u, 0x00050041u, 0x0000001fu, 0x00000020u,
    0x0000001eu, 0x0000001cu, 0x0004003du, 0x00000010u, 0x00000021u, 0x00000020u, 0x00050051u, 0x00000006u,
    0x00000023u, 0x00000021u, 0x00000000u, 0x00050051u, 0x00000006u, 0x00000024u, 0x00000021u, 0x00000001u,
    0x00070050u, 0x00000007u, 0x00000025u, 0x00000023u, 0x00000024u, 0x00000013u, 0x00000022u, 0x00050041u,
    0x00000026u, 0x00000027u, 0x0000000du, 0x0000000fu, 0x0003003eu, 0x00000027u, 0x00000025u, 0x000100fdu,
    0x00010038u,
};
#endif

#ifdef LODOS_TRIANGLE_FRAG_SPV
#include LODOS_TRIANGLE_FRAG_SPV
#else
static const uint32_t k_frag_spv[] = {
    0x07230203u, 0x00010000u, 0x0008000bu, 0x0000000du, 0x00000000u, 0x00020011u, 0x00000001u, 0x0006000bu,
    0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu, 0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u,
    0x0006000fu, 0x00000004u, 0x00000004u, 0x6e69616du, 0x00000000u, 0x00000009u, 0x00030010u, 0x00000004u,
    0x00000007u, 0x00030003u, 0x00000002u, 0x000001c2u, 0x00040005u, 0x00000004u, 0x6e69616du, 0x00000000u,
    0x00050005u, 0x00000009u, 0x4374756fu, 0x726f6c6fu, 0x00000000u, 0x00040047u, 0x00000009u, 0x0000001eu,
    0x00000000u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u, 0x00000002u, 0x00030016u, 0x00000006u,
    0x00000020u, 0x00040017u, 0x00000007u, 0x00000006u, 0x00000004u, 0x00040020u, 0x00000008u, 0x00000003u,
    0x00000007u, 0x0004003bu, 0x00000008u, 0x00000009u, 0x00000003u, 0x0004002bu, 0x00000006u, 0x0000000au,
    0x3f800000u, 0x0004002bu, 0x00000006u, 0x0000000bu, 0x00000000u, 0x0007002cu, 0x00000007u, 0x0000000cu,
    0x0000000au, 0x0000000bu, 0x0000000bu, 0x0000000au, 0x00050036u, 0x00000002u, 0x00000004u, 0x00000000u,
    0x00000003u, 0x000200f8u, 0x00000005u, 0x0003003eu, 0x00000009u, 0x0000000cu, 0x000100fdu, 0x00010038u,
};
#endif

static uint32_t find_mem_type(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags req) {
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & req) == req) return i;
    return UINT32_MAX;
}

static void check_gdpa_coverage(VkDevice dev) {
    static const char *const k_req[] = {
        "vkCreateImage", "vkDestroyImage", "vkBindImageMemory",
        "vkGetImageMemoryRequirements", "vkGetImageMemoryRequirements2",
        "vkCreateBuffer", "vkDestroyBuffer", "vkBindBufferMemory",
        "vkGetBufferMemoryRequirements", "vkGetBufferMemoryRequirements2",
        "vkCreateImageView", "vkDestroyImageView",
        "vkCreateRenderPass", "vkCreateRenderPass2", "vkDestroyRenderPass",
        "vkCreateFramebuffer", "vkDestroyFramebuffer",
        "vkCreatePipelineLayout", "vkDestroyPipelineLayout",
        "vkCreateGraphicsPipelines", "vkDestroyPipeline",
        "vkCmdBeginRenderPass", "vkCmdEndRenderPass",
        "vkCmdBeginRenderPass2", "vkCmdEndRenderPass2",
        "vkCmdBindPipeline", "vkCmdDraw",
        "vkCmdSetViewport", "vkCmdSetScissor",
        "vkCmdPipelineBarrier", "vkCmdCopyImageToBuffer",
    };
    for (size_t i = 0; i < sizeof(k_req) / sizeof(k_req[0]); i++) {
        if (!vkGetDeviceProcAddr(dev, k_req[i]))
            DIE("vkGetDeviceProcAddr(%s) returned NULL", k_req[i]);
    }
    printf("VKB gdpa_graphics_check ok count=%zu\n", sizeof(k_req) / sizeof(k_req[0]));
}

static void write_ppm(const char *path, const uint8_t *rgba, uint32_t w, uint32_t h) {
    FILE *f = fopen(path, "wb");
    if (!f) DIE("fopen(%s) failed", path);
    fprintf(f, "P6\n%u %u\n255\n", w, h);
    uint8_t row[W * 3];
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *src = rgba + (size_t)y * w * 4u;
        for (uint32_t x = 0; x < w; x++) {
            row[x * 3u + 0u] = src[x * 4u + 0u];
            row[x * 3u + 1u] = src[x * 4u + 1u];
            row[x * 3u + 2u] = src[x * 4u + 2u];
        }
        if (fwrite(row, 1, (size_t)w * 3u, f) != (size_t)w * 3u) DIE("fwrite(%s) failed", path);
    }
    if (fclose(f) != 0) DIE("fclose(%s) failed", path);
}

int main(int argc, char **argv) {
    const char *ppm_path = argc > 1 ? argv[1] : "build/vk-bridge/host-triangle/out.ppm";

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "lodos-host-triangle",
        .applicationVersion = 1,
        .pEngineName = "lodos",
        .engineVersion = 1,
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance inst = VK_NULL_HANDLE;
    VK(vkCreateInstance(&ici, NULL, &inst));

    uint32_t npd = 0;
    VK(vkEnumeratePhysicalDevices(inst, &npd, NULL));
    if (!npd) DIE("no physical devices exposed via ICD");
    VkPhysicalDevice pds[8];
    if (npd > 8) npd = 8;
    VK(vkEnumeratePhysicalDevices(inst, &npd, pds));
    VkPhysicalDevice pd = pds[0];

    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    VkPhysicalDeviceMemoryProperties mprops;
    vkGetPhysicalDeviceMemoryProperties(pd, &mprops);
    printf("VKB device=%s vendor=0x%x api=%u.%u.%u\n",
           props.deviceName, props.vendorID,
           VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion),
           VK_API_VERSION_PATCH(props.apiVersion));

    VkPhysicalDeviceDynamicRenderingFeatures dyn_feat = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
    };
    VkPhysicalDeviceFeatures2 feat2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &dyn_feat,
    };
    if (props.apiVersion >= VK_API_VERSION_1_2)
        vkGetPhysicalDeviceFeatures2(pd, &feat2);

    uint32_t nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, NULL);
    VkQueueFamilyProperties qfs[16];
    if (nqf > 16) nqf = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qfs);
    uint32_t qfi = UINT32_MAX;
    for (uint32_t i = 0; i < nqf; i++) {
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfi = i; break; }
    }
    if (qfi == UINT32_MAX) DIE("no graphics queue family");

    float qprio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qfi,
        .queueCount = 1,
        .pQueuePriorities = &qprio,
    };
    VkPhysicalDeviceDynamicRenderingFeatures dyn_en = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES,
        .dynamicRendering = dyn_feat.dynamicRendering,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext = dyn_feat.dynamicRendering ? &dyn_en : NULL,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
    };
    VkDevice dev = VK_NULL_HANDLE;
    VK(vkCreateDevice(pd, &dci, NULL, &dev));
    check_gdpa_coverage(dev);

    #define GDPA(fn) PFN_##fn p_##fn = (PFN_##fn)vkGetDeviceProcAddr(dev, #fn); if (!p_##fn) DIE("missing " #fn)
    GDPA(vkGetDeviceQueue);
    GDPA(vkQueueSubmit);
    GDPA(vkQueueWaitIdle);
    GDPA(vkDeviceWaitIdle);
    GDPA(vkCreateFence);
    GDPA(vkDestroyFence);
    GDPA(vkWaitForFences);
    GDPA(vkAllocateMemory);
    GDPA(vkFreeMemory);
    GDPA(vkMapMemory);
    GDPA(vkUnmapMemory);
    GDPA(vkFlushMappedMemoryRanges);
    GDPA(vkInvalidateMappedMemoryRanges);
    GDPA(vkCreateImage);
    GDPA(vkDestroyImage);
    GDPA(vkGetImageMemoryRequirements);
    GDPA(vkGetImageMemoryRequirements2);
    GDPA(vkBindImageMemory);
    GDPA(vkCreateImageView);
    GDPA(vkDestroyImageView);
    GDPA(vkCreateBuffer);
    GDPA(vkDestroyBuffer);
    GDPA(vkGetBufferMemoryRequirements);
    GDPA(vkGetBufferMemoryRequirements2);
    GDPA(vkBindBufferMemory);
    GDPA(vkCreateShaderModule);
    GDPA(vkDestroyShaderModule);
    GDPA(vkCreateRenderPass);
    GDPA(vkCreateRenderPass2);
    GDPA(vkDestroyRenderPass);
    GDPA(vkCreateFramebuffer);
    GDPA(vkDestroyFramebuffer);
    GDPA(vkCreatePipelineLayout);
    GDPA(vkDestroyPipelineLayout);
    GDPA(vkCreateGraphicsPipelines);
    GDPA(vkDestroyPipeline);
    GDPA(vkCreateCommandPool);
    GDPA(vkDestroyCommandPool);
    GDPA(vkAllocateCommandBuffers);
    GDPA(vkFreeCommandBuffers);
    GDPA(vkBeginCommandBuffer);
    GDPA(vkEndCommandBuffer);
    GDPA(vkCmdBeginRenderPass);
    GDPA(vkCmdEndRenderPass);
    GDPA(vkCmdBindPipeline);
    GDPA(vkCmdSetViewport);
    GDPA(vkCmdSetScissor);
    GDPA(vkCmdDraw);
    GDPA(vkCmdPipelineBarrier);
    GDPA(vkCmdCopyImageToBuffer);

    VkQueue queue = VK_NULL_HANDLE;
    p_vkGetDeviceQueue(dev, qfi, 0, &queue);
    if (!queue) DIE("null queue");

    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo img_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = fmt,
        .extent = { W, H, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage img = VK_NULL_HANDLE;
    VK(p_vkCreateImage(dev, &img_ci, NULL, &img));

    VkMemoryRequirements img_mr;
    p_vkGetImageMemoryRequirements(dev, img, &img_mr);
    VkImageMemoryRequirementsInfo2 imri2 = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_REQUIREMENTS_INFO_2,
        .image = img,
    };
    VkMemoryRequirements2 img_mr2 = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    p_vkGetImageMemoryRequirements2(dev, &imri2, &img_mr2);
    if (img_mr2.memoryRequirements.size != img_mr.size)
        DIE("GetImageMemoryRequirements2 mismatch (%llu vs %llu)",
            (unsigned long long)img_mr2.memoryRequirements.size, (unsigned long long)img_mr.size);

    uint32_t mt_img = find_mem_type(&mprops, img_mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt_img == UINT32_MAX) mt_img = find_mem_type(&mprops, img_mr.memoryTypeBits, 0);
    if (mt_img == UINT32_MAX) DIE("no suitable image memory type");

    VkMemoryAllocateInfo mai_img = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = img_mr.size,
        .memoryTypeIndex = mt_img,
    };
    VkDeviceMemory mem_img = VK_NULL_HANDLE;
    VK(p_vkAllocateMemory(dev, &mai_img, NULL, &mem_img));
    VK(p_vkBindImageMemory(dev, img, mem_img, 0));

    VkImageViewCreateInfo iv_ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = fmt,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    VkImageView iv = VK_NULL_HANDLE;
    VK(p_vkCreateImageView(dev, &iv_ci, NULL, &iv));

    const VkDeviceSize buf_sz = (VkDeviceSize)W * H * 4u;
    VkBufferCreateInfo buf_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = buf_sz,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buf = VK_NULL_HANDLE;
    VK(p_vkCreateBuffer(dev, &buf_ci, NULL, &buf));

    VkMemoryRequirements buf_mr;
    p_vkGetBufferMemoryRequirements(dev, buf, &buf_mr);
    VkBufferMemoryRequirementsInfo2 bmri2 = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2,
        .buffer = buf,
    };
    VkMemoryRequirements2 buf_mr2 = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    p_vkGetBufferMemoryRequirements2(dev, &bmri2, &buf_mr2);
    if (buf_mr2.memoryRequirements.size != buf_mr.size)
        DIE("GetBufferMemoryRequirements2 mismatch");

    uint32_t mt_buf = find_mem_type(&mprops, buf_mr.memoryTypeBits,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (mt_buf == UINT32_MAX)
        mt_buf = find_mem_type(&mprops, buf_mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    if (mt_buf == UINT32_MAX) DIE("no host-visible memory type");

    VkMemoryAllocateInfo mai_buf = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = buf_mr.size,
        .memoryTypeIndex = mt_buf,
    };
    VkDeviceMemory mem_buf = VK_NULL_HANDLE;
    VK(p_vkAllocateMemory(dev, &mai_buf, NULL, &mem_buf));
    VK(p_vkBindBufferMemory(dev, buf, mem_buf, 0));

    /* Pre-fill readback buffer with non-black/non-red sentinel to prove GPU write + invalidate */
    {
        uint8_t *pre = NULL;
        VK(p_vkMapMemory(dev, mem_buf, 0, buf_sz, 0, (void **)&pre));
        memset(pre, 0x5a, (size_t)buf_sz);
        VkMappedMemoryRange mmr_pre = {
            .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
            .memory = mem_buf,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        VK(p_vkFlushMappedMemoryRanges(dev, 1, &mmr_pre));
        p_vkUnmapMemory(dev, mem_buf);
    }

    VkAttachmentDescription att = {
        .format = fmt,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference color_ref = {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkSubpassDescription subpass = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_ref,
    };
    VkSubpassDependency dep = {
        .srcSubpass = VK_SUBPASS_EXTERNAL,
        .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
    };
    VkRenderPassCreateInfo rp_ci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &att,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dep,
    };
    VkRenderPass rp = VK_NULL_HANDLE;
    VK(p_vkCreateRenderPass(dev, &rp_ci, NULL, &rp));

    /* Also exercise vkCreateRenderPass2 nested struct arrays across the bridge */
    {
        VkAttachmentDescription2 att2 = {
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
            .format = fmt,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };
        VkAttachmentReference2 ref2 = {
            .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
            .attachment = 0,
            .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        };
        VkSubpassDescription2 sub2 = {
            .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = 1,
            .pColorAttachments = &ref2,
        };
        VkRenderPassCreateInfo2 rp2_ci = {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,
            .attachmentCount = 1,
            .pAttachments = &att2,
            .subpassCount = 1,
            .pSubpasses = &sub2,
        };
        VkRenderPass rp2 = VK_NULL_HANDLE;
        VK(p_vkCreateRenderPass2(dev, &rp2_ci, NULL, &rp2));
        p_vkDestroyRenderPass(dev, rp2, NULL);
    }

    VkFramebufferCreateInfo fb_ci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = rp,
        .attachmentCount = 1,
        .pAttachments = &iv,
        .width = W,
        .height = H,
        .layers = 1,
    };
    VkFramebuffer fb = VK_NULL_HANDLE;
    VK(p_vkCreateFramebuffer(dev, &fb_ci, NULL, &fb));

    VkShaderModuleCreateInfo vs_ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(k_vert_spv),
        .pCode = k_vert_spv,
    };
    VkShaderModuleCreateInfo fs_ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(k_frag_spv),
        .pCode = k_frag_spv,
    };
    VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
    VK(p_vkCreateShaderModule(dev, &vs_ci, NULL, &vs));
    VK(p_vkCreateShaderModule(dev, &fs_ci, NULL, &fs));

    VkPipelineLayoutCreateInfo pl_ci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
    };
    VkPipelineLayout playout = VK_NULL_HANDLE;
    VK(p_vkCreatePipelineLayout(dev, &pl_ci, NULL, &playout));

    VkPipelineShaderStageCreateInfo stages[2] = {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vs,
            .pName = "main",
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fs,
            .pName = "main",
        },
    };
    VkPipelineVertexInputStateCreateInfo vi_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo ia_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo vp_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo rs_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkSampleMask sample_mask = 0x1u;
    VkPipelineMultisampleStateCreateInfo ms_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .pSampleMask = &sample_mask,
    };
    VkPipelineColorBlendAttachmentState cba = {
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo cb_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &cba,
    };
    VkDynamicState dyn_states[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dyn_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2,
        .pDynamicStates = dyn_states,
    };
    VkGraphicsPipelineCreateInfo gp_ci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2,
        .pStages = stages,
        .pVertexInputState = &vi_state,
        .pInputAssemblyState = &ia_state,
        .pViewportState = &vp_state,
        .pRasterizationState = &rs_state,
        .pMultisampleState = &ms_state,
        .pColorBlendState = &cb_state,
        .pDynamicState = &dyn_state,
        .layout = playout,
        .renderPass = rp,
        .subpass = 0,
    };
    VkPipeline pipe = VK_NULL_HANDLE;
    VK(p_vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp_ci, NULL, &pipe));

    /* Exercise pNext chain on VkGraphicsPipelineCreateInfo (VkPipelineRenderingCreateInfo) when supported */
    if (dyn_feat.dynamicRendering) {
        VkPipelineRenderingCreateInfo pr_ci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
            .colorAttachmentCount = 1,
            .pColorAttachmentFormats = &fmt,
        };
        VkGraphicsPipelineCreateInfo gp_dyn = gp_ci;
        gp_dyn.pNext = &pr_ci;
        gp_dyn.renderPass = VK_NULL_HANDLE;
        VkPipeline pipe_dyn = VK_NULL_HANDLE;
        VK(p_vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gp_dyn, NULL, &pipe_dyn));
        p_vkDestroyPipeline(dev, pipe_dyn, NULL);
    }

    VkCommandPoolCreateInfo cp_ci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = qfi,
    };
    VkCommandPool cpool = VK_NULL_HANDLE;
    VK(p_vkCreateCommandPool(dev, &cp_ci, NULL, &cpool));

    VkCommandBufferAllocateInfo cb_ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cpool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VK(p_vkAllocateCommandBuffers(dev, &cb_ai, &cb));

    VkCommandBufferBeginInfo cb_bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK(p_vkBeginCommandBuffer(cb, &cb_bi));

    VkClearValue clear_val = { .color = { .float32 = { 0.0f, 0.0f, 0.0f, 1.0f } } };
    VkRenderPassBeginInfo rp_bi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = rp,
        .framebuffer = fb,
        .renderArea = { .offset = { 0, 0 }, .extent = { W, H } },
        .clearValueCount = 1,
        .pClearValues = &clear_val,
    };
    p_vkCmdBeginRenderPass(cb, &rp_bi, VK_SUBPASS_CONTENTS_INLINE);
    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);

    VkViewport vp = { .x = 0.0f, .y = 0.0f, .width = (float)W, .height = (float)H, .minDepth = 0.0f, .maxDepth = 1.0f };
    VkRect2D sc = { .offset = { 0, 0 }, .extent = { W, H } };
    p_vkCmdSetViewport(cb, 0, 1, &vp);
    p_vkCmdSetScissor(cb, 0, 1, &sc);
    p_vkCmdDraw(cb, 3, 1, 0, 0);
    p_vkCmdEndRenderPass(cb);

    VkImageMemoryBarrier imb = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, NULL, 0, NULL, 1, &imb);

    VkBufferImageCopy bic = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .mipLevel = 0,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { W, H, 1 },
    };
    p_vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &bic);

    VkBufferMemoryBarrier bmb = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buf,
        .offset = 0,
        .size = buf_sz,
    };
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                           0, 0, NULL, 1, &bmb, 0, NULL);

    VK(p_vkEndCommandBuffer(cb));

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    VK(p_vkCreateFence(dev, &fci, NULL, &fence));

    VkSubmitInfo sub = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
    };
    VK(p_vkQueueSubmit(queue, 1, &sub, fence));
    VK(p_vkWaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ull));

    const uint8_t *px = NULL;
    VK(p_vkMapMemory(dev, mem_buf, 0, buf_sz, 0, (void **)&px));
    VkMappedMemoryRange mmr = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = mem_buf,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    VK(p_vkInvalidateMappedMemoryRanges(dev, 1, &mmr));

    const uint8_t *center = px + ((size_t)(H / 2u) * W + (W / 2u)) * 4u;
    const uint8_t *c00 = px;
    const uint8_t *c10 = px + (size_t)(W - 1u) * 4u;
    const uint8_t *c01 = px + (size_t)(H - 1u) * W * 4u;
    const uint8_t *c11 = px + ((size_t)(H - 1u) * W + (W - 1u)) * 4u;

    if (center[0] < 250 || center[1] > 5 || center[2] > 5 || center[3] < 250)
        DIE("center pixel not red: (%u,%u,%u,%u)", center[0], center[1], center[2], center[3]);
    const uint8_t *corners[4] = { c00, c10, c01, c11 };
    for (int i = 0; i < 4; i++) {
        const uint8_t * cp = corners[i];
        if (cp[0] > 5 || cp[1] > 5 || cp[2] > 5 || cp[3] < 250)
            DIE("corner[%d] pixel not black: (%u,%u,%u,%u)", i, cp[0], cp[1], cp[2], cp[3]);
    }

    write_ppm(ppm_path, px, W, H);
    uint8_t cr[4] = { center[0], center[1], center[2], center[3] };
    uint8_t kr[4] = { c00[0], c00[1], c00[2], c00[3] };

    p_vkUnmapMemory(dev, mem_buf);
    VK(p_vkQueueWaitIdle(queue));
    VK(p_vkDeviceWaitIdle(dev));

    p_vkDestroyFence(dev, fence, NULL);
    p_vkFreeCommandBuffers(dev, cpool, 1, &cb);
    p_vkDestroyCommandPool(dev, cpool, NULL);
    p_vkDestroyPipeline(dev, pipe, NULL);
    p_vkDestroyPipelineLayout(dev, playout, NULL);
    p_vkDestroyShaderModule(dev, fs, NULL);
    p_vkDestroyShaderModule(dev, vs, NULL);
    p_vkDestroyFramebuffer(dev, fb, NULL);
    p_vkDestroyRenderPass(dev, rp, NULL);
    p_vkDestroyBuffer(dev, buf, NULL);
    p_vkFreeMemory(dev, mem_buf, NULL);
    p_vkDestroyImageView(dev, iv, NULL);
    p_vkDestroyImage(dev, img, NULL);
    p_vkFreeMemory(dev, mem_img, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);

    printf("VKB client triangle verified center=0x%02x%02x%02x%02x corner=0x%02x%02x%02x%02x ppm=%s\n",
           cr[0], cr[1], cr[2], cr[3], kr[0], kr[1], kr[2], kr[3], ppm_path);
    return 0;
}
