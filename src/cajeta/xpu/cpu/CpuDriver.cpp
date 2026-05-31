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

        // Block dim (ntid) is constant across the launch; coordinate layout is
        // [tid.xyz, ctaid.xyz, ntid.xyz]. We iterate blocks × work-items and call
        // the thunk per work-item, varying only tid + ctaid.
        std::int32_t coord[9];
        coord[6] = static_cast<std::int32_t>(blockX);   // ntid.x
        coord[7] = static_cast<std::int32_t>(blockY);   // ntid.y
        coord[8] = static_cast<std::int32_t>(blockZ);   // ntid.z

        for (unsigned cz = 0; cz < gridZ; ++cz) {
            coord[5] = static_cast<std::int32_t>(cz);   // ctaid.z
            for (unsigned cy = 0; cy < gridY; ++cy) {
                coord[4] = static_cast<std::int32_t>(cy);   // ctaid.y
                for (unsigned cx = 0; cx < gridX; ++cx) {
                    coord[3] = static_cast<std::int32_t>(cx);   // ctaid.x
                    for (unsigned tz = 0; tz < blockZ; ++tz) {
                        coord[2] = static_cast<std::int32_t>(tz);   // tid.z
                        for (unsigned ty = 0; ty < blockY; ++ty) {
                            coord[1] = static_cast<std::int32_t>(ty); // tid.y
                            for (unsigned tx = 0; tx < blockX; ++tx) {
                                coord[0] = static_cast<std::int32_t>(tx); // tid.x
                                fn(argv, coord);
                            }
                        }
                    }
                }
            }
        }
        return true;
    }

} // namespace cpu
} // namespace xpu
} // namespace cajeta
