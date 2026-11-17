// AMDGPU kernel lowering — @Kernel AST → device llvm::Function. A thin wrapper
// over the shared KernelLowering walk supplying the AMDGPU target: private
// address space 5, amdgcn id intrinsics, s.barrier, amdgpu_kernel convention.

#pragma once

#include <cstdint>
#include <memory>

namespace llvm {
    class Module;
    class Function;
}

namespace cajeta {
    class Method;
    using MethodPtr = std::shared_ptr<Method>;
}

namespace cajeta {
namespace xpu {
namespace amd {

    // Lowers `method` (a @Kernel) into an already-configured `deviceModule` and
    // returns the created amdgpu_kernel function, whose symbol is the simple method
    // name. Throws XPU-N01 on an unsupported construct.
    llvm::Function* lowerKernel(const MethodPtr& method,
                                llvm::Module& deviceModule);

    // Whether a 16-bit cooperative fragment whose lane elements are consecutive lowers to one
    // wide load (the default); registration turns it off to re-lower a kernel it made spill.
    void setCoop16WideLoads(bool on);

    // How many fragment loads took the 16-bit wide load since the last reset.
    unsigned coop16WideLoadsTaken();
    void resetCoop16WideLoadsTaken();

    // The MFMA fragment map on CDNA3 (gfx94x, gfx950): the tile cell element `e` of
    // `lane` holds for a role (`use` 0 = A, 1 = B, 2 = C/D) whose lanes carry
    // `kPerLane` K-values (4 for f16/bf16 16x16x16, 8 for the int8 16x16x32 that
    // carries a 16x16x16 tile). `row`/`col` are tile coordinates; for A the column
    // is K and for B the row is. `valid` is false for an int8 lane past K = 16,
    // which carries zero.
    struct MfmaCell { unsigned row = 0; unsigned col = 0; bool valid = false; };
    MfmaCell mfmaFragmentCell(uint32_t use, unsigned kPerLane, unsigned lane, unsigned e);

} // namespace amd
} // namespace xpu
} // namespace cajeta
