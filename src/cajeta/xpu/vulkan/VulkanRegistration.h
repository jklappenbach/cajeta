//
// Vulkan/SPIR-V kernel registration — embed each @Kernel's SPIR-V binary into
// the host module and emit a global constructor that registers it with the
// runtime.
//
// Structurally identical to Nvptx/AmdgpuRegistration: the runtime symbol
// __cajeta_xpu_register_module is backend-neutral, keyed by entry name. The
// only difference behind this header is the device binary format — SPIR-V
// bytes instead of cubin/hsaco — and the lowering/emit pipeline producing them
// (descriptor-set SPIR-V; no external assembler).
//
// Self-contained and GPU-free to emit: needs only the SPIR-V target in this
// LLVM build. Kernels whose body uses an unsupported construct (XPU-N01) are
// skipped — never throws on a per-kernel basis.
//

#pragma once

#include <memory>
#include <string>
#include <vector>

namespace llvm { class Module; }

namespace cajeta {
    class Method;
    using MethodPtr = std::shared_ptr<Method>;
}

namespace cajeta {
namespace xpu {
namespace vulkan {

    // Emit SPIR-V constants + registration ctors into `hostModule` for each
    // @Kernel in `kernels`. Returns the number embedded (0 if none / the spirv
    // target is unavailable). `arch` is the SPIR-V target env (e.g. "vulkan1.3").
    int emitKernelRegistration(const std::vector<MethodPtr>& kernels,
                               llvm::Module& hostModule,
                               const std::string& arch = "vulkan1.3");

    // Emit SPIR-V constants + registration ctors into `hostModule` for each
    // graphics-shader method (@Vertex/@Fragment/…) in `shaders` — the
    // rasterization parallel of emitKernelRegistration. Each is lowered on its
    // own per-stage SPIR-V TargetMachine and registered under its entry name, so
    // graphics shaders ride the normal build exactly like @Kernels. Returns the
    // number embedded (0 if none / a stage's target env is unavailable). Stages
    // using an unsupported construct (XPU-N01) are skipped, never fatal.
    int emitGraphicsRegistration(const std::vector<MethodPtr>& shaders,
                                 llvm::Module& hostModule,
                                 const std::string& arch = "vulkan1.3");

} // namespace vulkan
} // namespace xpu
} // namespace cajeta
