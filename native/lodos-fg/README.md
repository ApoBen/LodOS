# lodos-fg v0

Standalone Vulkan compute core for FSR1 spatial scaling. `lodos_fsr1_record` records the upstream AMD EASU pass and then RCAS into caller-owned RGBA8 images already in `VK_IMAGE_LAYOUT_GENERAL`. It owns compute pipelines, descriptor sets, and sampler. Callers own image allocation, synchronization before/after the call, and output presentation. It does not hook a swapchain.

The EASU and RCAS algorithms and portability headers are from [GPUOpen-Effects/FidelityFX-FSR](https://github.com/GPUOpen-Effects/FidelityFX-FSR), pinned at `a21ffb8f6c13233ba336352bdff293894c706575`; MIT license is in `third_party/amd-fsr1/LICENSE`. Shader source is included unchanged in meaning, with Vulkan GLSL callbacks and bindings for combined sampled images and storage outputs. The v0 shader path uses FP32 and RGBA8 UNORM.

Reuse check: vkBasalt (`https://github.com/DadSchoorse/vkBasalt`, inspected at `4f97f09ffe91900e6ca136cc26cf7966f8f6970d`) provides a Vulkan post-processing framework and configurable CAS sharpening pass. Its framework targets post-process effects and does not provide an FSR1 EASU+RCAS two-pass scaler suitable as this standalone compute API. Its CAS configuration informed the exposed sharpness parameter; no vkBasalt code was copied.

| Preset | Input-to-output scale | Example for 1920x1080 output |
|---|---:|---:|
| quality | 1.5x | 1280x720 |
| balanced | 1.7x | 1129x635 |
| performance | 2.0x | 960x540 |

Build with CMake, Vulkan development files, `glslangValidator`, and libpng. The offscreen executable is a headless Vulkan proof and writes `docs/screens/T068-fsr1.png`.

## layer usage

`VK_LAYER_LODOS_fg` is built beside `liblodos_fg.so`. Enable it with `LODOS_FG=1`, `VK_LAYER_PATH` set to the build directory, and `VK_INSTANCE_LAYERS=VK_LAYER_LODOS_fg`. `LODOS_FG_PRESET` accepts quality (1.5x), balanced (1.7x), or performance (2x); `LODOS_FG_SHARPNESS` defaults to 0.2. `LODOS_FG_DUMP` and `LODOS_FG_DUMP_FRAME` select a diagnostic dump and frame number. The dispatch and loader-chain pattern follows Mesa's Vulkan vram-report-limit layer (MIT, `third_party/src/mesa-kopper/src/vulkan/vram-report-limit-layer`); no Mesa source is copied.
