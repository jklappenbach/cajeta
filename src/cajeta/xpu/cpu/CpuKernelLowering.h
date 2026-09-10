// CPU kernel lowering — @Kernel AST → host llvm::Function, a thin wrapper over
// the shared KernelLowering walk. The CPU has no hardware grid, so coordinates
// arrive as trailing params, buffers are flat addrspace(0), and waves are width-1.

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

    // Trailing i32 coordinate params on every CPU kernel: 3 dims ×
    // {threadId, workgroupId, workgroupDim, gridDim}, written by the host driver
    // and read back by threadId() and friends in place of hardware intrinsics.
    inline constexpr unsigned kNumCoordParams = 12;

    // Lower a @Kernel into `deviceModule`, returning the host function it made,
    // named for the method and carrying the coordinate params. Throws XPU-N01 on
    // an unsupported construct.
    llvm::Function* lowerKernel(const MethodPtr& method,
                                llvm::Module& deviceModule);

} // namespace cpu
} // namespace xpu
} // namespace cajeta
