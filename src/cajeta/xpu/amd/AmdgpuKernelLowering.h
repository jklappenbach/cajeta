//
// AMDGPU kernel lowering — @Kernel AST → device llvm::Function.
//
// The AMD twin of nvidia::lowerKernel. Both are thin wrappers over the
// shared xpu/lowering/KernelLowering.cpp AST walk; this one supplies the
// AMDGPU LoweringTarget (cajeta-amd.md §2):
//   - alloca address space 5 (private), not 0
//   - llvm.amdgcn.workitem.id.* / workgroup.id.* for coordinates
//   - block dim read from the HSA dispatch packet (no ntid intrinsic on AMD)
//   - llvm.amdgcn.s.barrier (+ workgroup fences) for the barrier
//   - amdgpu_kernel calling convention, no annotations metadata
// Everything else (the body walk, buffer/shared addressing, operators) is
// the shared lowerer. Unsupported constructs raise XPU-N01, exactly as NVPTX.
//

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

    // Lower `method` (a @Kernel) into `deviceModule` (already AMDGPU-configured
    // via AmdgpuBackend::configureDeviceModule). Returns the created
    // amdgpu_kernel function (symbol = simple method name). Throws XPU-N01 on
    // an unsupported construct.
    llvm::Function* lowerKernel(const MethodPtr& method,
                                llvm::Module& deviceModule);

} // namespace amd
} // namespace xpu
} // namespace cajeta
