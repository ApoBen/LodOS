# Mali-G720 MC7 Vulkan (POCO X7 Pro, vendor driver) — vkprobe summary
Source: docs/research/mali-g720-vk.json (tools/vkprobe, run via adb shell 2026-11-26).
- API 1.3.278, driver v1.r49p1 (driverID 9 ARM proprietary), conformance 1.3.8.0, vendor 0x13b5, 150 device exts, 1 queue family (gfx+compute+transfer+protected, x2).
- Memory: one 11.0 GiB device-local+host-visible heap (UMA) + 100 MiB protected heap. maxImageDimension2D 16384, push consts 256, maxMemoryAllocationCount 16384, maxBoundDescriptorSets 7, maxViewports 1, compute shared mem 32 KiB.
- Present: geometryShader, tessellationShader, logicOp, depthClamp, dualSrcBlend, independentBlend, imageCubeArray, samplerAnisotropy, int64/int16, ETC2+ASTC LDR/HDR, all 1.1/1.2/1.3 core features (bufferDeviceAddress, timelineSemaphore, descriptorIndexing, dynamicRendering, sync2), transform_feedback, custom_border_color, extended_dynamic_state 1/2/3, vertex_input_dynamic_state, line_rasterization, provoking_vertex, depth_clip_enable, AHB + dma_buf + drm_format_modifier + external fd (good for D6 bridge / zero-copy), ray query.
- Depth/stencil: D24S8, D32S8, D16, X8_D24 all supported (optimal 0x1de01).
## MISSING (what DXVK / zink will miss)
- textureCompressionBC = false; all BC1..BC7 formats 0x0 -> R2 confirmed: BCn->RGBA/ASTC transcode needed (DXVK/zink/wine games).
- VK_EXT_robustness2 absent (robustBufferAccess2/nullDescriptor) -> DXVK 2.x hard-requires it; needs patch/emulation in bridge or DXVK fork.
- shaderClipDistance/shaderCullDistance = false -> DXVK & zink (GL 3.0+ gl_ClipDistance) need shader lowering (clip via discard / extra varyings).
- multiViewport = false, maxViewports 1 -> DXVK D3D11 and GL 4.1 ARB_viewport_array limited.
- fillModeNonSolid = false (wireframe/glPolygonMode), depthBounds, pipelineStatisticsQuery, shaderFloat64 (GL 4.0 fp64 -> softfp64), sparse* (no sparse textures/D3D tiled resources), vertexPipelineStoresAndAtomics, shaderStorageImageMultisample, variableMultisampleRate, alphaToOne, shaderTessellationAndGeometryPointSize, multiview GS/TS.
- Extensions not exposed: VK_EXT_graphics_pipeline_library (DXVK shader-compile stutter), VK_EXT_descriptor_buffer, VK_EXT_shader_stencil_export, VK_EXT_memory_priority/pageable_device_local_memory, VK_EXT_non_seamless_cube_map, VK_EXT_attachment_feedback_loop_layout, VK_EXT_conditional_rendering.
- Verdict: zink GL likely capped ~GL 3.x/ES 3.2 without lowering patches; DXVK needs robustness2 + clip/cull + BCn workarounds (Vortek-style shims) — plan in S1.
