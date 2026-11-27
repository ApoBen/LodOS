# S1 Research: Mali GPU Bridge (`glibc` Guest under `proot` -> Bionic Vendor Driver)

Target hardware & environment: POCO X7 Pro (Dimensity 8400, Mali-G720, 4 KB pages, HyperOS / Android 15, unrooted), Debian 13 (`trixie`) `aarch64` `glibc` guest under `proot` (`ai.md` D3, D4, D5, D6, D11; risks R1, R2).

---

## Sources

All upstream and fork repositories were shallow-cloned into `third_party/src/` (gitignored) and inspected at source level:

| Component | Repository & Branch | Commit | Key Source Paths (relative to `third_party/src/`) |
| :--- | :--- | :--- | :--- |
| Winlator umbrella | `https://github.com/brunodev85/winlator.git` (`main`) | `30aafbdb9eea47b92669201621e03cd2cf04540d` | `.gitmodules` linking `app`, `vortek`, `gladio` |
| Vortek server (`libvortekrenderer.so`) | `https://github.com/brunodev85/winlator-app.git` (`main`) | `a030f552f452158a2db64fdb32b490fa19c0b48d` | `winlator/app/app/src/main/cpp/vortekrenderer/{src,include}/`, `winlator/app/app/src/main/cpp/winlator/{src/gpu_image.c,include/dma_utils.h}`, `winlator/app/app/src/main/java/com/winlator/xenvironment/components/VortekRendererComponent.java` |
| Vortek guest ICD (`libvulkan_vortek.so`) | `https://github.com/brunodev85/vortek.git` (`main`) | `4e0d790ef8afee4766b40e194bc82d2505d5ea0f` | `vortek/src/{main.c,vulkan_calls.c,ring_buffer.c,vk_object.c,async_pipeline_manager.c}`, `vortek/include/vortek_serializer.h` |
| Winlator Gladio | `https://github.com/brunodev85/gladio.git` (`main`) | `116c0d14dedbea3bd057f98f1db138bb1efe225e` | `winlator/gladio/` |
| Termux Vulkan Wrapper (Mesa) | `https://github.com/xMeM/mesa.git` (`wrapper`) | `e65c7eb6ee2f9903c3256f2677beb1d98464103f` | `xMeM-mesa/src/vulkan/wrapper/{wrapper_instance.c,wrapper_physical_device.c,wrapper_device.c,wrapper_device_memory.c}`, `xMeM-mesa/src/vulkan/wsi/wsi_common_ahardware_buffer.c` |
| Bionic Vulkan Wrapper (Mali + GPU BCn fork) | `https://github.com/leegao/bionic-vulkan-wrapper.git` (`main`) | `3abf48704a4d9303967a0811c0dc8ff8567d613b` | `bionic-vulkan-wrapper/src/vulkan/wrapper/{wrapper_device.c,wrapper_image.c,wrapper_device_memory.c,spirv_edit.cpp,bcdec.c,s3tc.comp,bc6.comp,bc7.comp}` |
| Vortek protocol notes | `https://github.com/leegao/vortek-deep-dive.git` (`master`) | `e8b2b85bae8859bce8ba26c2967cffa5336811a5` | `vortek-deep-dive/README.md` |
| Termux:X11 (`lorie`) | `https://github.com/termux/termux-x11.git` (`master`) | `24ce2076fe68eff1599da286c5c162f7693c0501` | `termux-x11/app/src/main/cpp/lorie/{InitOutput.c,buffer.c,renderer.cpp}` |
| Termux `proot` | `https://github.com/termux/proot.git` (`master`) | `c0b2d098469a64a88c0a7e240b1a1dc252988cdb` | `proot/src/syscall/socket.c`, `proot/src/extension/ashmem_memfd/ashmem_memfd.c` |

Note on upstream packaging:
- Upstream `brunodev85/winlator` now publishes both the Vortek client (`brunodev85/vortek`) and server (`brunodev85/winlator-app` under `cpp/vortekrenderer/`) via git submodules, though their `HEAD` commits are protocol-skewed (detailed below).
- Termux's `vulkan-wrapper-android` (`mesa-vulkan-icd-wrapper` subpackage in `termux/termux-packages/packages/mesa`) originates from `xMeM/mesa` (`wrapper` branch); Winlator Bionic/CMOD forks use `leegao/bionic-vulkan-wrapper`, which extends `xMeM/mesa` with GPU compute BC1–BC7 decompression and Mali SPIR-V/allocation workarounds.

---

## Licenses

Every inspected repository and embedded dependency is **GPL-3.0-compatible** (recorded with commit hashes in `THIRD_PARTY.md`):

- **`brunodev85/winlator`, `winlator-app`, `vortek`, `gladio` — `LGPL-2.1`**:
  - Compatible with **GPL-3.0** via LGPL-2.1 §3 (explicitly permits relicensing under GPLv2-or-later, hence GPL-3.0).
  - Embedded third-party headers in `winlator-app`: `bc_decoder.h` derives from Google SwiftShader (**`Apache-2.0`**, compatible with GPL-3.0; note combining LGPL-2.1 + Apache-2.0 yields GPL-3.0, matching LodOS); `libadrenotools` is **`MIT`**.
- **`xMeM/mesa` (`wrapper`) — `MIT`** (`docs/license.rst`, standard Mesa/Khronos MIT licenses):
  - Fully **GPL-3.0-compatible**.
- **`leegao/bionic-vulkan-wrapper` — `MIT` + `MIT OR Unlicense` + `Apache-2.0`**:
  - Core Mesa wrapper (`src/vulkan/wrapper/*.c`) is **`MIT`**.
  - Compute BC1–BC7 shaders (`s3tc.comp`, `bc6.comp`, `bc7.comp`, originally by Hans-Kristian Arntzen, modified by Lee Gao) are **`MIT`**.
  - CPU fallback `bcdec.h` (Sergii Kudlai) is dual-licensed **`MIT OR Unlicense`**.
  - Linked `SPIRV-Tools` (used in `spirv_edit.cpp` for Mali shader passes) is **`Apache-2.0`**.
  - All components are **GPL-3.0-compatible**.
- **`leegao/vortek-deep-dive` — `MIT`**: **GPL-3.0-compatible** (reference documentation only).
- **`termux/termux-x11` — `GPL-3.0`** (`LICENSE`, plus X11/MIT lorie/xorg headers): **GPL-3.0-compatible**.
- **`termux/proot` — `GPL-2.0-or-later`** (per source headers in `src/loader/loader.c`, `src/syscall/socket.c`, `src/execve/enter.c`): **GPL-3.0-compatible**.

---

## Vortek

### Architecture & Transport
- **Process model**: Out-of-process RPC split between a guest `glibc` Vulkan ICD (`libvulkan_vortek.so`, ~50.4k LOC C including 43,555-line `vortek_serializer.h`) and an Android Bionic JNI library (`libvortekrenderer.so`, ~6.5k LOC C) that `dlopen`s `/system/lib64/libvulkan.so`.
- **Bootstrap & rings**:
  - Client connects to `AF_UNIX` `SOCK_STREAM` at hardcoded `/data/data/com.winlator/files/rootfs/tmp/.vortek/V0`.
  - On `REQUEST_CODE_CREATE_CONTEXT`, server allocates `ashmem` regions (`SERVER_RING_BUFFER_SIZE = 4 MB`, `CLIENT_RING_BUFFER_SIZE = 256 KB`) and transfers fds via `SCM_RIGHTS`.
  - Payloads `< 4 MB` traverse lock-free atomic SPSC ring buffers (`ring_buffer.c`: spin-yield backoff -> `nanosleep`, or `SYS_futex` semaphores on 8 auxiliary 128-byte rings in v2.1 client). Payloads `>= 4 MB` fall back to stream socket writes (`REQUEST_CODE_SEND_EXTRA_DATA`).
- **Command buffer batching**: `vkCmd*` entrypoints do **zero IPC** during recording; calls are serialized into a per-`VkCommandBuffer` heap `CommandBatch` (`vulkan_calls.c`) and transmitted in one contiguous buffer at `vkEndCommandBuffer` (`REQUEST_CODE_VK_END_COMMAND_BUFFER`) for server replay (`request_handler.c`).
- **Host-visible memory & `MAP_FIXED`**:
  - `resource_memory.c` (`ResourceMemory_allocate`) backs host-visible `VkDeviceMemory` with an exportable fd using three fallbacks:
    1. `VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT` (`vkGetMemoryFdKHR`).
    2. `/dev/dma_heap/system-uncached` or `/dev/dma_heap/system` (fallback `/dev/ion`) imported via `VK_EXT_external_memory_dma_buf`.
    3. `AHardwareBuffer` (`AHARDWAREBUFFER_FORMAT_BLOB`) extracting `AHardwareBuffer_getNativeHandle(ahb)->data[0]` and importing via `VK_ANDROID_external_memory_android_hardware_buffer`.
  - `vkMapMemory` / `vkMapMemory2KHR` passes the fd over `SCM_RIGHTS`; guest `mmap`s it directly (`MAP_SHARED`, supporting `VK_EXT_map_memory_placed` via `MAP_FIXED` for Wine/Box64).

### Swapchain, BCn, and Mali Workarounds
- **Swapchain/Present**: `VK_KHR_xlib_surface` only. Guest sends X11 `Window` XID -> `xwindow_swapchain.c` invokes JNI `getWindowHardwareBuffer(windowId)` -> allocates a **single** `AHardwareBuffer` (`minImageCount = 1`, `acquireNextImage` always returns `imageIndex = 0`) -> imports into `VkImage` -> `vkQueuePresentKHR` calls JNI `updateWindowContent(windowId)` -> binds `AHardwareBuffer` to `EGLImageKHR` (`glEGLImageTargetTexture2DOES`) in Winlator's GLES XServer.
- **BCn handling**: `texture_decoder.c` + `bc_decoder.h` performs **CPU-only** decompression for **BC1–BC5 only** (substitutes `VK_FORMAT_R8G8B8A8_UNORM` on `vkCreateImage`, intercepts `vkCmdCopyBufferToImage` at `vkQueueSubmit`, `mmap`s source buffer fd, decodes on an 8-thread worker pool with MurmurHash64 disk cache for `>= 1024x1024`, uploads via staging buffer). **BC6H and BC7 are completely unimplemented** (fatal for DX11/DX12 games).
- **Mali & Zink workarounds**:
  - `vulkan_helper.c`: disables `VK_EXT_extended_dynamic_state{,2,3}`, `VK_EXT_color_write_enable`, and `VK_KHR_push_descriptor` when `pEngineName == "mesa zink"`; disables `extended_dynamic_state{,2,3}`, `hdr_metadata`, and `swapchain_maintenance1` on `VK_DRIVER_ID_ARM_PROPRIETARY` for older DXVK; spoofs desktop `VkPhysicalDeviceFeatures` (`textureCompressionBC`, `depthClamp`, `fillModeNonSolid`, `shaderClipDistance`, `logicOp`, `dualSrcBlend`, `multiViewport`) and strips unsupported bits in `vkCreateDevice`.
  - `shader_inspector.c`: CPU SPIR-V rewriter that strips unsupported `ClipDistance`, converts `USCALED`/`SSCALED` vertex attributes to `UINT`/`SINT` + `OpConvertUToF`/`OpConvertSToF`, removes Mali `OpSelect` image bound checks, patches VS/FS component count mismatches, strips constant `1.0f` `PointSize`, and widens vertex push-constant stage flags to include `FRAGMENT_BIT`.

### Blocking Defects Against Direct Reuse
1. **Git client/server protocol mismatch**: `brunodev85/vortek` `HEAD` (v2.1 client) expects `2 + 8 = 10` shm fds on init and `ringId`-based fence/semaphore/pipeline waits (`REQUEST_CODE_GET_PIPELINE_READY = 3`), whereas `winlator-app` `HEAD` `vortekrenderer` sends 2 shm fds and uses `pipe`/`eventfd`/`sync_fd` (`winlator-app` bundles a binary `vortek-2.1.tzst` rather than matching server C code).
2. **Global IPC mutex**: `vulkan_calls.c` serializes every non-batched Vulkan call behind one global `pthread_mutex_t vt_call_mutex`, bottlenecking multi-threaded renderers (DXVK, VKD3D, Zink).
3. **No Wayland WSI & broken external fd APIs**: Lacks `VK_KHR_wayland_surface` and `VK_EXT_image_drm_format_modifier`; `vkImportSemaphoreFdKHR`, `vkImportFenceFdKHR`, and `vkGetMemoryFdPropertiesKHR` are stubbed to `VK_ERROR_INVALID_EXTERNAL_HANDLE`, breaking Mesa `wsi_common_wayland.c` and Zink/`wlroots` `dma_buf` sharing.
4. **Maintainability**: 43.5k LOC monolithic `vortek_serializer.h` with no `vk.xml` generator script in repo, hardcoded `/data/data/com.winlator` paths, and tight coupling to Java XServer JNI.

---

## Wrapper

### Architecture (`xMeM/mesa` & `leegao/bionic-vulkan-wrapper`)
- **Process model**: **Strictly in-process Bionic ICD** (`libvulkan_wrapper.so`) built inside Mesa (`src/vulkan/wrapper/`) using `vk_lite_runtime`/`vk_runtime` and `idep_vulkan_wsi`.
- Calls `dlopen("/system/lib64/libvulkan.so", RTLD_LOCAL | RTLD_NOW)` directly (`wrapper_instance.c:68`) inside a native Termux/Android Bionic process, translating desktop WSI (`xcb`, `xlib`, `wayland`), `VK_EXT_map_memory_placed` (`MAP_FIXED` over `/dev/dma_heap/system` or `AHardwareBuffer` fds in `wrapper_device_memory.c`), BCn textures, and driver quirks onto Android's vendor driver.

### Why Guest `glibc` Cannot Load Bionic Mali In-Process (`libhybris`-style is Non-Viable)
1. **`aarch64` `TPIDR_EL0` TLS ABI collision**: Both `glibc` and Android Bionic bind hardware register `TPIDR_EL0` to incompatible thread control block / fixed TLS slot layouts (`TLS_SLOT_SELF`, `TLS_SLOT_BIONIC_TLS`, `TLS_SLOT_OPENGL_API`, stack guard). Closed-source `/vendor/lib64/egl/libGLES_mali.so` spawns internal Mali worker threads via Bionic `pthread_create`, uses Bionic mutexes/allocators, and indexes fixed Bionic TLS offsets — crashing immediately if entered with a `glibc` `TPIDR_EL0` or calling `glibc` callbacks from a Bionic worker thread.
2. **Android 14/15 APEX & Treble Linker Namespaces**: `/system/lib64/libvulkan.so` loads `/vendor/lib64/hw/vulkan.*.so` via `android_dlopen_ext` across isolated `linker64` namespaces (`sphal`, `vndk`, `default` defined by `/linkerconfig/ld.config.txt` and backed by `/apex/com.android.runtime/lib64/bionic/libc.so`). `libhybris` lacks modern Android 14/15 APEX Vulkan namespace emulation; `libadrenotools` only patches Bionic namespaces *inside an already-Bionic host process* for custom Adreno Turnip `.so`s. Furthermore, open-source Mesa **PanVK** cannot run rootless on POCO X7 Pro (Mali-G720 CSF) because Xiaomi/MediaTek ships Arm's proprietary `/dev/mali0` (`kbase`) kernel driver rather than upstream Linux `panthor` DRM.
3. **`proot` path & `/proc/self/maps` interference**: `proot` translates guest filesystem paths to Debian's rootfs and expects `ld-linux-aarch64.so.1` layout. Even Termux's `glibc-packages` abandons `vulkan-wrapper-android` for IPC (`virtio`), and Winlator forks using `bionic-vulkan-wrapper` had to drop `glibc` containers entirely ("Winlator Bionic"). With LodOS locked to Debian 13 `glibc` (`ai.md` D3/D4), **guest-to-Mali Vulkan must cross a process boundary via IPC**.

### High-Value Mali & BC1–BC7 Capabilities in `leegao/bionic-vulkan-wrapper`
While `bionic-vulkan-wrapper` cannot run inside the `glibc` guest, **it runs natively in LodOS's Bionic host process** and solves Mali's hardest compatibility problems far better than Vortek:
- **GPU Compute BC1–BC7 Decompression** (`s3tc.comp`, `bc6.comp`, `bc7.comp`, `wrapper_image.c`, `wrapper_device.c`):
  - Emulates all 16 BC1–BC7 formats (`BC1..BC5` -> `R8G8B8A8_UNORM`/`SRGB`, `BC6H` -> `R16G16B16A16_SFLOAT`, `BC7` -> `R8G8B8A8_UNORM`/`SRGB`).
  - Intercepts `vkCmdCopyBufferToImage` and dispatches compute shaders writing directly into `VK_IMAGE_LAYOUT_GENERAL` storage `VkImageView`s on the GPU (falling back to staging buffer + `bcdec.h` CPU decode only for non-4x4-aligned/striped pitch or transfer-only queues).
- **Critical Mali Driver Fixes**:
  - **Clears `prefersDedicatedAllocation`** in `GetImageMemoryRequirements2` / `GetBufferMemoryRequirements2` (`wrapper_device.c:470`): Arm's Mali driver sets `prefersDedicatedAllocation = VK_TRUE` on most images, tricking DXVK/Zink into allocating one `VkDeviceMemory` (`kbase` kernel VMA) per texture until VMA limits exhaust and `vkAllocateMemory` fails.
  - **`SPIRV-Tools` Mali compiler bug workarounds** (`spirv_edit.cpp`): executes `CreateFixMaliSpecConstantCompositePass()`, `CreateMaliOptimizationBarrierPass()`, and `CreateRemoveClipCullDistPass()` on `vkCreateShaderModule` when `driverID == VK_DRIVER_ID_ARM_PROPRIETARY`.
  - **State/Format sanitization**: rewrites `VK_POLYGON_MODE_LINE` -> `FILL` when `fillModeNonSolid` is unsupported, masks broken `extended_dynamic_state{,2,3}` and subgroup/float-control flags, and strips `VK_IMAGE_ASPECT_STENCIL_BIT` when downgrading unsupported depth-stencil formats.

---

## Present path

### GDPA gaps after T063
- Added RPC coverage for 128 previously missing names, including VK_EXT_debug_marker begin/end/insert and `vkSetDeviceMemoryPriorityEXT`; codegen accepts all four additions (380 commands accepted, 377 RPC bridged, 24 remain stubs for unsupported pointer/payload shapes).
- Remaining large groups: ray tracing and acceleration-structure builds, video, platform/external-handle WSI, opaque capture descriptor payloads, and commands whose pointer shapes still exceed generator support. Avoid exposing unsupported Mali features until device capability filtering is wired in.
- `test/host_zink.sh` writes observed missing GDPA names to `build/vk-bridge/gdpa-misses.txt`; the current host run selected llvmpipe and stopped at the zink+Arc renderer check, so it did not collect a post-change Zink miss count. `host_syncfd.sh` previously stopped with `FAIL no pdev`.

### Guest-side WSI v0 (T047, implemented)
- `native/vk-bridge/icd/wsi_wayland.c` (#included by icd.c): VK_KHR_surface/wayland_surface/swapchain live entirely in the guest ICD; host never sees WSI objects (VK_KHR_swapchain stripped from host CreateDevice).
- Swapchain image = bridged OPTIMAL VkImage + host-visible readback buffer (memfd shadow) + prebuilt copy cmdbuf; Acquire = round-robin + empty submit signalling sem/fence.
- vkQueuePresentKHR: submit(wait sems, copy) -> wait fence (server syncs shadow) -> memcpy into wl_shm pool (XRGB8888) -> attach/damage_buffer/commit/flush on a private wl_event_queue.
- libwayland-client dlopen'ed (no build dep). Proof: `test/host_wsi.sh` vkcube 120 frames on lodos-wl headless -> docs/screens/T047-wsi.png.
- Limits: 2 full copies/frame (GPU->shadow->shm), sync present, no resize/OUT_OF_DATE handling, fixed queue (first created). Next: AHB/dmabuf zero-copy on phone.
- **Zink GL on Wayland (T051)**: `test/host_zinkwsi.sh` runs `eglgears_wayland` (`MESA_LOADER_DRIVER_OVERRIDE=zink` -> Kopper) over `libvulkan_lodos.so` WSI -> `vkb_server` -> Intel Arc on `lodos-wl` headless 640x480 -> `docs/screens/T051-zink-wsi.png` (120+ frames, `GL_RENDERER` = `zink Vulkan 1.3(Intel(R) Arc(tm) A750 Graphics ...)`).
- Fixes landed: `vkb_server`/`rpcgen` overwrite device procs via `vkGetDeviceProcAddr` (+`KHR`/`EXT` fallback) instead of `dlsym`/GIPA VK1.4 core trampolines on 1.3 instances; `wsi_wayland.c` queues async `wl_display.sync` per present (waking `poll`+`wl_display_dispatch` loops via `delete_id`) and adds `IMMEDIATE` mode + `AcquireNextImage2KHR`/device-group WSI queries.
- Mesa Wayland EGL note: hardware/Kopper `dri2_initialize_wayland_drm` requires a DRM render-node fd from `zwp_linux_dmabuf_v1` default feedback (`LODOS_WL_DMABUF_DEV` added to `lodos-wl`); on `/dev/dri`-less Android `proot`, custom Mesa (`D10`) or nested compositor will bypass `dri2_setup_device`'s DRM node requirement.

### Existing Approaches Inspected
- **`xMeM/mesa` + `termux-x11` (`lorie`)**:
  - On X11 (`wsi_common_ahardware_buffer.c`), `xMeM/mesa` allocates per-swapchain `AHardwareBuffer`s, tags DRI3 `PixmapFromBuffers` with synthetic DRM modifier `1255`, passes a `socketpair` fd over X11, and transfers the `AHardwareBuffer` via Bionic `AHardwareBuffer_sendHandleToUnixSocket` (`libandroid.so`). `termux-x11` (`lorie/InitOutput.c`, `buffer.c`, `renderer.cpp`) receives the `AHardwareBuffer`, imports an `EGLImageKHR`, and composites onto Android `SurfaceView`. (`AHardwareBuffer_sendHandleToUnixSocket` uses Bionic parcel IPC and cannot be called from `glibc`.)
  - On Wayland (`wsi_common_wayland.c`), Mesa uses standard `zwp_linux_dmabuf_v1` (`dma_buf` fd passing).
- **Vortek + Winlator**: X11 `Window` ID over RPC -> single-buffered JNI `AHardwareBuffer` -> `EGLImageKHR` GLES blit. Single-buffered (`minImageCount = 1`) and X11-only.
- **`termux/proot` syscall behavior**:
  - `proot` (`src/syscall/socket.c`) translates `AF_UNIX` `sun_path` on `bind`/`connect` (creating short symlink aliases if host path `> 108` bytes — keep socket guest path short, e.g., `/tmp/.lodos/vk0`).
  - Crucially, `sendmsg`/`recvmsg` `SCM_RIGHTS` fd passing (`dma_buf`, `ashmem`, `memfd`, `sync_file`, `eventfd`), `mmap(MAP_SHARED | MAP_FIXED)` on received fds, and shared-memory `SYS_futex` wakes execute **directly in the Linux kernel with zero `proot` data-plane overhead**.

### LodOS Zero-Copy Wayland Present Architecture (`ai.md` D5 + D6)
Running the **Vulkan bridge server and the Rust `smithay` Wayland host in the same Android Bionic process** (`liblodos_host.so`) unlocks standard upstream Mesa Wayland WSI in the guest with true zero-copy Android scanout:

```
[Guest glibc App / Zink / nested phoc]
   │  upstream Mesa wsi_common_wayland.c (VK_KHR_wayland_surface)
   ├─► Vulkan IPC (SCM_RIGHTS): vkGetMemoryFdKHR(DMA_BUF) + SYNC_FD semaphores/fences
   │      backed on host by AHardwareBuffer (GPU_FRAMEBUFFER | GPU_SAMPLED_IMAGE | COMPOSER_OVERLAY)
   └─► Wayland socket (zwp_linux_dmabuf_v1 + explicit/implicit sync_fd): commits dma_buf fd
          │
          ▼
[Host Bionic Process: liblodos_host.so (vk-bridge + Smithay Wayland Host)]
   ├─► Looks up (dma_buf st_ino -> AHardwareBuffer*) in shared in-process table (0 copies, 0 Binder IPC)
   └─► Submits AHardwareBuffer* + acquire fence directly via ASurfaceTransaction_setBuffer()
       onto SurfaceView ASurfaceControl -> Android SurfaceFlinger / HWC overlay scanout
```

Key properties of this path:
1. **Unmodified guest WSI & compositor stack**: `vkcube-wayland`, Mesa Zink, GTK4/GNOME apps, `phoc` (`wlroots`), Xwayland, and Wine's Wayland driver all use standard `zwp_linux_dmabuf_v1` without custom X11/JNI swapchain hacks.
2. **True triple-buffered zero-copy**: Guest renders into `VkImage`s backed by multi-buffered `AHardwareBuffer`s (`minImageCount = 3`). In nested desktop mode, `phoc` composites client `dma_buf`s on Mali into `phoc`'s output `AHardwareBuffer` and commits it to Smithay -> `ASurfaceTransaction_setBuffer`. In fullscreen game mode (S4), Smithay can scan out the game's `AHardwareBuffer` directly to `ASurfaceControl` with zero intermediate composition blit.

---

## Recommendation

### Summary Decisions
1. **Must use IPC** (`guest glibc ICD` <-> `host Bionic service`); in-process `libhybris`-style loading is impossible on rootless Android 14/15 `aarch64` (`TPIDR_EL0` TLS conflict, APEX linker namespaces, `/dev/mali0` `kbase` kernel ABI).
2. **Why not Virtio-GPU Venus (`virglrenderer`) as primary**: While Debian 13 includes `libvulkan_virtio.so` and host `virglrenderer` could load `bionic-vulkan-wrapper`, standalone `vtest` copies command streams over Unix sockets (no shm ring without virtio kernel/VM), lacks native Android `AHardwareBuffer` scanout registry integration without invasive forks, and lacks `VK_EXT_map_memory_placed` (`MAP_FIXED`, required later for Wine/Box64 `wow64` in S4).
3. **Recommended Two-Layer Host + `vk.xml`-Generated Bridge (`lodos-vk-bridge`)**:
   - **Host Bottom Layer — Reuse `leegao/bionic-vulkan-wrapper` (~85% as-is, `MIT`)**: Build as an NDK shared library (`libvulkan_mali_layer.so`) wrapping `/system/lib64/libvulkan.so`. Remove Termux X11 DRI3 socket WSI; retain **GPU compute BC1–BC7 (`s3tc.comp`, `bc6.comp`, `bc7.comp`)**, **Mali `SPIRV-Tools` shader passes**, **`prefersDedicatedAllocation` clearing**, and **`dma_heap`/`AHardwareBuffer` exportable allocation**.
   - **IPC Bridge Layer — Clean C implementation (`ai.md` D6/D11) generated from Khronos `vk.xml`**: Rather than inheriting Vortek's skewed client/server snapshots, global lock, and 43.5k-LOC hand-edited serializer, write a compact Python `vk.xml` marshaller generator (~800 LOC) + C runtime (~2,500 LOC) borrowing Vortek's proven mechanics:
     - **Per-thread SPSC shared-memory rings** (`ASharedMemory_create` / `memfd_create`, 2–4 MB per active Vulkan thread + `SYS_futex` wakeups) — eliminating Vortek's global `vt_call_mutex`.
     - **Client-side `vkCmd*` command batching** flushed once at `vkEndCommandBuffer`.
     - **Complete `SCM_RIGHTS` external handle support**: `VK_EXT_external_memory_dma_buf` (`vkGetMemoryFdKHR` + `vkGetMemoryFdPropertiesKHR` via `stat()` inode tracking), `VK_EXT_image_drm_format_modifier` (`DRM_FORMAT_MOD_LINEAR`), `VK_KHR_external_semaphore_fd` & `VK_KHR_external_fence_fd` (`SYNC_FD`), and `VK_EXT_map_memory_placed` (`MAP_FIXED` `mmap` in guest).
     - **In-process `AHardwareBuffer` registry** (`lodos_vk_ahb_from_dmabuf_fd(int fd)`) linked directly into the Rust `smithay` host process for `ASurfaceTransaction_setBuffer` zero-copy present.
     - **Zink + Mali quirk table** ported from `vortekrenderer/src/vulkan_helper.c` and `shader_inspector.c` (filtering `extended_dynamic_state{,2,3}`, `color_write_enable`, `push_descriptor` for Zink where needed on Mali-G720).

### Reuse vs Rewrite Matrix & Effort Estimate

| Component | Disposition | Effort Estimate |
| :--- | :--- | :--- |
| `termux/proot` | **Reuse as-is** (configure bind-mount for `/tmp/.lodos/vk0` & Wayland socket `< 108` chars) | **0.5 days** |
| Guest Mesa (`zink` + `wsi_common_wayland.c`) | **Reuse Debian 13 `trixie` packages for S1**; custom Mesa build (`ai.md` D10) only in S4 if Mali/Zink tuning needed | **0.5 days** |
| `leegao/bionic-vulkan-wrapper` (`MIT`) | **Reuse ~85% on Android Bionic host** (strip X11 WSI, keep GPU BC1–BC7 compute + Mali SPIR-V/VMA fixes + AHB/dma_heap) | **2–3 days** |
| `lodos-vk-bridge` (`vk.xml` codegen + C IPC + AHB registry) | **Write clean** (using Vortek's command-batching, shm ring, and Zink/Mali filter patterns as reference) | **5–6 days** (S1 MVP: Vulkan 1.2/1.3 core + `dma_buf`/`SYNC_FD` for `vkcube` & Zink) |
| Gaming extensions (S4: `map_memory_placed`, async pipelines, DXVK/VKD3D hardening) | **Incremental extension** on `lodos-vk-bridge` + `bionic-vulkan-wrapper` | **+5–7 days** (in S4) |

**Total S1 GPU Bridge Effort**: **~8–10 engineer-days** to reach hardware-accelerated `vkcube-wayland` and Mesa Zink (`glmark2-es2-wayland` / `phoc`) with zero-copy `SurfaceControl` scanout on POCO X7 Pro.

---

## Next tasks

- **Analyze `T004` (`vkprobe`) telemetry from POCO X7 Pro (Mali-G720)**: verify exact vendor `apiVersion`, `driverVersion`, `VK_ANDROID_external_memory_android_hardware_buffer`, `VK_EXT_external_memory_dma_buf`, `VK_KHR_external_semaphore_fd` (`SYNC_FD`), unrooted `/dev/dma_heap/system*` access, and supported `AHardwareBuffer` usage/format combinations (`R8G8B8A8_UNORM`, `B8G8R8A8_UNORM`, `COMPOSER_OVERLAY`).
- **Extract and build `host/vk-mali-layer` (NDK Bionic `.so`) from `leegao/bionic-vulkan-wrapper`**: strip X11/Termux DRI3 code, compile `s3tc.comp`/`bc6.comp`/`bc7.comp` + `SPIRV-Tools` Mali passes + `prefersDedicatedAllocation` fix + `AHardwareBuffer`/`dma_heap` export allocator with Android NDK CMake/Meson, and test standalone on phone via a native Bionic compute/offscreen smoke test.
- **Implement `vk.xml` RPC code generator and `lodos-vk-bridge` core (`guest/vulkan-icd` + `host/vk-bridge`)**: per-thread `ASharedMemory`/`memfd` SPSC futex rings, client-side `vkCmd*` command-buffer batching, handle translation tables, and `SCM_RIGHTS` fd transfer over `/tmp/.lodos/vk0`.
- **Implement `AHardwareBuffer`-backed `dma_buf` + `SYNC_FD` WSI bridge and in-process Smithay registry**: support `VK_EXT_external_memory_dma_buf`, `VK_EXT_image_drm_format_modifier` (`DRM_FORMAT_MOD_LINEAR`), `vkGetMemoryFdKHR`/`vkGetMemoryFdPropertiesKHR`, semaphore/fence `SYNC_FD` import/export, and `lodos_vk_ahb_from_dmabuf_fd()` -> `ASurfaceTransaction_setBuffer` in the Android Wayland host.
- **Bring up S1 guest Vulkan + Zink inside rootless `proot` Debian 13 on POCO X7 Pro**: register `lodos_icd.aarch64.json`, verify `vulkaninfo` and `vkcube-wayland` (>= 60 fps zero-copy), then enable Zink (`MESA_LOADER_DRIVER_OVERRIDE=zink`) with Mali extension filters and verify `glmark2-es2-wayland` and nested `phoc`.
