// CPU "driver" — launch a registered CPU kernel by name over a grid. A CPU
// kernel is host code already linked in, so there is no library to dlopen, no
// device to select and no transfer: buffers ARE host pointers. Serial.

#pragma once

#include <cstdint>

namespace cajeta {
namespace xpu {
namespace cpu {

    // The launcher-thunk signature emitted by CpuRegistration. argv[i] points
    // at arg_i's value; coord is the per-work-item i32[13] =
    // {tid.xyz, ctaid.xyz, ntid.xyz, nctaid.xyz, dynSharedBytes}.
    using CpuLaunchFn = void (*)(void** argv, const std::int32_t* coord);

    class CpuDriver {
    public:
        static bool available() { return true; }

        // Run the kernel registered as `name` over gridX×gridY×gridZ blocks of
        // blockX×blockY×blockZ work-items, `argv` shared across all of them.
        // Returns false iff no kernel is registered under `name`.
        bool launch(const char* name, void** argv,
                    unsigned gridX, unsigned blockX,
                    unsigned gridY = 1, unsigned blockY = 1,
                    unsigned gridZ = 1, unsigned blockZ = 1);
    };

} // namespace cpu
} // namespace xpu
} // namespace cajeta
