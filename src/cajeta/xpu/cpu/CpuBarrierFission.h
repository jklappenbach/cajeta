// CPU work-item loop fission for workgroup barriers: a kernel is split at each
// barrier into regions, each wrapped in its own loop over the block's work-items,
// so the serialized loops on one thread honor the barrier POCL-style.

#pragma once

#include <vector>

namespace llvm {
    class Function;
    class Module;
    class Value;
    class UncondBrInst;
}

namespace cajeta {
namespace xpu {
namespace cpu {

    // True iff `linked` calls the barrier marker (i.e. needs fission).
    bool usesBarrier(llvm::Function& linked);

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
                              llvm::Value* dynSharedBytes = nullptr);

} // namespace cpu
} // namespace xpu
} // namespace cajeta
