// AMDGPU kernel lowering — @Kernel AST → device llvm::Function. A thin wrapper
// over the shared KernelLowering walk supplying the AMDGPU target: private
// address space 5, amdgcn id intrinsics, s.barrier, amdgpu_kernel convention.

#pragma once

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

} // namespace amd
} // namespace xpu
} // namespace cajeta
