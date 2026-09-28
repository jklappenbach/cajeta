// CPU kernel registration. A CPU kernel IS host code, so there is no binary to
// embed: the kernel is lowered into the host module as __cajeta_xpu_cpu.<name>,
// wrapped in a launcher thunk, and registered by a global ctor.

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace llvm { class Function; class Module; }

namespace cajeta {
    class Method;
    using MethodPtr = std::shared_ptr<Method>;
}

namespace cajeta {
namespace xpu {

    struct KernelManifest;

namespace cpu {

    // Lower each @Kernel into `hostModule` with a registration ctor, returning
    // how many. `arch` is ignored, present only for a uniform backend signature;
    // a CPU `manifests` entry is identity-only, its footprint absent, never zero.
    int emitKernelRegistration(const std::vector<MethodPtr>& kernels,
                               llvm::Module& hostModule,
                               const std::string& arch = "",
                               std::vector<KernelManifest>* manifests = nullptr);

    // The left-scalar gate. True when a scalar wave stub survives where it
    // would run with width-1 semantics: under a work-item loop that did not
    // vectorize, or INSIDE the vector loop LoopVectorize built (a
    // scalarized call, predicated or replicated, is one scalar call per
    // lane whatever the loop's metadata says). A scalar call in the scalar
    // remainder loop is the vectorizer's own epilogue and is accepted.
    // `which` names the stub. Exposed for the gate's own tests.
    bool waveOpLeftScalar(llvm::Function& f, std::string* which);

    // Calls to a wave stub or one of its width-W VFABI variants. Compared
    // before and after vectorization: a wave op that vanished had its
    // cross-lane semantics optimized away and the kernel is refused.
    unsigned waveOpCallCount(llvm::Function& f);

} // namespace cpu
} // namespace xpu
} // namespace cajeta
