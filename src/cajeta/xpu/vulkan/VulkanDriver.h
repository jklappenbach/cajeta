//
// Minimal Vulkan compute runtime wrapper — dlopen'd libvulkan, no build-time
// Vulkan link dependency.
//
// The Vulkan analog of HipDriver / CudaDriver (the Driver seam), but the launch
// model forks hard: where CUDA/HIP take a flat kernelParams array and a raw
// module, Vulkan needs instance → device → descriptor-set layout + pool →
// compute pipeline → command buffer → vkCmdDispatch. Buffers are bound as
// descriptor-set STORAGE_BUFFERs (set 0, binding = arg index), matching the
// descriptor-set SPIR-V the SpirvTarget emits.
//
// Uses <vulkan/vulkan.h> for the struct/enum definitions (guarded by
// __has_include so a box without the Vulkan SDK still builds — available()
// just returns false there), but resolves every entry point via dlopen +
// vkGetInstanceProcAddr/vkGetDeviceProcAddr, so there is no link-time libvulkan
// dependency. A box without a Vulkan ICD simply has no compute device — the
// binary still links and runs.
//
// Scope: load a SPIR-V module and run a 1-D compute dispatch over host-visible
// storage buffers (the SAXPY end-to-end path). Broader surface layers on later.
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cajeta {
namespace xpu {
namespace vulkan {

    class VulkanDriver {
    public:
        // Opaque storage-buffer handle (index into the driver's table; 0 is
        // invalid). Each is a host-visible, host-coherent VkBuffer.
        using Buffer = std::uint32_t;

        // True iff libvulkan is loadable, an instance can be created, and at
        // least one physical device with a compute queue exists. Safe to call
        // without a prior init() (constructs + tears down a throwaway driver).
        static bool available();

        VulkanDriver() = default;
        ~VulkanDriver();
        VulkanDriver(const VulkanDriver&) = delete;
        VulkanDriver& operator=(const VulkanDriver&) = delete;

        // Resolve libvulkan, create instance/device, pick a compute queue.
        // Idempotent; returns false if no usable Vulkan compute device.
        bool init();

        // Host-visible coherent storage buffer of `bytes`. Returns 0 on failure.
        Buffer alloc(std::size_t bytes);
        bool upload(Buffer b, const void* src, std::size_t bytes);
        bool download(void* dst, Buffer b, std::size_t bytes);
        void free(Buffer b);

        // Load `spirv` (len bytes), build a compute pipeline for entry point
        // `entry`, bind `bindings` as descriptor-set 0 STORAGE_BUFFERs (binding
        // i = bindings[i]), and dispatch `groupCountX` workgroups. Synchronous
        // (waits for the queue to idle). Returns false on any Vulkan error.
        bool launch(const void* spirv, std::size_t len, const char* entry,
                    const std::vector<Buffer>& bindings, unsigned groupCountX);

    private:
        struct Impl;
        Impl* impl = nullptr;
    };

} // namespace vulkan
} // namespace xpu
} // namespace cajeta
