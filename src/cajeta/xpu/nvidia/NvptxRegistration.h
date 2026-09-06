//
// NVPTX kernel registration — embed each @Kernel's cubin into the host
// module and emit a global constructor that registers it with the runtime.
//
// CajetaXPU step 11 (host-launch wiring). The host launch site lowers
// `kernel.launch(...)(args)` to __cajeta_xpu_launch(name, ...), which resolves
// the kernel by name from a runtime registry. This pass fills that registry:
// for each @Kernel in the module it builds the device cubin (AST -> LLVM ->
// PTX -> ptxas), stores the bytes as a private constant in the *host* module,
// and appends an llvm.global_ctors entry that calls
// __cajeta_xpu_register_module(entryName, cubinBytes, len) at module-init time
// (LLJIT runs these in jit->initialize(); a native binary runs them at
// startup). The entry name is the kernel's simple method name — the same
// string the launch site and cuModuleGetFunction use.
//
// Self-contained and GPU-free: emission only needs ptxas. Kernels whose body
// uses a construct the device lowerer doesn't support (XPU-N01), or any kernel
// when ptxas is absent, are skipped — the CPU-emulation path still works.
//

#pragma once

#include <string>
#include <vector>
#include <memory>

namespace llvm { class Module; }

namespace cajeta {
    class Method;
    using MethodPtr = std::shared_ptr<Method>;
}

namespace cajeta {
namespace xpu {

    struct KernelManifest;

namespace nvidia {

    // Emit cubin constants + registration ctors into `hostModule` for each
    // method in `kernels` that is a @Kernel. Returns the number of kernels
    // successfully embedded (0 if none / ptxas unavailable). Never throws —
    // unsupported kernels are skipped.
    // `manifests`, when given, receives one KernelManifest per kernel: identity
    // + the cubin hash, footprint from `ptxas -v` (xpu-tile-manifest §3.1).
    int emitKernelRegistration(const std::vector<MethodPtr>& kernels,
                               llvm::Module& hostModule,
                               const std::string& arch = "sm_89",
                               std::vector<KernelManifest>* manifests = nullptr);

} // namespace nvidia
} // namespace xpu
} // namespace cajeta
