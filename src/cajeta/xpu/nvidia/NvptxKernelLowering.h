//
// NVPTX kernel lowering — @Kernel AST -> device llvm::Function.
//
// CajetaXPU step 8 (increment C part 2). A focused device-IR emitter:
// it walks the kernel's parsed AST and emits NVPTX-shaped LLVM IR into
// a device module, WITHOUT reusing the host CajetaLlvmVisitor (which is
// entangled with host-runtime emissions — scope/drop/bounds). It builds
// device types fresh in the device context rather than calling
// CajetaType::getLlvmType() (whose cached llvm::Type* is bound to the
// host context).
//
// Supported subset (enough for SAXPY-class kernels; extended later):
//   - params: primitives by value, Buffer<T> / T[] as ptr addrspace(1)
//   - Thread / Workgroup coordinate builtins (-> nvvm sreg reads)
//   - local `T name = expr;` (single-assignment), if/else, blocks
//   - buffer/array index load & store (addrspace(1) GEP)
//   - integer + float arithmetic and comparisons
// Unsupported constructs raise cajeta::Exception (errorId XPU-N01).
//

#pragma once

#include <memory>
#include <string>

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

    // Lower `method` (must be a @Kernel) into `deviceModule` (already
    // NVPTX-configured via configureDeviceModule). Returns the created
    // ptx_kernel function; its symbol name is the kernel's simple method
    // name (what cuModuleGetFunction will look up). Throws on an
    // unsupported construct.
    llvm::Function* lowerKernel(const MethodPtr& method,
                                llvm::Module& deviceModule);

} // namespace nvidia
} // namespace xpu
} // namespace cajeta
