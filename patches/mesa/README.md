# Mesa patch

- Upstream: https://gitlab.freedesktop.org/mesa/mesa
- Pinned tag: `mesa-26.2.5`
- Pinned commit: `ddaf85e353442d9d7f192fdcf1823e652e3f2ee3` (peeled tag commit)
- License: MIT (Mesa)
- Reuse reference: https://github.com/localdesktop/localdesktop.github.io/tree/ac6295674cc0a2c437f80d71aab9995521274d28/patches/build-mesa/0001-egl-wayland-zink-kopper-without-render-node.patch (Local Desktop, GPL-3.0-or-later; patch logic adapted)

## 0001-egl-wayland-zink-kopper-without-render-node.patch

- What: `LIBGL_KOPPER_DRI2=1` selects Wayland's swrast/Kopper loader path for forced Zink without requiring a DRM render node; the same no-node EGLDevice handling is applied to X11 and surfaceless initialization.
- Why: Mesa's Wayland DRM path requires render-node feedback. In a rootless guest, the Vulkan ICD is reachable while `/dev/dri` is absent, so the standard EGL initialization falls back or fails before Zink Kopper can use its Vulkan WSI.
- Reproduce: run `MESA_LOADER_DRIVER_OVERRIDE=zink LIBGL_KOPPER_DRI2=1 EGL_PLATFORM=wayland eglinfo -B -p wayland` against a Wayland compositor that advertises no `zwp_linux_dmabuf_v1` feedback, with a functioning Vulkan ICD.
- Tested: pending PC acceptance run via `native/vk-bridge/test/host_zink_norender.sh`.
- Upstreamable: yes, candidate. This adds an opt-in path equivalent to the established X11 `LIBGL_KOPPER_DRI2` switch for clients backed by a Vulkan device with no DRM node.
