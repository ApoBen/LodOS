# vk-bridge work path notes

Mapped allocations use a client memfd shadow mapping. The guest ICD passes the fd to the server with SCM_RIGHTS; the server keeps the real Vulkan allocation mapped and copies bytes between the shared shadow and the driver mapping on map/unmap, flush/invalidate, submit, and idle/wait synchronization. This works across the guest glibc and Android bionic boundary without sharing process pointers. It is a copy-based compatibility path, not zero-copy Mali memory import.

## cmd batching

By default the client records each generated `vkCmd*` request into a per-command-buffer stream. `vkBeginCommandBuffer` resets it, and `vkEndCommandBuffer` appends the end request and sends one `VKB_CMD_STREAM` RPC. The server replays each length-prefixed request through the normal generated dispatcher, preserving the existing codecs and handle translation. Set `VKB_NO_BATCH=1` to use the original per-call RPC path. The host triangle fixture reports server RPC counts for both modes.

The stream uses the existing ring message size limit; oversized command buffers currently return `VK_ERROR_OUT_OF_HOST_MEMORY` at end rather than being split. Commands used outside a begin/end recording interval are still individually forwarded in batch mode.

The test programs cover a basic compute dispatch and classic render pass with image-to-buffer readback. `vkcube` still needs WSI/swapchain presentation, acquire/present synchronization, and broader extension coverage. Zink additionally needs broad Vulkan 1.3/1.4 command/extension fidelity, descriptor/pipeline edge cases, and lower RPC overhead; this work does not establish Zink compatibility.

The current compute fixture is inherited from the host proof and doubles 1,048,576 `uint32_t` values. It does not yet satisfy the requested float-add shader workload.
