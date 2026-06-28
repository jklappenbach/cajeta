//
// DeviceModel — turn queryable GPU properties + a kernel's measured resource
// demand into a PRUNED, ORDERED candidate set for the autotune fallback
// (kernel-occupancy-autotune §4, smart fallback). When there is no shipped
// guidance, this replaces a blind grid sweep with a machine-model search:
//   - prune: drop any block size that cannot fit the register / LDS / wave
//            budgets (zero measurement — pure arithmetic);
//   - order: by predicted occupancy (resident waves/CU), the conventional prior
//            for an unknown kernel; the empirical pass then settles the tail.
//
// Honest scope: the occupancy model and the LINEAR conflict-free LDS pad are
// derivable from properties. The WMMA-fragment conflict-free pad depends on the
// instruction's fixed lane->address map and is NOT the textbook coprime formula
// (that mismatch is what burned the hand-tuning) — it is deferred, not faked.
//

#pragma once

#include <cstdint>
#include <vector>

namespace cajeta {
namespace xpu {

    // Machine parameters. Defaults describe gfx1151 (RDNA3.5, Strix Halo); a
    // caller fills these from rocminfo / hipDeviceProp for another GPU.
    struct DeviceModel {
        unsigned waveSize          = 32;     // hipDeviceProp.warpSize
        unsigned maxThreadsPerBlock = 1024;  // hipDeviceProp.maxThreadsPerBlock
        unsigned maxWavesPerCU     = 32;     // rocminfo "Max Waves Per CU"
        unsigned simdsPerCU        = 2;      // rocminfo "SIMDs per CU"
        unsigned vgprFilePerSIMD   = 1536;   // RDNA3 wave32 VGPR file per SIMD
        unsigned maxWavesPerSIMD   = 16;     // RDNA3 wave32 cap
        unsigned ldsBytesPerCU     = 65536;  // hipDeviceProp.sharedMemPerBlock (64 KB)
        unsigned ldsBankCount      = 32;     // RDNA LDS banks
        unsigned ldsBankWidth      = 4;      // bytes per bank

        // Resident waves per CU for `block` threads, given the kernel's compiled
        // per-thread VGPR demand and per-workgroup LDS usage (bytes). The closed
        // form: min over the register, LDS, and hardware-wave limiters. 0 means
        // the config does not fit (→ pruned).
        unsigned occupancy(unsigned block, unsigned kernelVgpr,
                           unsigned ldsBytes) const;

        // The feasible block sizes (wave multiples ≤ maxThreadsPerBlock that fit
        // the budgets), ordered best-first by predicted occupancy. `kernelVgpr`
        // and `ldsBytes` are the compiled kernel's demand; `clamp` (0 = none) caps
        // the block (the §2 / @Occupancy bound). Empty if nothing fits.
        std::vector<unsigned> candidateBlocks(unsigned kernelVgpr,
                                              unsigned ldsBytes,
                                              unsigned clamp = 0) const;
    };

    // Smallest row pad (in elements) that makes a row of `rowElems` elements of
    // `elemBytes` each occupy a whole, bank-coprime number of LDS banks — so
    // consecutive rows land on distinct banks (conflict-free) for LINEAR /
    // coalesced access. Derivable purely from the bank geometry. (For gfx1151,
    // a 64-element f16 row yields pad 2 → stride 66, matching the measured
    // conflict-light layout.)
    unsigned conflictFreePad(unsigned rowElems, unsigned elemBytes,
                             unsigned bankCount = 32, unsigned bankWidth = 4);

} // namespace xpu
} // namespace cajeta
