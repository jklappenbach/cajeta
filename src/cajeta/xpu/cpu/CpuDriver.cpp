//
// CpuDriver — see header. The serial grid→threads loop over a registered
// launcher thunk.
//

#include "CpuDriver.h"

// The runtime CPU kernel registry (runtime/native/cajeta_runtime.c), linked into
// the program via cajeta_lib. Returns the launcher-thunk pointer, or null.
extern "C" void* __cajeta_xpu_lookup_cpu_kernel(const char* name);

namespace cajeta {
namespace xpu {
namespace cpu {

    bool CpuDriver::launch(const char* name, void** argv,
                           unsigned gridX, unsigned blockX,
                           unsigned gridY, unsigned blockY,
                           unsigned gridZ, unsigned blockZ) {
        void* fnptr = __cajeta_xpu_lookup_cpu_kernel(name);
        if (!fnptr) return false;   // no kernel registered under `name`
        auto fn = reinterpret_cast<CpuLaunchFn>(fnptr);

        // The launcher thunk is the per-BLOCK wrapper (Inc 5B) — it loops the
        // block's work-items internally (vectorized). So we call it once per
        // BLOCK, setting only ctaid + ntid; tid is the wrapper's loop var.
        // coord layout: [tid.xyz (unused here), ctaid.xyz, ntid.xyz].
        std::int32_t coord[9] = {0, 0, 0, 0, 0, 0,
                                 static_cast<std::int32_t>(blockX),
                                 static_cast<std::int32_t>(blockY),
                                 static_cast<std::int32_t>(blockZ)};
        for (unsigned cz = 0; cz < gridZ; ++cz) {
            coord[5] = static_cast<std::int32_t>(cz);   // ctaid.z
            for (unsigned cy = 0; cy < gridY; ++cy) {
                coord[4] = static_cast<std::int32_t>(cy);   // ctaid.y
                for (unsigned cx = 0; cx < gridX; ++cx) {
                    coord[3] = static_cast<std::int32_t>(cx);   // ctaid.x
                    fn(argv, coord);   // per-block; wrapper loops work-items
                }
            }
        }
        return true;
    }

} // namespace cpu
} // namespace xpu
} // namespace cajeta
