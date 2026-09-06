//
// AMDGPU kernel registration — embed each @Kernel's hsaco into the host
// module and emit a global constructor that registers it with the runtime.
//
// Structurally identical to NvptxRegistration (cajeta-amd.md §1: the runtime
// symbol __cajeta_xpu_register_module is already backend-neutral, keyed by
// entry name). The only difference behind this header is the device binary
// format — hsaco bytes instead of cubin bytes — and the lowering/assembly
// pipeline that produces them (AMDGCN ISA -> object -> lld -> hsaco).
//
// Self-contained and GPU-free to emit: only needs the AMDGPU target in this
// LLVM build + lld. Kernels whose body uses an unsupported construct
// (XPU-N01) are skipped — never throws on a per-kernel basis.
//

#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace llvm { class Module; }

namespace cajeta {
    class Method;
    using MethodPtr = std::shared_ptr<Method>;
}

namespace cajeta {
namespace xpu {

    struct KernelManifest;

namespace amd {

    // kernel-occupancy-autotune §2: kernel canonical name -> the largest launch
    // workgroup size (product of block dims) seen at any launch site, used to set
    // amdgpu-flat-work-group-size so the backend budgets registers correctly.
    using KernelMaxThreads = std::unordered_map<std::string, unsigned>;

    // Emit hsaco constants + registration ctors into `hostModule` for each
    // @Kernel in `kernels`. Returns the number embedded (0 if none / the
    // amdgcn target is unavailable). `arch` is a GFX target (e.g. "gfx1151").
    // `maxThreads` carries each kernel's launch workgroup size (empty = unknown).
    // `manifests`, when given, receives one KernelManifest per (kernel, arch)
    // embedded — the hash and footprint of the very hsaco that registers
    // (xpu-tile-manifest §2, §3). A multi-arch bundle yields one per arch.
    int emitKernelRegistration(const std::vector<MethodPtr>& kernels,
                               llvm::Module& hostModule,
                               const std::string& arch = "gfx1151",
                               const KernelMaxThreads& maxThreads = {},
                               std::vector<KernelManifest>* manifests = nullptr);

} // namespace amd
} // namespace xpu
} // namespace cajeta
