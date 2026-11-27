# S1: Mesa Kopper on Wayland without a DRM render node

## Finding

Mesa EGL's Wayland backend normally enters `dri2_initialize_wayland_drm()`. That path reads `zwp_linux_dmabuf_v1` feedback, selects a render device with `loader_get_user_preferred_fd()`, then builds EGL's driver/device state from that DRM fd. A Wayland host with neither `wl_drm` nor dma-buf feedback cannot provide that fd. The later `dri2_detect_swrast_kopper()` call does not bypass this earlier device selection.

Mesa has a Wayland swrast initializer (`dri2_initialize_wayland_swrast`) and Kopper loader callbacks for Zink. The existing `LIBGL_KOPPER_DRI2` opt-in applies to the X11 DRI3-vs-Kopper choice; Mesa documentation describes it in terms of X11 modifiers, and the upstream Wayland initializer does not consult it. `EGL_PLATFORM=wayland` only selects the platform; it does not select the software/Kopper loader. `LIBGL_ALWAYS_SOFTWARE=1` forces a software renderer, not Zink on the guest Vulkan ICD.

The change carries the same opt-in into Wayland and skips the DRM-backed EGLDevice description for this forced Vulkan-backed Zink path. The X11 and surfaceless changes make the private stack consistent with the same no-DRM-node mode. Kopper creates the Vulkan swapchain through our ICD's Wayland WSI; T047's ICD reads back and commits via `wl_shm`, so the compositor need not import dmabufs.

## Upstream/reuse check

Mesa release notes document `LIBGL_KOPPER_DRI2` handling for X11, while Wayland initialization still dispatches to the DRM path unless software rendering is forced. Local Desktop carries a patch for this exact missing Wayland switch, based on Mesa 26.2.5. Its behavior and source are recorded in `patches/mesa/README.md`; LodOS adapts it to a PC Zink-only build. The Mesa patch is marked as an upstream candidate in `UPSTREAM.md`.

## Source references

- Mesa source, pinned tag `mesa-26.2.5`, commit `ddaf85e353442d9d7f192fdcf1823e652e3f2ee3`: `src/egl/drivers/dri2/platform_wayland.c`, `platform_surfaceless.c`, `platform_x11.c`.
- Mesa EGL Wayland source: https://cgit.freedesktop.org/mesa/mesa/tree/src/egl/drivers/dri2/platform_wayland.c
- Mesa environment variables: https://docs.mesa3d.org/envvars.html (`LIBGL_KOPPER_DRI2`).
- Mesa 25.2 release notes: https://docs.mesa3d.org/relnotes/25.2.0.html (X11 Kopper DRI2 changes and Wayland EGL path changes).
- Reused implementation: https://github.com/localdesktop/localdesktop.github.io/tree/ac6295674cc0a2c437f80d71aab9995521274d28/patches/build-mesa/.

## PC proof

`native/vk-bridge/test/host_zink_norender.sh` builds Mesa out of tree under `build/mesa-pc`, starts lodos-wl with `LODOS_WL_NO_DMABUF=1`, runs `eglgears_wayland` through Zink and the LodOS ICD, checks present/frame output, and writes `docs/screens/T067-eglgears-norender.png`. The test prints `NORENDER ok` on completion. Local build verification passed; on this runner, Vulkan enumeration reaches the LodOS ICD but Zink does not select a pdev, so the screenshot portion remains unverified.
