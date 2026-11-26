// vkprobe: dump Vulkan device capabilities as JSON. dlopen("libvulkan.so"), no layers.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static void kstr(const char *k, const char *v) { key(k); jstr(v); }
static void kint(const char *k, unsigned long long v) { key(k); fprintf(o, "%llu", v); }
static void khex(const char *k, unsigned long long v) { key(k); fprintf(o, "\"0x%llx\"", v); }
static void kbool(const char *k, VkBool32 v) { key(k); fputs(v ? "true" : "false", o); }
static void kflt(const char *k, double v) { key(k); fprintf(o, "%g", v); }
static void obeg(const char *k) { if (k) key(k); else SEP(); fputc('{', o); first = 1; }
static void oend(void) { fputc('}', o); first = 0; }
static void abeg(const char *k) { key(k); fputc('[', o); first = 1; }
static void aend(void) { fputc(']', o); first = 0; }
static void ver(const char *k, uint32_t v) {
    char b[64]; snprintf(b, sizeof b, "%u.%u.%u", VK_API_VERSION_MAJOR(v), VK_API_VERSION_MINOR(v), VK_API_VERSION_PATCH(v));
    kstr(k, b);
}

#define LOAD(inst, name) PFN_##name name = (PFN_##name)gipa(inst, #name); \
    if (!name) { fprintf(stderr, "missing %s\n", #name); }

static int has_ext(VkExtensionProperties *e, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(e[i].extensionName, name)) return 1;
    return 0;
}

int main(int argc, char **argv) {
    o = argc > 1 ? fopen(argv[1], "w") : stdout;
    if (!o) { perror("fopen"); return 1; }
    void *lib = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(lib, "vkGetInstanceProcAddr");
    if (!gipa) { fprintf(stderr, "no vkGetInstanceProcAddr\n"); return 1; }

    LOAD(NULL, vkEnumerateInstanceVersion);
    LOAD(NULL, vkCreateInstance);
    LOAD(NULL, vkEnumerateInstanceExtensionProperties);
    uint32_t iver = VK_API_VERSION_1_0;
    if (vkEnumerateInstanceVersion) vkEnumerateInstanceVersion(&iver);

    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "vkprobe",
        .apiVersion = iver >= VK_API_VERSION_1_3 ? VK_API_VERSION_1_3 : iver };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst;
    VkResult r = vkCreateInstance(&ici, NULL, &inst);
    if (r) { fprintf(stderr, "vkCreateInstance %d\n", r); return 1; }

    LOAD(inst, vkEnumeratePhysicalDevices);
    LOAD(inst, vkGetPhysicalDeviceProperties2);
    LOAD(inst, vkGetPhysicalDeviceFeatures2);
    LOAD(inst, vkGetPhysicalDeviceMemoryProperties);
    LOAD(inst, vkGetPhysicalDeviceQueueFamilyProperties);
    LOAD(inst, vkGetPhysicalDeviceFormatProperties);
    LOAD(inst, vkEnumerateDeviceExtensionProperties);

    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(inst, &ndev, NULL);
    VkPhysicalDevice *devs = calloc(ndev ? ndev : 1, sizeof *devs);
    vkEnumeratePhysicalDevices(inst, &ndev, devs);

    fputc('{', o); first = 1;
    ver("instanceVersion", iver);
    {
        uint32_t n = 0; vkEnumerateInstanceExtensionProperties(NULL, &n, NULL);
        VkExtensionProperties *e = calloc(n ? n : 1, sizeof *e);
        vkEnumerateInstanceExtensionProperties(NULL, &n, e);
        abeg("instanceExtensions");
        for (uint32_t i = 0; i < n; i++) { SEP(); jstr(e[i].extensionName); }
        aend(); free(e);
    }
    abeg("devices");
    for (uint32_t d = 0; d < ndev; d++) {
        VkPhysicalDevice pd = devs[d];
        uint32_t next = 0;
        vkEnumerateDeviceExtensionProperties(pd, NULL, &next, NULL);
        VkExtensionProperties *ext = calloc(next ? next : 1, sizeof *ext);
        vkEnumerateDeviceExtensionProperties(pd, NULL, &next, ext);
        int has_drv = has_ext(ext, next, VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
        int has_rb2 = has_ext(ext, next, VK_EXT_ROBUSTNESS_2_EXTENSION_NAME) || has_ext(ext, next, "VK_KHR_robustness2");

        VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        VkPhysicalDeviceDriverProperties drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        vkGetPhysicalDeviceProperties2(pd, &p2); // first get version
        uint32_t dver = p2.properties.apiVersion;
        if (has_drv || dver >= VK_API_VERSION_1_2) p2.pNext = &drv;
        vkGetPhysicalDeviceProperties2(pd, &p2);
        VkPhysicalDeviceProperties *p = &p2.properties;

        obeg(NULL);
        ver("apiVersion", p->apiVersion);
        khex("driverVersion", p->driverVersion);
        kstr("deviceName", p->deviceName);
        khex("vendorID", p->vendorID);
        khex("deviceID", p->deviceID);
        kint("deviceType", p->deviceType);
        if (p2.pNext) {
            obeg("driverProperties");
            kint("driverID", drv.driverID);
            kstr("driverName", drv.driverName);
            kstr("driverInfo", drv.driverInfo);
            char cv[32]; snprintf(cv, sizeof cv, "%u.%u.%u.%u", drv.conformanceVersion.major, drv.conformanceVersion.minor,
                drv.conformanceVersion.subminor, drv.conformanceVersion.patch);
            kstr("conformanceVersion", cv);
            oend();
        }
        abeg("extensions");
        for (uint32_t i = 0; i < next; i++) {
            SEP(); fputc('{', o); first = 1;
            kstr("name", ext[i].extensionName); kint("specVersion", ext[i].specVersion);
            fputc('}', o); first = 0;
        }
        aend();

        // features
        VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
        VkPhysicalDeviceVulkan11Features f11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
        VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
        VkPhysicalDeviceRobustness2FeaturesEXT rb2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
        void **tail = &f2.pNext;
        if (dver >= VK_API_VERSION_1_2) { *tail = &f11; tail = &f11.pNext; *tail = &f12; tail = &f12.pNext; }
        if (dver >= VK_API_VERSION_1_3) { *tail = &f13; tail = &f13.pNext; }
        if (has_rb2) { *tail = &rb2; tail = &rb2.pNext; }
        vkGetPhysicalDeviceFeatures2(pd, &f2);

        obeg("features10");
#define F(x) kbool(#x, f2.features.x)
        F(robustBufferAccess); F(fullDrawIndexUint32); F(imageCubeArray); F(independentBlend); F(geometryShader);
        F(tessellationShader); F(sampleRateShading); F(dualSrcBlend); F(logicOp); F(multiDrawIndirect);
        F(drawIndirectFirstInstance); F(depthClamp); F(depthBiasClamp); F(fillModeNonSolid); F(depthBounds);
        F(wideLines); F(largePoints); F(alphaToOne); F(multiViewport); F(samplerAnisotropy);
        F(textureCompressionETC2); F(textureCompressionASTC_LDR); F(textureCompressionBC); F(occlusionQueryPrecise);
        F(pipelineStatisticsQuery); F(vertexPipelineStoresAndAtomics); F(fragmentStoresAndAtomics);
        F(shaderTessellationAndGeometryPointSize); F(shaderImageGatherExtended); F(shaderStorageImageExtendedFormats);
        F(shaderStorageImageMultisample); F(shaderStorageImageReadWithoutFormat); F(shaderStorageImageWriteWithoutFormat);
        F(shaderUniformBufferArrayDynamicIndexing); F(shaderSampledImageArrayDynamicIndexing);
        F(shaderStorageBufferArrayDynamicIndexing); F(shaderStorageImageArrayDynamicIndexing); F(shaderClipDistance);
        F(shaderCullDistance); F(shaderFloat64); F(shaderInt64); F(shaderInt16); F(shaderResourceResidency);
        F(shaderResourceMinLod); F(sparseBinding); F(sparseResidencyBuffer); F(sparseResidencyImage2D);
        F(sparseResidencyImage3D); F(sparseResidencyAliased); F(variableMultisampleRate); F(inheritedQueries);
#undef F
        oend();
        if (dver >= VK_API_VERSION_1_2) {
            obeg("features11");
#define F(x) kbool(#x, f11.x)
            F(storageBuffer16BitAccess); F(uniformAndStorageBuffer16BitAccess); F(storagePushConstant16); F(storageInputOutput16);
            F(multiview); F(multiviewGeometryShader); F(multiviewTessellationShader); F(variablePointersStorageBuffer);
            F(variablePointers); F(protectedMemory); F(samplerYcbcrConversion); F(shaderDrawParameters);
#undef F
            oend();
            obeg("features12");
#define F(x) kbool(#x, f12.x)
            F(samplerMirrorClampToEdge); F(drawIndirectCount); F(storageBuffer8BitAccess); F(uniformAndStorageBuffer8BitAccess);
            F(storagePushConstant8); F(shaderBufferInt64Atomics); F(shaderSharedInt64Atomics); F(shaderFloat16); F(shaderInt8);
            F(descriptorIndexing); F(shaderInputAttachmentArrayDynamicIndexing); F(shaderUniformTexelBufferArrayDynamicIndexing);
            F(shaderStorageTexelBufferArrayDynamicIndexing); F(shaderUniformBufferArrayNonUniformIndexing);
            F(shaderSampledImageArrayNonUniformIndexing); F(shaderStorageBufferArrayNonUniformIndexing);
            F(shaderStorageImageArrayNonUniformIndexing); F(shaderInputAttachmentArrayNonUniformIndexing);
            F(shaderUniformTexelBufferArrayNonUniformIndexing); F(shaderStorageTexelBufferArrayNonUniformIndexing);
            F(descriptorBindingUniformBufferUpdateAfterBind); F(descriptorBindingSampledImageUpdateAfterBind);
            F(descriptorBindingStorageImageUpdateAfterBind); F(descriptorBindingStorageBufferUpdateAfterBind);
            F(descriptorBindingUniformTexelBufferUpdateAfterBind); F(descriptorBindingStorageTexelBufferUpdateAfterBind);
            F(descriptorBindingUpdateUnusedWhilePending); F(descriptorBindingPartiallyBound);
            F(descriptorBindingVariableDescriptorCount); F(runtimeDescriptorArray); F(samplerFilterMinmax);
            F(scalarBlockLayout); F(imagelessFramebuffer); F(uniformBufferStandardLayout); F(shaderSubgroupExtendedTypes);
            F(separateDepthStencilLayouts); F(hostQueryReset); F(timelineSemaphore); F(bufferDeviceAddress);
            F(bufferDeviceAddressCaptureReplay); F(bufferDeviceAddressMultiDevice); F(vulkanMemoryModel);
            F(vulkanMemoryModelDeviceScope); F(vulkanMemoryModelAvailabilityVisibilityChains); F(shaderOutputViewportIndex);
            F(shaderOutputLayer); F(subgroupBroadcastDynamicId);
#undef F
            oend();
        }
        if (dver >= VK_API_VERSION_1_3) {
            obeg("features13");
#define F(x) kbool(#x, f13.x)
            F(robustImageAccess); F(inlineUniformBlock); F(descriptorBindingInlineUniformBlockUpdateAfterBind);
            F(pipelineCreationCacheControl); F(privateData); F(shaderDemoteToHelperInvocation); F(shaderTerminateInvocation);
            F(subgroupSizeControl); F(computeFullSubgroups); F(synchronization2); F(textureCompressionASTC_HDR);
            F(shaderZeroInitializeWorkgroupMemory); F(dynamicRendering); F(shaderIntegerDotProduct); F(maintenance4);
#undef F
            oend();
        }
        if (has_rb2) {
            obeg("robustness2");
            kbool("robustBufferAccess2", rb2.robustBufferAccess2); kbool("robustImageAccess2", rb2.robustImageAccess2);
            kbool("nullDescriptor", rb2.nullDescriptor);
            oend();
        }

        // limits
        VkPhysicalDeviceLimits *l = &p->limits;
        obeg("limits");
#define L(x) kint(#x, l->x)
        L(maxImageDimension1D); L(maxImageDimension2D); L(maxImageDimension3D); L(maxImageDimensionCube); L(maxImageArrayLayers);
        L(maxTexelBufferElements); L(maxUniformBufferRange); L(maxStorageBufferRange); L(maxPushConstantsSize);
        L(maxMemoryAllocationCount); L(maxSamplerAllocationCount); L(bufferImageGranularity); L(sparseAddressSpaceSize);
        L(maxBoundDescriptorSets); L(maxPerStageDescriptorSamplers); L(maxPerStageDescriptorUniformBuffers);
        L(maxPerStageDescriptorStorageBuffers); L(maxPerStageDescriptorSampledImages); L(maxPerStageDescriptorStorageImages);
        L(maxPerStageResources); L(maxDescriptorSetSamplers); L(maxDescriptorSetUniformBuffers);
        L(maxDescriptorSetStorageBuffers); L(maxDescriptorSetSampledImages); L(maxDescriptorSetStorageImages);
        L(maxVertexInputAttributes); L(maxVertexInputBindings); L(maxVertexOutputComponents);
        L(maxGeometryShaderInvocations); L(maxGeometryOutputVertices); L(maxTessellationGenerationLevel);
        L(maxFragmentOutputAttachments); L(maxColorAttachments); L(maxComputeSharedMemorySize);
        L(maxComputeWorkGroupInvocations); L(maxDrawIndexedIndexValue); L(maxDrawIndirectCount); L(maxSamplerLodBias);
        L(maxViewports); L(maxFramebufferWidth); L(maxFramebufferHeight); L(maxFramebufferLayers);
        L(minUniformBufferOffsetAlignment); L(minStorageBufferOffsetAlignment); L(minTexelBufferOffsetAlignment);
        L(optimalBufferCopyOffsetAlignment); L(optimalBufferCopyRowPitchAlignment); L(nonCoherentAtomSize);
        L(timestampComputeAndGraphics); L(maxClipDistances); L(maxCullDistances);
        khex("framebufferColorSampleCounts", l->framebufferColorSampleCounts);
        kflt("maxSamplerAnisotropy", l->maxSamplerAnisotropy); kflt("timestampPeriod", l->timestampPeriod);
#undef L
        oend();

        // memory
        VkPhysicalDeviceMemoryProperties mp;
        vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        abeg("memoryHeaps");
        for (uint32_t i = 0; i < mp.memoryHeapCount; i++) {
            obeg(NULL); kint("size", mp.memoryHeaps[i].size); khex("flags", mp.memoryHeaps[i].flags); oend();
        }
        aend();
        abeg("memoryTypes");
        for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
            obeg(NULL); kint("heapIndex", mp.memoryTypes[i].heapIndex); khex("propertyFlags", mp.memoryTypes[i].propertyFlags); oend();
        }
        aend();

        // queues
        uint32_t nq = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
        VkQueueFamilyProperties *q = calloc(nq ? nq : 1, sizeof *q);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, q);
        abeg("queueFamilies");
        for (uint32_t i = 0; i < nq; i++) {
            obeg(NULL); khex("queueFlags", q[i].queueFlags); kint("queueCount", q[i].queueCount);
            kint("timestampValidBits", q[i].timestampValidBits); oend();
        }
        aend(); free(q);

        // formats
        static const struct { const char *n; VkFormat f; } fmts[] = {
            {"BC1_RGB_UNORM", VK_FORMAT_BC1_RGB_UNORM_BLOCK}, {"BC1_RGBA_UNORM", VK_FORMAT_BC1_RGBA_UNORM_BLOCK},
            {"BC1_RGBA_SRGB", VK_FORMAT_BC1_RGBA_SRGB_BLOCK}, {"BC2_UNORM", VK_FORMAT_BC2_UNORM_BLOCK},
            {"BC3_UNORM", VK_FORMAT_BC3_UNORM_BLOCK}, {"BC3_SRGB", VK_FORMAT_BC3_SRGB_BLOCK},
            {"BC4_UNORM", VK_FORMAT_BC4_UNORM_BLOCK}, {"BC5_UNORM", VK_FORMAT_BC5_UNORM_BLOCK},
            {"BC6H_UFLOAT", VK_FORMAT_BC6H_UFLOAT_BLOCK}, {"BC6H_SFLOAT", VK_FORMAT_BC6H_SFLOAT_BLOCK},
            {"BC7_UNORM", VK_FORMAT_BC7_UNORM_BLOCK}, {"BC7_SRGB", VK_FORMAT_BC7_SRGB_BLOCK},
            {"ETC2_R8G8B8_UNORM", VK_FORMAT_ETC2_R8G8B8_UNORM_BLOCK}, {"ETC2_R8G8B8A8_UNORM", VK_FORMAT_ETC2_R8G8B8A8_UNORM_BLOCK},
            {"EAC_R11G11_UNORM", VK_FORMAT_EAC_R11G11_UNORM_BLOCK},
            {"ASTC_4x4_UNORM", VK_FORMAT_ASTC_4x4_UNORM_BLOCK}, {"ASTC_4x4_SRGB", VK_FORMAT_ASTC_4x4_SRGB_BLOCK},
            {"R8G8B8A8_UNORM", VK_FORMAT_R8G8B8A8_UNORM}, {"R8G8B8A8_SRGB", VK_FORMAT_R8G8B8A8_SRGB},
            {"B8G8R8A8_UNORM", VK_FORMAT_B8G8R8A8_UNORM}, {"B8G8R8A8_SRGB", VK_FORMAT_B8G8R8A8_SRGB},
            {"R16G16B16A16_SFLOAT", VK_FORMAT_R16G16B16A16_SFLOAT}, {"A2B10G10R10_UNORM", VK_FORMAT_A2B10G10R10_UNORM_PACK32},
            {"D16_UNORM", VK_FORMAT_D16_UNORM}, {"X8_D24_UNORM", VK_FORMAT_X8_D24_UNORM_PACK32},
            {"D24_UNORM_S8_UINT", VK_FORMAT_D24_UNORM_S8_UINT}, {"D32_SFLOAT", VK_FORMAT_D32_SFLOAT},
            {"D32_SFLOAT_S8_UINT", VK_FORMAT_D32_SFLOAT_S8_UINT}, {"S8_UINT", VK_FORMAT_S8_UINT},
        };
        obeg("formats");
        for (size_t i = 0; i < sizeof fmts / sizeof *fmts; i++) {
            VkFormatProperties fp;
            vkGetPhysicalDeviceFormatProperties(pd, fmts[i].f, &fp);
            obeg(fmts[i].n);
            khex("optimalTilingFeatures", fp.optimalTilingFeatures);
            khex("linearTilingFeatures", fp.linearTilingFeatures);
            khex("bufferFeatures", fp.bufferFeatures);
            kbool("sampled", (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0);
            oend();
        }
        oend();
        oend();
        free(ext);
    }
    aend();
    fputs("\n}\n", o);
    if (o != stdout) fclose(o);
    return 0;
}
