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
// Supported subset (general single-kernel compute bodies):
//   - params: primitives by value, Buffer<T> / T[] as ptr addrspace(1)
//   - Thread / Workgroup coordinate builtins (-> nvvm sreg reads)
//   - Barrier.workgroup() (-> bar.sync)
//   - mutable locals (entry-block allocas; mem2reg'd before emit), with or
//     without initializer; scalar + buffer-element assignment and compound
//     assignment (+=, -=, …)
//   - if/else, for / while / do-while loops, unlabeled break / continue
//   - buffer/array index load & store (addrspace(1) GEP)
//   - workgroup-shared memory: `Shared<T> tile = shared T[N];` (the `shared`
//     placement keyword) -> one per-block addrspace(3) global of constant
//     size N; indexed/assigned exactly like a buffer
//   - full integer + float operator set: +-*/ %, & | ^, << >> (logical/
//     arithmetic by signedness), short-circuit && ||, comparisons
//   - unary +/-/~/!, prefix & postfix ++/--, numeric casts
// Still raised as XPU-N01 (next increment): dynamic shared memory (size from
// the launch config), Wave shuffles/ballots, calls to user @Device helpers,
// for-each loops, and labeled break/continue. The shared-aliasing borrow rule
// (overlapping &mut slices, spec §11 case 2) is a separate deferred item.
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
