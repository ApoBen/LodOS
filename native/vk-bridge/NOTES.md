# vk-bridge work path notes

Mapped allocations use a client memfd shadow mapping. The guest ICD passes the fd to the server with SCM_RIGHTS; the server keeps the real Vulkan allocation mapped and copies bytes between the shared shadow and the driver mapping on map/unmap, flush/invalidate, submit, and idle/wait synchronization. This works across the guest glibc and Android bionic boundary without sharing process pointers. It is a copy-based compatibility path, not zero-copy Mali memory import.

Command buffers are forwarded to the host driver while the guest records them. Each `vkCmd*` call is currently an RPC round trip; batching encoded commands into one request at submit time is not implemented and remains a performance/latency gap.

The test programs cover a basic compute dispatch and classic render pass with image-to-buffer readback. `vkcube` still needs WSI/swapchain presentation, acquire/present synchronization, and broader extension coverage. Zink additionally needs broad Vulkan 1.3/1.4 command/extension fidelity, descriptor/pipeline edge cases, and lower RPC overhead; this work does not establish Zink compatibility.

The current compute fixture is inherited from the host proof and doubles 1,048,576 `uint32_t` values. It does not yet satisfy the requested float-add shader workload.
