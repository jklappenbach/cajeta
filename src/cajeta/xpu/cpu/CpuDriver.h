//
// CPU "driver" — launch a registered CPU kernel by name over a grid
// (cajeta-cpu.md Increment 3, serial).
//
// The CPU twin of CudaDriver/HipDriver/VulkanDriver, but trivially simple: a CPU
// kernel is host code already linked into the program, so there is no library to
// dlopen, no device to select, and no memory to transfer — buffers ARE host
// pointers. The driver resolves the kernel's registered launcher thunk by name
// (__cajeta_xpu_lookup_cpu_kernel) and runs the grid→threads loop, calling the
// thunk once per work-item with the kernelParams argv (shared) and that
// work-item's coordinate vector.
//
// `available()` is always true: the CPU is the guaranteed terminal of the
// backend priority chain (CUDA→HIP→Vulkan→CPU) — "degrade to CPU" is just
// dispatch reaching its always-present last link.
//
// Execution is serial in this increment; multi-core threading and true
// wave=SIMD-lane vectorization are Increment 5.
//

#pragma once

#include <cstdint>

namespace cajeta {
namespace xpu {
namespace cpu {

    // The uniform launcher-thunk signature emitted by CpuRegistration:
    // argv is the kernelParams array (argv[i] → &arg_i value), coord is the
    // per-work-item i32[9] = {tid.xyz, ctaid.xyz, ntid.xyz}.
    using CpuLaunchFn = void (*)(void** argv, const std::int32_t* coord);

    class CpuDriver {
    public:
        // The CPU is always present — no probe, no device.
        static bool available() { return true; }

        // Launch the kernel registered as `name` over a grid of `gridX×gridY×
        // gridZ` blocks, each `blockX×blockY×blockZ` work-items. `argv` is the
        // kernelParams array (cuLaunch/hipModuleLaunch convention): argv[i]
        // points to the i-th kernel argument's value, shared across work-items.
        // Returns false iff no kernel is registered under `name` (the precise
        // "no such kernel" contract); true once every work-item has run.
        bool launch(const char* name, void** argv,
                    unsigned gridX, unsigned blockX,
                    unsigned gridY = 1, unsigned blockY = 1,
                    unsigned gridZ = 1, unsigned blockZ = 1);
    };

} // namespace cpu
} // namespace xpu
} // namespace cajeta
