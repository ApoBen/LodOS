# AHB / dmabuf / dma_heap / Vulkan AHB import — POCO X7 Pro (Mali-G720 MC7, MT6899)

Probe: `tools/ahbprobe` (run as adb shell, uid 2000). Raw: `ahb-dmabuf.json`. Date 2026-11-26.
App uid (untrusted_app SELinux) not tested yet; dma_heap and fd results may differ there.

## Results
| test | result |
|---|---|
| AHB alloc 1280x720 RGBA8, GPU_SAMPLED\|GPU_COLOR_OUTPUT\|COMPOSER_OVERLAY | ok, stride 1280 |
| BGRA8 (fmt 5), RGBX8, RGB565, RGBA16F | all ok |
| native_handle | numFds=3, numInts=62: fd0 `anon_inode:gralloc_extra`, **fd1 = dmabuf** (exp_name `mtk_mm-uncached`, 3747840 B), fd2 memfd `gralloc_shared_memory` (metadata) |
| /dev/dma_heap | 12 heaps. Open ok: system, system-uncached, mtk_mm, mtk_mm-uncached, mtk_prot*/svp*. EACCES: mtk_sapu_page-uncached, mtk_slc. /dev/ion absent |
| dma_heap/system alloc ioctl + mmap | ok |
| Vk exts | AHB ext, EXT_external_memory_dma_buf, KHR_external_memory_fd, KHR_external_semaphore_fd, KHR_external_fence_fd, EXT_queue_family_foreign, EXT_image_drm_format_modifier: all present. EXT_external_memory_host: absent |
| vkGetAndroidHardwareBufferPropertiesANDROID | OK, allocSize 3686400, memTypeBits 0x2, format 37 (RGBA8_UNORM). BGRA AHB also maps to VkFormat 37 (driver swizzles; externalFormat 0x28) |
| AHB import -> VkImage + dedicated VkDeviceMemory, vkCmdClearColorImage magenta | all VK_SUCCESS; **CPU lock readback = 0xffff00ff** -> real zero-copy |
| Vk image -> AHB export (vkGetMemoryAndroidHardwareBufferANDROID) | OK (1280x720 fmt1 usage 0x300) |
| ext mem caps (buffer) | OPAQUE_FD: unsupported (vkCreateBuffer = ERROR_INVALID_EXTERNAL_HANDLE). DMA_BUF: **import-only** (features 0x4). AHB: export+import (0x6) |
| vkGetMemoryFdKHR(DMA_BUF) on exportable alloc | returns VK_SUCCESS but fd=-1 (driver allows the call but cannot export) |
| dma_heap/system dmabuf -> vkAllocateMemory(VkImportMemoryFdInfoKHR DMA_BUF) + bind buffer | OK (memTypeBits 0x2) |
| sync_fd semaphore / fence | import+export (0x3). OPAQUE_FD: unsupported |

## Implications for the bridge (D5/D6)
- Zero-copy present path confirmed: alloc AHB -> import to Vulkan -> render -> hand the same AHB to SurfaceControl/SurfaceView. Gralloc-allocated memory is a dmabuf (fd index 1 of the native handle, not 0; don't assume fd0).
- Vulkan cannot export its own memory as dmabuf or opaque fd. Shared memory must therefore come from **AHB (preferred) or dma_heap**, then be imported into Vulkan. The Vortek-like ICD bridge should allocate backing memory host-side as AHB (VkExportMemoryAllocateInfo AHB works) and pass the dmabuf fd (native handle fd1) / AHB socket handle to the guest.
- Wayland linux-dmabuf from the guest: a dmabuf imports as VkBuffer memory. VkImage import via EXT_image_drm_format_modifier is untested (next probe). Fallback: wrap guest buffers by allocating AHBs host-side and exporting their dmabufs to guest clients.
- Sync: use SYNC_FD for semaphores/fences (matches Android acquire/release fences). No OPAQUE_FD, so timeline-semaphore sharing via opaque fd is out.
- Prefer heap `system`/`mtk_mm`. Re-test from the app uid before relying on direct dma_heap access; AHB alloc is always allowed.
- The BGRA AHB reports VkFormat 37 (RGBA8). The channel order (swizzle vs. suggestedYcbcr components) was not verified; check it before using BGRA AHBs, and default to RGBA8 AHBs.
