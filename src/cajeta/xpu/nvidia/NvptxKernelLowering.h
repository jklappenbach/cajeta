// NVPTX kernel lowering: walks a @Kernel's AST and emits device LLVM IR
// directly, without the host CajetaLlvmVisitor, building device types fresh in
// the device context (CajetaType's cached llvm::Type* is bound to the host).

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
namespace nvidia {

    // Lower `method` (must be a @Kernel) into `deviceModule`, already
    // NVPTX-configured by configureDeviceModule. Returns the ptx_kernel function,
    // symbol-named for the kernel's simple name. Throws on unsupported constructs.
    llvm::Function* lowerKernel(const MethodPtr& method,
                                llvm::Module& deviceModule);

    // Same, reporting how many bytes of DYNAMIC shared the lowered kernel
    // needs at launch. Non-zero only when the kernel's static Shared<T>
    // tiles total more than the PTX ABI's 48 KB static cap, in which case
    // they have been relocated into one `extern .shared` block and the
    // launch must size it (and opt in with cuFuncSetAttribute). Zero is the
    // ordinary case and means nothing was moved.
    llvm::Function* lowerKernel(const MethodPtr& method,
                                llvm::Module& deviceModule,
                                uint64_t* dynSharedBytes);

} // namespace nvidia
} // namespace xpu
} // namespace cajeta
