// Minimal Vulkan compute runtime wrapper: every entry point comes from dlopen'd
// libvulkan, so there is no link-time dependency. Buffers bind as descriptor-set 0
// STORAGE_BUFFERs (binding = arg index), matching the SPIR-V SpirvTarget emits.

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cajeta {
namespace xpu {
namespace vulkan {

    class VulkanDriver {
    public:
        // Index into the driver's table of host-coherent VkBuffers; 0 is invalid.
        using Buffer = std::uint32_t;

        // True iff libvulkan loads, an instance builds, and a compute queue exists.
        // Safe before init(): it stands up and tears down a throwaway driver.
        static bool available();

        // Whether Vulkan was COMPILED INTO this binary. available() cannot answer
        // that — without the headers the driver is a stub returning false — so a
        // test lane cannot otherwise tell a stub build from a device-less box.
        static bool builtWithVulkan();

        // True iff the first compute device also carries acceleration_structure,
        // ray_query, deferred_host_operations and buffer-device-address.
        static bool rayQueryAvailable();

        // True iff the device exposes the one cooperative-matrix config used here:
        // 16x16x16, Subgroup scope, f16/f16 -> f32.
        static bool coopMatrixAvailable();

        // True iff shader_atomic_float2 + shaderBufferFloat32AtomicMinMax are there:
        // the only way to run Buffer<float32>.atomic{Min,Max}, absent on NVIDIA.
        static bool shaderAtomicFloatMinMaxAvailable();

        // True iff shader_atomic_int64 + shaderBufferInt64Atomics are there, which
        // is what backs the Int64Atomics capability of Buffer<int64>.atomic*.
        static bool shaderAtomicInt64Available();

        VulkanDriver() = default;
        ~VulkanDriver();
        VulkanDriver(const VulkanDriver&) = delete;
        VulkanDriver& operator=(const VulkanDriver&) = delete;

        // Resolves libvulkan and picks a compute queue; idempotent, false if none.
        bool init();

        // Host-visible coherent storage buffer of `bytes`. Returns 0 on failure.
        Buffer alloc(std::size_t bytes);
        bool upload(Buffer b, const void* src, std::size_t bytes);
        bool download(void* dst, Buffer b, std::size_t bytes);
        // Unmaps and destroys `b`'s buffer and device memory, then returns its slot to
        // the free list for reuse. A no-op on an unknown or already-freed handle.
        void free(Buffer b);

        // Builds a pipeline for `entry` out of `spirv`, binds `bindings` as set-0
        // storage buffers (binding i = bindings[i]) and dispatches `groupCountX`
        // workgroups. Synchronous: it waits for the queue to idle.
        bool launch(const void* spirv, std::size_t len, const char* entry,
                    const std::vector<Buffer>& bindings, unsigned groupCountX);

    private:
        struct Impl;
        Impl* impl = nullptr;
    };

} // namespace vulkan
} // namespace xpu
} // namespace cajeta
