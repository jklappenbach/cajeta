//
// XPU backend selection seam.
//
// The frontend, MIR, KernelArg validation, the `shared` keyword, launch
// grammar and launch-borrow checking are all backend-agnostic — they run
// identically for every device target. The fork happens only at codegen:
// which device IR, which assembler, which loader. This header is the single
// dispatch point that picks the concrete backend.
//
// `Backend` is deliberately defined here (in the xpu layer) and NOT reused
// from Compiler.h's `XpuBackend`: the xpu/ code must not depend on the
// compile/ layer. Compiler maps its own enum onto this one (see
// Compiler::emitXpuKernels). The None case lives only in Compiler's enum —
// by the time we reach this seam a concrete device backend has been chosen.
//
// History: this seam was extracted by threading a second backend (AMDGPU)
// through the originally NVIDIA-only path — the first `switch (backend)`
// below is exactly the seam coordinate cajeta-amd.md §0 describes.
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

    // The concrete device backends.
    enum class Backend {
        Nvptx,     // NVIDIA: AST -> device IR -> PTX -> ptxas -> cubin.
        Amdgpu,    // AMD:    AST -> device IR -> AMDGCN ISA -> lld -> hsaco.
        Spirv,     // Vulkan: AST -> device IR -> SPIR-V (descriptor-set SSBOs).
        Cpu,       // CPU:    AST -> host IR (grid->threads) -> native object.
    };

    // Lowercase backend name for diagnostics / artifact suffixes.
    inline const char* backendName(Backend b) {
        switch (b) {
            case Backend::Nvptx:  return "nvptx";
            case Backend::Amdgpu: return "amdgpu";
            case Backend::Spirv:  return "spirv";
            case Backend::Cpu:    return "cpu";
        }
        return "?";
    }

    // Embed each @Kernel's device binary + a registration ctor into
    // `hostModule`, dispatching to the chosen backend. Returns the number of
    // kernels successfully embedded. Mirrors the per-backend
    // emitKernelRegistration contract: unsupported kernels (XPU-N01) are
    // skipped, not fatal. `arch` is the device arch string (e.g. "sm_89" or
    // "gfx1151").
    int emitKernelRegistration(Backend backend,
                               const std::vector<MethodPtr>& kernels,
                               llvm::Module& hostModule,
                               const std::string& arch,
                               const std::unordered_map<std::string, unsigned>&
                                   kernelMaxThreads = {});

    // Embed each graphics-shader (@Vertex/@Fragment/…) method's SPIR-V + a
    // registration ctor into `hostModule` — the rasterization parallel of
    // emitKernelRegistration. Graphics is a SPIR-V/Vulkan-only capability: this
    // dispatches to the Vulkan backend for Backend::Spirv and is a no-op
    // (returns 0) for every other device target, which has no raster pipeline.
    // Returns the number of shaders embedded. Same skip-don't-fail contract.
    int emitGraphicsRegistration(Backend backend,
                                 const std::vector<MethodPtr>& shaders,
                                 llvm::Module& hostModule,
                                 const std::string& arch);

    // Emit one global ctor per bundled backend calling the runtime hook
    // __cajeta_xpu_register_backend((int) backend) — the compile-time manifest
    // the runtime dispatcher (cajeta-cpu.md Increment 4) reads to know which
    // backends a binary bundled. The Backend enum values (Nvptx=0, Amdgpu=1,
    // Spirv=2, Cpu=3) deliberately match the runtime's priority-ordered ids
    // (CUDA=0, HIP=1, VULKAN=2, CPU=3), so the id is just (int) backend.
    void emitBackendManifest(const std::vector<Backend>& backends,
                             llvm::Module& hostModule);

} // namespace xpu
} // namespace cajeta
