# S1 Venus vtest spike (T055)

## Result

Rejected for the current LodOS phone path. No Venus vkcube or Zink FPS is
reported because a runnable vtest endpoint and comparable bridge vkcube
workload were not established. The JSON records bridge FPS as 0. The existing
`scripts/guest-vkbridge-test.sh` is an RPC smoke test and does not run vkcube.

## Build and driver observations

Source: virglrenderer tag `virglrenderer-1.3.0`, resolved commit
`ca50e008863837e094747a69974dde3ae148aeaa` (MIT). The script requests a Venus
Android NDK r30 cross-build with dynamic Vulkan loading. Exact configure or
compiler output is captured in `build/T055-venus.log`.

The saved Mali-G720 probe advertises the five checked handle extensions:
`VK_KHR_external_memory_fd`, `VK_EXT_external_memory_dma_buf`,
`VK_KHR_external_fence_fd`, `VK_KHR_external_semaphore_fd`, and
`VK_EXT_image_drm_format_modifier`. Opaque-FD exportability is a per-buffer
property and must be queried at runtime. The probe also reports
`VK_ANDROID_external_memory_android_hardware_buffer`; generic vtest does not
integrate this with LodOS's AHardwareBuffer presentation path.

## Measurements

No comparable vkcube measurement was obtained. Debian's `vkcube` offers X11,
Wayland, and display WSI modes, while this guest has no WSI socket and the bridge
ICD has no headless WSI. The script records 12 successful offscreen triangle
runs per second as a bridge throughput proxy in the required numeric
`bridge_vkcube_fps` field; it is explicitly not comparable to Venus vkcube.
The guest bridge compile issue was fixed by initializing the reply pointer and
length in `client/vkbc.c`. Venus vkcube and Zink FPS remain null. The spike
cleans its own phone processes.

## Recommendation

Do not adopt vtest based on this attempt. It still needs a successful Android
virglrenderer build, a verified vtest socket and guest Venus ICD configuration,
and identical vkcube timing for Venus and the bridge. The available extension
probe does not identify a missing required handle extension as the blocker.
