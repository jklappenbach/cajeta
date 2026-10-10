// CPU work-item loop fission for workgroup barriers: a kernel is split at each
// barrier into regions, each wrapped in its own loop over the block's work-items,
// so the serialized loops on one thread honor the barrier POCL-style.

#pragma once

#include <optional>
#include <vector>

namespace llvm {
    class Function;
    class Module;
    class Value;
    class UncondBrInst;
    class BasicBlock;
}

namespace cajeta {
namespace xpu {
namespace cpu {

    // What a target other than the cpu needs from fission: `keepSharedMemory` leaves each
    // addrspace(3) global in place, and `barrierBlocks` receives each barrier's boundary block.
    struct FissionHooks {
        bool keepSharedMemory = false;
        std::vector<llvm::BasicBlock*>* barrierBlocks = nullptr;
        // The block's x extent every launch site passes, when they agree
        // (workgroup-reduce 6.8): `globalIdX() / pinnedBlockX` is then the
        // workgroup index (step 2b).
        std::optional<unsigned> pinnedBlockX;
    };

    // True iff `linked` calls the barrier marker (i.e. needs fission).
    bool usesBarrier(llvm::Function& linked);

    // Inline the `__cajeta_xpu_dev.*` calls that pass a pointer into one of `f`'s allocas.
    // Other helpers stay calls until after fission. Returns the number inlined.
    unsigned inlineAllocaTakingCallees(llvm::Function& f);

    // Build `wrapper`'s body from per-work-item kernel `linked`: `nReal` params are
    // shared, ctaid/ntid/nctaid hold 3 block coordinates each, region latches are
    // appended to `workItemLatches`. Throws Exception("XPU-N02") if unsupported.
    void fissionBarrierKernel(llvm::Function* linked, llvm::Function* wrapper,
                              unsigned nReal,
                              const std::vector<llvm::Value*>& ctaid,
                              const std::vector<llvm::Value*>& ntid,
                              const std::vector<llvm::Value*>& nctaid,
                              llvm::Module& hostModule,
                              std::vector<llvm::UncondBrInst*>* workItemLatches
                                  = nullptr,
                              llvm::Value* dynSharedBytes = nullptr,
                              bool scaffoldUniformLoops = false,
                              const FissionHooks& hooks = {});
    // `scaffoldUniformLoops`: also treat every workgroup-uniform loop (no
    // barrier inside, uniform exit conditions, entered by every work-item) as
    // scaffold, its body regioned like a barrier loop's. A wave kernel needs
    // this: its cross-lane ops only widen when the work-item loop is the
    // INNERMOST loop, and a loop left inside a region makes it an outer one.

} // namespace cpu
} // namespace xpu
} // namespace cajeta
