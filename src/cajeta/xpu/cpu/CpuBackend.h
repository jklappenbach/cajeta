//
// CPU backend — host LLVM-module → native object (the fall-to-CPU backend's
// codegen half; cajeta-cpu.md Increment 2).
//
// The CPU analog of NvptxBackend/AmdgpuBackend/SpirvBackend. There is no
// external assembler and no device triple: the host TargetMachine emits a
// native relocatable object directly, and the "device" module *is* a host
// module (the kernel lowered by CpuKernelLowering runs in-process). This header
// is the LLVM-side seam — the host TargetMachine + object emit — kept separate
// from the kernel lowering (CpuKernelLowering) to mirror the other backends.
//

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace llvm {
    class Module;
    class TargetMachine;
}

namespace cajeta {
namespace xpu {
namespace cpu {

    // Create a TargetMachine for the host CPU. Generic CPU/features (correct,
    // not yet tuned — host-CPU feature selection for vectorization is a later
    // refinement). Returns nullptr if the native target isn't registered.
    std::unique_ptr<llvm::TargetMachine> createCpuTargetMachine();

    // Set the host triple + DataLayout on `m` so the lowered IR is laid out for
    // native codegen / JIT.
    void configureHostModule(llvm::Module& m, llvm::TargetMachine& tm);

    // Emit a native relocatable object for `m` via `tm` (already host-
    // configured). Returns the object bytes, or empty on a codegen error
    // (logged). The Tier-0 AOT artifact for `--xpu-backend=cpu --xpu-emit=obj`.
    std::vector<uint8_t> emitObject(llvm::Module& m, llvm::TargetMachine& tm);

} // namespace cpu
} // namespace xpu
} // namespace cajeta
