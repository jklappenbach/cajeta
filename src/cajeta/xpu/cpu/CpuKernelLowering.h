//
// CPU kernel lowering — @Kernel AST → host llvm::Function (the fall-to-CPU
// backend; cajeta-cpu.md).
//
// The CPU twin of nvidia::/amd::/vulkan::lowerKernel, and the same thin
// wrapper over the shared xpu/lowering/KernelLowering.cpp AST walk. The CPU
// LoweringTarget supplies the one real divergence from the SIMT backends: the
// CPU has no hardware grid and no coordinate intrinsics, so
//   - the kernel signature gains 9 trailing i32 coordinate params
//     (tid.{x,y,z}, ctaid.{x,y,z}, ntid.{x,y,z}) — the grid→threads model,
//   - threadId/workgroupId/workgroupDim read those args (not intrinsics),
//   - buffers are flat addrspace(0) host pointers, allocas addrspace 0,
//   - wave ops are width-1 (one work-item per invocation), barriers deferred.
// Everything else (the body walk, buffer/operator/control-flow lowering,
// globalId = ctaid*ntid+tid) is the shared lowerer.
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
namespace cpu {

    // Number of trailing i32 coordinate parameters appended to every CPU
    // kernel (3 dims × {threadId, workgroupId, workgroupDim, gridDim}). The host
    // driver passes a work-item's coordinates here; threadId() etc. read them
    // back. The 4th group, gridDim (block count = nctaid), is what makes the
    // grid-stride for-each loop's stride gridSize(dim) = nctaid·ntid available on
    // the CPU (Item 6 Stage 2); the SIMT backends read it from hardware instead.
    inline constexpr unsigned kNumCoordParams = 12;

    // Lower `method` (a @Kernel) into `deviceModule` (host-configured via
    // configureHostModule). Returns the created host function (symbol = simple
    // method name) with the grid→threads coordinate-param signature. Throws
    // XPU-N01 on an unsupported construct (e.g. a workgroup barrier).
    llvm::Function* lowerKernel(const MethodPtr& method,
                                llvm::Module& deviceModule);

} // namespace cpu
} // namespace xpu
} // namespace cajeta
