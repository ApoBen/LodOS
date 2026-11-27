/* T041: external semaphore/fence fd round-trip through libvulkan_lodos.so -> vkb_server -> host ICD.
 * Semaphore A (exportable, SYNC_FD or OPAQUE_FD) is signaled by an empty submit, exported to an fd
 * (passed host->guest via SCM_RIGHTS), imported into semaphore B (fd passed guest->host), then a
 * submit waits on B and signals a fence. Same for fences when VK_KHR_external_fence_fd is exposed.
 * env VKB_SYNCFD_OPAQUE=1 forces OPAQUE_FD. Prints: VKB syncfd ok sem=<type> fence=<type|skip> */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define CK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { \
    fprintf(stderr, "FAIL %s -> %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)
#define TIMEOUT_NS 5000000000ull

static int has_ext(VkPhysicalDevice pd, const char *name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL);
    VkExtensionProperties *p = calloc(n ? n : 1, sizeof *p);
    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, p);
    int f = 0;
    for (uint32_t i = 0; i < n; i++) if (!strcmp(p[i].extensionName, name)) f = 1;
    free(p);
    return f;
}

static const char *htname(uint32_t t) {
    return t == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT ? "SYNC_FD" :
           t == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT ? "OPAQUE_FD" : "?";
}

static int check_fd(int fd, const char *what) {
    if (fd == -1) { fprintf(stderr, "%s: fd=-1 (already signaled)\n", what); return 0; }
    if (fd < 0 || fcntl(fd, F_GETFD) < 0) { fprintf(stderr, "FAIL %s: bad fd %d\n", what, fd); exit(1); }
    fprintf(stderr, "%s: fd=%d valid in guest process\n", what, fd);
    return 1;
}

int main(void) {
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "host_syncfd",
                              .apiVersion = VK_API_VERSION_1_1 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst;
    CK(vkCreateInstance(&ici, NULL, &inst));
    uint32_t npd = 1;
    VkPhysicalDevice pd;
    VkResult er = vkEnumeratePhysicalDevices(inst, &npd, &pd);
    if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || !npd) { fprintf(stderr, "FAIL no pdev\n"); return 1; }

    int sem_ext = has_ext(pd, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME);
    int fen_ext = has_ext(pd, VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME);
    fprintf(stderr, "ext: semaphore_fd=%d fence_fd=%d\n", sem_ext, fen_ext);
    if (!sem_ext) { fprintf(stderr, "FAIL VK_KHR_external_semaphore_fd not exposed\n"); return 1; }

    /* pick semaphore handle type: prefer SYNC_FD, else OPAQUE_FD */
    uint32_t sem_ht = 0;
    const uint32_t sem_cands[2] = { VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT,
                                    VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT };
    const int first = getenv("VKB_SYNCFD_OPAQUE") ? 1 : 0; /* force OPAQUE_FD for sem+fence */
    for (int i = first; i < 2 && !sem_ht; i++) {
        VkPhysicalDeviceExternalSemaphoreInfo si = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO,
                                                     .handleType = sem_cands[i] };
        VkExternalSemaphoreProperties sp = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES };
        vkGetPhysicalDeviceExternalSemaphoreProperties(pd, &si, &sp);
        uint32_t need = VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
        if ((sp.externalSemaphoreFeatures & need) == need)
            sem_ht = sem_cands[i];
    }
    if (!sem_ht) { fprintf(stderr, "FAIL no exportable+importable semaphore fd type\n"); return 1; }

    uint32_t fen_ht = 0;
    if (fen_ext) {
        const uint32_t fc[2] = { VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT, VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT };
        for (int i = first; i < 2 && !fen_ht; i++) {
            VkPhysicalDeviceExternalFenceInfo fi = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO,
                                                     .handleType = fc[i] };
            VkExternalFenceProperties fp = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES };
            vkGetPhysicalDeviceExternalFenceProperties(pd, &fi, &fp);
            uint32_t need = VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT;
            if ((fp.externalFenceFeatures & need) == need) fen_ht = fc[i];
        }
    }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
    const char *exts[2] = { VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_FENCE_FD_EXTENSION_NAME };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1,
                               .pQueueCreateInfos = &qci, .enabledExtensionCount = fen_ht ? 2 : 1,
                               .ppEnabledExtensionNames = exts };
    VkDevice dev;
    CK(vkCreateDevice(pd, &dci, NULL, &dev));
    VkQueue q;
    vkGetDeviceQueue(dev, 0, 0, &q);

    PFN_vkGetSemaphoreFdKHR pGetSemFd = (PFN_vkGetSemaphoreFdKHR)vkGetDeviceProcAddr(dev, "vkGetSemaphoreFdKHR");
    PFN_vkImportSemaphoreFdKHR pImpSemFd = (PFN_vkImportSemaphoreFdKHR)vkGetDeviceProcAddr(dev, "vkImportSemaphoreFdKHR");
    PFN_vkGetFenceFdKHR pGetFenFd = (PFN_vkGetFenceFdKHR)vkGetDeviceProcAddr(dev, "vkGetFenceFdKHR");
    PFN_vkImportFenceFdKHR pImpFenFd = (PFN_vkImportFenceFdKHR)vkGetDeviceProcAddr(dev, "vkImportFenceFdKHR");
    if (!pGetSemFd || !pImpSemFd) { fprintf(stderr, "FAIL GDPA semaphore fd fns\n"); return 1; }
    if (fen_ht && (!pGetFenFd || !pImpFenFd)) { fprintf(stderr, "FAIL GDPA fence fd fns\n"); return 1; }

    /* ---- semaphore round-trip */
    VkExportSemaphoreCreateInfo esci = { .sType = VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO,
                                         .handleTypes = sem_ht };
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &esci };
    VkSemaphore semA, semB;
    CK(vkCreateSemaphore(dev, &sci, NULL, &semA));
    sci.pNext = NULL;
    CK(vkCreateSemaphore(dev, &sci, NULL, &semB));

    VkSubmitInfo sig = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .signalSemaphoreCount = 1, .pSignalSemaphores = &semA };
    CK(vkQueueSubmit(q, 1, &sig, VK_NULL_HANDLE));

    VkSemaphoreGetFdInfoKHR gfi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR,
                                    .semaphore = semA, .handleType = sem_ht };
    int sfd = -2;
    CK(pGetSemFd(dev, &gfi, &sfd));
    check_fd(sfd, "semaphore export");
    if (sfd == -1 && sem_ht != VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT) {
        fprintf(stderr, "FAIL OPAQUE_FD export returned -1\n"); return 1;
    }

    VkImportSemaphoreFdInfoKHR isi = { .sType = VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR,
                                       .semaphore = semB, .handleType = sem_ht, .fd = sfd,
                                       .flags = sem_ht == VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT
                                                    ? VK_SEMAPHORE_IMPORT_TEMPORARY_BIT : 0 };
    CK(pImpSemFd(dev, &isi));   /* on success the fd is owned by the implementation */

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CK(vkCreateFence(dev, &fci, NULL, &fence));
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo wait = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1,
                          .pWaitSemaphores = &semB, .pWaitDstStageMask = &ws };
    CK(vkQueueSubmit(q, 1, &wait, fence));
    CK(vkWaitForFences(dev, 1, &fence, VK_TRUE, TIMEOUT_NS));
    fprintf(stderr, "semaphore: wait on imported semaphore done (%s)\n", htname(sem_ht));

    /* ---- fence round-trip */
    const char *fen_name = "skip";
    if (fen_ht) {
        VkExportFenceCreateInfo efci = { .sType = VK_STRUCTURE_TYPE_EXPORT_FENCE_CREATE_INFO, .handleTypes = fen_ht };
        VkFenceCreateInfo xfci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = &efci };
        VkFence fA, fB;
        CK(vkCreateFence(dev, &xfci, NULL, &fA));
        CK(vkCreateFence(dev, &fci, NULL, &fB));
        CK(vkQueueSubmit(q, 0, NULL, fA));
        if (fen_ht == VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT)
            CK(vkWaitForFences(dev, 1, &fA, VK_TRUE, TIMEOUT_NS));
        VkFenceGetFdInfoKHR gffi = { .sType = VK_STRUCTURE_TYPE_FENCE_GET_FD_INFO_KHR, .fence = fA, .handleType = fen_ht };
        int ffd = -2;
        CK(pGetFenFd(dev, &gffi, &ffd));
        check_fd(ffd, "fence export");
        VkImportFenceFdInfoKHR ifi = { .sType = VK_STRUCTURE_TYPE_IMPORT_FENCE_FD_INFO_KHR, .fence = fB,
                                       .handleType = fen_ht, .fd = ffd,
                                       .flags = fen_ht == VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT
                                                    ? VK_FENCE_IMPORT_TEMPORARY_BIT : 0 };
        CK(pImpFenFd(dev, &ifi));
        CK(vkWaitForFences(dev, 1, &fB, VK_TRUE, TIMEOUT_NS));
        fen_name = fen_ht == VK_EXTERNAL_FENCE_HANDLE_TYPE_SYNC_FD_BIT ? "SYNC_FD" : "OPAQUE_FD";
        fprintf(stderr, "fence: wait on imported fence done (%s)\n", fen_name);
        vkDestroyFence(dev, fA, NULL);
        vkDestroyFence(dev, fB, NULL);
    }

    CK(vkDeviceWaitIdle(dev));
    vkDestroyFence(dev, fence, NULL);
    vkDestroySemaphore(dev, semA, NULL);
    vkDestroySemaphore(dev, semB, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    printf("VKB syncfd ok sem=%s fence=%s\n", htname(sem_ht), fen_name);
    return 0;
}
