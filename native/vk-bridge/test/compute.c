/* T029 PC-host e2e compute client via Khronos Vulkan Loader -> libvulkan_lodos.so -> vk-bridge -> host ICD.
 * Verifies GDPA coverage for compute entrypoints and dispatches a compute shader doubling 1M uint32s
 * across host->device CopyBuffer + PipelineBarrier + Compute Dispatch + device->host CopyBuffer + Fence. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

#define N_ELEMS (1u << 20) /* 1,048,576 uint32_t = 4 MiB */
#define WG_SIZE 256u

#define DIE(...) do { fprintf(stderr, "HOST_COMPUTE FAIL: " __VA_ARGS__); fputc('\n', stderr); exit(1); } while (0)
#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) DIE("%s -> %d", #x, (int)r_); } while (0)

/* Precompiled SPIR-V (Vulkan 1.0, glslc -O):
 *   #version 450
 *   layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;
 *   layout(set = 0, binding = 0) buffer Data { uint v[]; } data;
 *   void main() { uint idx = gl_GlobalInvocationID.x; data.v[idx] = data.v[idx] * 2u; } */
static const uint32_t k_double_spv[] = {
    0x07230203u, 0x00010000u, 0x000d000bu, 0x00000021u, 0x00000000u, 0x00020011u,
    0x00000001u, 0x0006000bu, 0x00000001u, 0x4c534c47u, 0x6474732eu, 0x3035342eu,
    0x00000000u, 0x0003000eu, 0x00000000u, 0x00000001u, 0x0006000fu, 0x00000005u,
    0x00000004u, 0x6e69616du, 0x00000000u, 0x0000000bu, 0x00060010u, 0x00000004u,
    0x00000011u, 0x00000100u, 0x00000001u, 0x00000001u, 0x00040047u, 0x0000000bu,
    0x0000000bu, 0x0000001cu, 0x00040047u, 0x00000010u, 0x00000006u, 0x00000004u,
    0x00030047u, 0x00000011u, 0x00000003u, 0x00050048u, 0x00000011u, 0x00000000u,
    0x00000023u, 0x00000000u, 0x00040047u, 0x00000013u, 0x00000021u, 0x00000000u,
    0x00040047u, 0x00000013u, 0x00000022u, 0x00000000u, 0x00040047u, 0x00000020u,
    0x0000000bu, 0x00000019u, 0x00020013u, 0x00000002u, 0x00030021u, 0x00000003u,
    0x00000002u, 0x00040015u, 0x00000006u, 0x00000020u, 0x00000000u, 0x00040017u,
    0x00000009u, 0x00000006u, 0x00000003u, 0x00040020u, 0x0000000au, 0x00000001u,
    0x00000009u, 0x0004003bu, 0x0000000au, 0x0000000bu, 0x00000001u, 0x0004002bu,
    0x00000006u, 0x0000000cu, 0x00000000u, 0x00040020u, 0x0000000du, 0x00000001u,
    0x00000006u, 0x0003001du, 0x00000010u, 0x00000006u, 0x0003001eu, 0x00000011u,
    0x00000010u, 0x00040020u, 0x00000012u, 0x00000002u, 0x00000011u, 0x0004003bu,
    0x00000012u, 0x00000013u, 0x00000002u, 0x00040015u, 0x00000014u, 0x00000020u,
    0x00000001u, 0x0004002bu, 0x00000014u, 0x00000015u, 0x00000000u, 0x00040020u,
    0x00000018u, 0x00000002u, 0x00000006u, 0x0004002bu, 0x00000006u, 0x0000001bu,
    0x00000002u, 0x0004002bu, 0x00000006u, 0x0000001eu, 0x00000100u, 0x0004002bu,
    0x00000006u, 0x0000001fu, 0x00000001u, 0x0006002cu, 0x00000009u, 0x00000020u,
    0x0000001eu, 0x0000001fu, 0x0000001fu, 0x00050036u, 0x00000002u, 0x00000004u,
    0x00000000u, 0x00000003u, 0x000200f8u, 0x00000005u, 0x00050041u, 0x0000000du,
    0x0000000eu, 0x0000000bu, 0x0000000cu, 0x0004003du, 0x00000006u, 0x0000000fu,
    0x0000000eu, 0x00060041u, 0x00000018u, 0x00000019u, 0x00000013u, 0x00000015u,
    0x0000000fu, 0x0004003du, 0x00000006u, 0x0000001au, 0x00000019u, 0x00050084u,
    0x00000006u, 0x0000001cu, 0x0000001au, 0x0000001bu, 0x0003003eu, 0x00000019u,
    0x0000001cu, 0x000100fdu, 0x00010038u,
};

static uint32_t find_mem_type(const VkPhysicalDeviceMemoryProperties *mp, uint32_t bits, VkMemoryPropertyFlags req) {
    for (uint32_t i = 0; i < mp->memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp->memoryTypes[i].propertyFlags & req) == req) return i;
    return UINT32_MAX;
}

static void check_gdpa_coverage(VkDevice dev) {
    static const char *const k_req[] = {
        "vkGetDeviceQueue", "vkQueueSubmit", "vkQueueWaitIdle", "vkDeviceWaitIdle",
        "vkCreateFence", "vkDestroyFence", "vkResetFences", "vkGetFenceStatus", "vkWaitForFences",
        "vkCreateSemaphore", "vkDestroySemaphore",
        "vkAllocateMemory", "vkFreeMemory", "vkMapMemory", "vkUnmapMemory",
        "vkFlushMappedMemoryRanges", "vkInvalidateMappedMemoryRanges",
        "vkCreateBuffer", "vkDestroyBuffer", "vkGetBufferMemoryRequirements", "vkBindBufferMemory",
        "vkCreateShaderModule", "vkDestroyShaderModule",
        "vkCreateDescriptorSetLayout", "vkDestroyDescriptorSetLayout",
        "vkCreateDescriptorPool", "vkDestroyDescriptorPool", "vkResetDescriptorPool",
        "vkAllocateDescriptorSets", "vkFreeDescriptorSets", "vkUpdateDescriptorSets",
        "vkCreatePipelineLayout", "vkDestroyPipelineLayout",
        "vkCreateComputePipelines", "vkDestroyPipeline",
        "vkCreateCommandPool", "vkDestroyCommandPool", "vkResetCommandPool",
        "vkAllocateCommandBuffers", "vkFreeCommandBuffers",
        "vkBeginCommandBuffer", "vkEndCommandBuffer", "vkResetCommandBuffer",
        "vkCmdBindPipeline", "vkCmdBindDescriptorSets", "vkCmdPushConstants",
        "vkCmdDispatch", "vkCmdPipelineBarrier", "vkCmdCopyBuffer",
    };
    for (size_t i = 0; i < sizeof(k_req) / sizeof(k_req[0]); i++) {
        if (!vkGetDeviceProcAddr(dev, k_req[i]))
            DIE("vkGetDeviceProcAddr(%s) returned NULL", k_req[i]);
    }
    printf("VKB gdpa_check ok count=%zu\n", sizeof(k_req) / sizeof(k_req[0]));
}

int main(void) {
    struct timespec started, finished;
    clock_gettime(CLOCK_MONOTONIC, &started);
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "lodos-host-compute",
        .applicationVersion = 1,
        .pEngineName = "lodos",
        .engineVersion = 1,
        .apiVersion = VK_API_VERSION_1_1,
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

    uint32_t nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, NULL);
    VkQueueFamilyProperties qfs[16];
    if (nqf > 16) nqf = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qfs);
    uint32_t qfi = UINT32_MAX;
    for (uint32_t i = 0; i < nqf; i++) {
        if (qfs[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { qfi = i; break; }
    }
    if (qfi == UINT32_MAX) DIE("no compute queue family");

    float qprio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qfi,
        .queueCount = 1,
        .pQueuePriorities = &qprio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
    };
    VkDevice dev = VK_NULL_HANDLE;
    VK(vkCreateDevice(pd, &dci, NULL, &dev));
    check_gdpa_coverage(dev);

    /* Resolve key dispatchable/device functions through GDPA explicitly */
    #define GDPA(fn) PFN_##fn p_##fn = (PFN_##fn)vkGetDeviceProcAddr(dev, #fn); if (!p_##fn) DIE("missing " #fn)
    GDPA(vkGetDeviceQueue);
    GDPA(vkQueueSubmit);
    GDPA(vkQueueWaitIdle);
    GDPA(vkDeviceWaitIdle);
    GDPA(vkCreateFence);
    GDPA(vkDestroyFence);
    GDPA(vkResetFences);
    GDPA(vkWaitForFences);
    GDPA(vkCreateSemaphore);
    GDPA(vkDestroySemaphore);
    GDPA(vkCreateBuffer);
    GDPA(vkDestroyBuffer);
    GDPA(vkGetBufferMemoryRequirements);
    GDPA(vkAllocateMemory);
    GDPA(vkFreeMemory);
    GDPA(vkBindBufferMemory);
    GDPA(vkMapMemory);
    GDPA(vkUnmapMemory);
    GDPA(vkFlushMappedMemoryRanges);
    GDPA(vkInvalidateMappedMemoryRanges);
    GDPA(vkCreateShaderModule);
    GDPA(vkDestroyShaderModule);
    GDPA(vkCreateDescriptorSetLayout);
    GDPA(vkDestroyDescriptorSetLayout);
    GDPA(vkCreateDescriptorPool);
    GDPA(vkDestroyDescriptorPool);
    GDPA(vkAllocateDescriptorSets);
    GDPA(vkFreeDescriptorSets);
    GDPA(vkUpdateDescriptorSets);
    GDPA(vkCreatePipelineLayout);
    GDPA(vkDestroyPipelineLayout);
    GDPA(vkCreateComputePipelines);
    GDPA(vkDestroyPipeline);
    GDPA(vkCreateCommandPool);
    GDPA(vkDestroyCommandPool);
    GDPA(vkAllocateCommandBuffers);
    GDPA(vkFreeCommandBuffers);
    GDPA(vkBeginCommandBuffer);
    GDPA(vkEndCommandBuffer);
    GDPA(vkCmdCopyBuffer);
    GDPA(vkCmdPipelineBarrier);
    GDPA(vkCmdBindPipeline);
    GDPA(vkCmdBindDescriptorSets);
    GDPA(vkCmdDispatch);

    VkQueue queue = VK_NULL_HANDLE;
    p_vkGetDeviceQueue(dev, qfi, 0, &queue);
    if (!queue) DIE("null queue");

    const VkDeviceSize byte_sz = (VkDeviceSize)N_ELEMS * sizeof(uint32_t);
    VkBufferCreateInfo bci_io = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = byte_sz,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBufferCreateInfo bci_gpu = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = byte_sz,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buf_io = VK_NULL_HANDLE, buf_gpu = VK_NULL_HANDLE;
    VK(p_vkCreateBuffer(dev, &bci_io, NULL, &buf_io));
    VK(p_vkCreateBuffer(dev, &bci_gpu, NULL, &buf_gpu));

    VkMemoryRequirements mr_io, mr_gpu;
    p_vkGetBufferMemoryRequirements(dev, buf_io, &mr_io);
    p_vkGetBufferMemoryRequirements(dev, buf_gpu, &mr_gpu);

    uint32_t mt_io = find_mem_type(&mprops, mr_io.memoryTypeBits,
                                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt_io == UINT32_MAX)
        mt_io = find_mem_type(&mprops, mr_io.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    uint32_t mt_gpu = find_mem_type(&mprops, mr_gpu.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt_gpu == UINT32_MAX) mt_gpu = mt_io;
    if (mt_io == UINT32_MAX || mt_gpu == UINT32_MAX) DIE("no suitable memory type");

    VkMemoryAllocateInfo mai_io = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr_io.size, .memoryTypeIndex = mt_io };
    VkMemoryAllocateInfo mai_gpu = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = mr_gpu.size, .memoryTypeIndex = mt_gpu };
    VkDeviceMemory mem_io = VK_NULL_HANDLE, mem_gpu = VK_NULL_HANDLE;
    VK(p_vkAllocateMemory(dev, &mai_io, NULL, &mem_io));
    VK(p_vkAllocateMemory(dev, &mai_gpu, NULL, &mem_gpu));
    VK(p_vkBindBufferMemory(dev, buf_io, mem_io, 0));
    VK(p_vkBindBufferMemory(dev, buf_gpu, mem_gpu, 0));

    uint32_t *mapped = NULL;
    VK(p_vkMapMemory(dev, mem_io, 0, VK_WHOLE_SIZE, 0, (void **)&mapped));
    for (uint32_t i = 0; i < N_ELEMS; i++) mapped[i] = i * 3u + 7u;
    VkMappedMemoryRange mmr = {
        .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
        .memory = mem_io,
        .offset = 0,
        .size = VK_WHOLE_SIZE,
    };
    VK(p_vkFlushMappedMemoryRanges(dev, 1, &mmr));

    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(k_double_spv),
        .pCode = k_double_spv,
    };
    VkShaderModule sh = VK_NULL_HANDLE;
    VK(p_vkCreateShaderModule(dev, &smci, NULL, &sh));

    VkDescriptorSetLayoutBinding dslb = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &dslb,
    };
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VK(p_vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl));

    VkDescriptorPoolSize dps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1 };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &dps,
    };
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VK(p_vkCreateDescriptorPool(dev, &dpci, NULL, &dpool));

    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dpool,
        .descriptorSetCount = 1,
        .pSetLayouts = &dsl,
    };
    VkDescriptorSet dset = VK_NULL_HANDLE;
    VK(p_vkAllocateDescriptorSets(dev, &dsai, &dset));

    VkDescriptorBufferInfo dbi = { .buffer = buf_gpu, .offset = 0, .range = byte_sz };
    VkWriteDescriptorSet wds = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = dset,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &dbi,
    };
    p_vkUpdateDescriptorSets(dev, 1, &wds, 0, NULL);

    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &dsl,
    };
    VkPipelineLayout playout = VK_NULL_HANDLE;
    VK(p_vkCreatePipelineLayout(dev, &plci, NULL, &playout));

    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = sh,
            .pName = "main",
        },
        .layout = playout,
    };
    VkPipeline pipe = VK_NULL_HANDLE;
    VK(p_vkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe));

    VkCommandPoolCreateInfo cp_ci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = qfi,
    };
    VkCommandPool cpool = VK_NULL_HANDLE;
    VK(p_vkCreateCommandPool(dev, &cp_ci, NULL, &cpool));

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cpool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VK(p_vkAllocateCommandBuffers(dev, &cbai, &cb));

    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    VK(p_vkBeginCommandBuffer(cb, &cbbi));

    VkBufferCopy copy_reg = { .srcOffset = 0, .dstOffset = 0, .size = byte_sz };
    p_vkCmdCopyBuffer(cb, buf_io, buf_gpu, 1, &copy_reg);

    VkBufferMemoryBarrier bmb_to_comp = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buf_gpu,
        .offset = 0,
        .size = byte_sz,
    };
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           0, 0, NULL, 1, &bmb_to_comp, 0, NULL);

    p_vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    p_vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, playout, 0, 1, &dset, 0, NULL);
    p_vkCmdDispatch(cb, N_ELEMS / WG_SIZE, 1, 1);

    VkBufferMemoryBarrier bmb_to_xfer = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buf_gpu,
        .offset = 0,
        .size = byte_sz,
    };
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, NULL, 1, &bmb_to_xfer, 0, NULL);

    p_vkCmdCopyBuffer(cb, buf_gpu, buf_io, 1, &copy_reg);

    VkBufferMemoryBarrier bmb_to_host = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buf_io,
        .offset = 0,
        .size = byte_sz,
    };
    p_vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT,
                           0, 0, NULL, 1, &bmb_to_host, 0, NULL);

    VK(p_vkEndCommandBuffer(cb));

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence = VK_NULL_HANDLE;
    VK(p_vkCreateFence(dev, &fci, NULL, &fence));
    VK(p_vkResetFences(dev, 1, &fence));

    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkSemaphore sem = VK_NULL_HANDLE;
    VK(p_vkCreateSemaphore(dev, &sci, NULL, &sem));

    VkSubmitInfo sub = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cb,
    };
    VK(p_vkQueueSubmit(queue, 1, &sub, fence));
    VK(p_vkWaitForFences(dev, 1, &fence, VK_TRUE, 10000000000ull));
    VK(p_vkInvalidateMappedMemoryRanges(dev, 1, &mmr));

    for (uint32_t i = 0; i < N_ELEMS; i++) {
        uint32_t exp = (i * 3u + 7u) * 2u;
        if (mapped[i] != exp)
            DIE("mismatch at i=%u: got %u expected %u", i, mapped[i], exp);
    }

    p_vkUnmapMemory(dev, mem_io);
    VK(p_vkQueueWaitIdle(queue));
    VK(p_vkDeviceWaitIdle(dev));

    p_vkDestroySemaphore(dev, sem, NULL);
    p_vkDestroyFence(dev, fence, NULL);
    p_vkFreeCommandBuffers(dev, cpool, 1, &cb);
    p_vkDestroyCommandPool(dev, cpool, NULL);
    p_vkDestroyPipeline(dev, pipe, NULL);
    p_vkDestroyPipelineLayout(dev, playout, NULL);
    VK(p_vkFreeDescriptorSets(dev, dpool, 1, &dset));
    p_vkDestroyDescriptorPool(dev, dpool, NULL);
    p_vkDestroyDescriptorSetLayout(dev, dsl, NULL);
    p_vkDestroyShaderModule(dev, sh, NULL);
    p_vkDestroyBuffer(dev, buf_gpu, NULL);
    p_vkDestroyBuffer(dev, buf_io, NULL);
    p_vkFreeMemory(dev, mem_gpu, NULL);
    p_vkFreeMemory(dev, mem_io, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);

    clock_gettime(CLOCK_MONOTONIC, &finished);
    double ms = (finished.tv_sec - started.tv_sec) * 1000.0 +
                (finished.tv_nsec - started.tv_nsec) / 1000000.0;
    printf("VKB compute ok n=%u ms=%.3f\n", N_ELEMS, ms);
    return 0;
}
