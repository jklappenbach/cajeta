// NVPTX kernel lowering: walks a @Kernel's AST and emits device LLVM IR
// directly, without the host CajetaLlvmVisitor, building device types fresh in
// the device context (CajetaType's cached llvm::Type* is bound to the host).

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
namespace nvidia {

    // Lower `method` (must be a @Kernel) into `deviceModule`, already
    // NVPTX-configured by configureDeviceModule. Returns the ptx_kernel function,
    // symbol-named for the kernel's simple name. Throws on unsupported constructs.
    llvm::Function* lowerKernel(const MethodPtr& method,
                                llvm::Module& deviceModule);

} // namespace nvidia
} // namespace xpu
} // namespace cajeta
