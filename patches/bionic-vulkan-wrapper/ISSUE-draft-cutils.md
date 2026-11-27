Title: Wrapper ICD links libcutils.so (non-NDK), so it cannot be loaded from a regular Android app process

Hi, thanks for this project. We use the wrapper in LodOS (rootless Linux on Android, https://github.com/ApoBen/LodOS) on a Mali-G720 device (Dimensity 8400, Android 15).

**Problem**
`libvulkan_wrapper.so` gets `DT_NEEDED: libcutils.so` through `src/android_stub` (`lib_names` includes `cutils`). libcutils is not part of the NDK, so since Android 7 linker namespaces stop a normal app (untrusted_app) from loading it. dlopen of the wrapper then fails inside an APK, even though it works from `adb shell`.

**What we changed (commit 3abf487, current `wrapper` HEAD)**
1. Removed `cutils` from the android_stub `lib_names` and added a small static `cutils_local.c` with the few symbols Mesa uses: `property_get` (on top of `__system_property_get`) and no-op `atrace_*` stubs, with tracing reported as disabled.
2. Optional: added a linker version script so the .so exports only `vk_icd*` symbols, plus `--exclude-libs` for the static SPIRV-Tools/libc++ archives. This keeps the wrapper's internal symbols from clashing with other libraries in the same process.

After this, `readelf -d` lists only NDK libraries as NEEDED. We are still testing it inside our app and will add results here.

Patch: https://github.com/ApoBen/LodOS/blob/master/patches/bionic-vulkan-wrapper/0002-lodos-no-cutils-icd-exports.patch

Would you be interested in a PR? We can split it into (1) the cutils removal and (2) the export map, and adjust them to your preferred style.
