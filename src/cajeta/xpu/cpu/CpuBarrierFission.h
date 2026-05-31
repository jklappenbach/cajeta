//
// CPU work-item loop fission for workgroup barriers (CajetaXPU Increment 6).
//
// A `@Kernel` that calls `Barrier.workgroup()` cannot run as a single work-item
// loop on the CPU: a barrier means every work-item of the block must reach that
// point before any proceeds. POCL-style fission splits the kernel at each
// barrier into "regions" and wraps each region in its own loop over all
// work-items of the block, so the serialized loops honor the barrier (a
// per-block wrapper call runs on one thread, so the regions run in order).
//
// `fissionBarrierKernel` builds the body of the per-block wrapper from the
// per-work-item kernel `linked` (whose barriers are `__cajeta_xpu_cpu_barrier()`
// marker calls). It clones the kernel body in, splits it at the markers, wraps
// each region in a counted work-item loop (which LoopVectorize still widens),
// gives values that live across a barrier per-work-item context arrays, and
// replaces the per-block addrspace(3) shared global with a wrapper-local buffer.
//
// Restricted to structured CFG + block-uniform barriers (all valid GPU code);
// anything else throws cajeta::Exception("XPU-N02") so the registration pass
// falls back to the host stub rather than miscompiling.
//

#pragma once

#include <vector>

namespace llvm {
    class Function;
    class Module;
    class Value;
}

namespace cajeta {
namespace xpu {
namespace cpu {

    // True iff `linked` calls the barrier marker (i.e. needs fission).
    bool usesBarrier(llvm::Function& linked);

    // Build `wrapper`'s body from the per-work-item kernel `linked` by work-item
    // loop fission. `nReal` is the count of real (non-coordinate) params shared
    // by both; `ctaid`/`ntid` are the wrapper's 3 block-coordinate args each.
    // Throws cajeta::Exception("XPU-N02") on an unsupported construct.
    void fissionBarrierKernel(llvm::Function* linked, llvm::Function* wrapper,
                              unsigned nReal,
                              const std::vector<llvm::Value*>& ctaid,
                              const std::vector<llvm::Value*>& ntid,
                              llvm::Module& hostModule);

} // namespace cpu
} // namespace xpu
} // namespace cajeta
