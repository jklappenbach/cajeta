// XPU backend selection seam: everything above codegen is backend-agnostic, and the fork
// (which device IR, which assembler, which loader) happens here. `Backend` is defined in
// the xpu layer rather than reused from Compiler.h, because xpu/ must not depend on compile/.
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

    // The concrete device backends. These values are a runtime ABI - cast to ids in
    // emitBackendManifest - so never renumber or reorder them.
    enum class Backend {
        Nvptx  = 0,  // NVIDIA: AST -> device IR -> PTX -> ptxas -> cubin.
        Amdgpu = 1,  // AMD:    AST -> device IR -> AMDGCN ISA -> lld -> hsaco.
        Spirv  = 2,  // Vulkan: AST -> device IR -> SPIR-V (descriptor-set SSBOs).
        Cpu    = 3,  // CPU:    AST -> host IR (grid->threads) -> native object.
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

    // Embed each @Kernel's device binary plus a registration ctor into `hostModule` for
    // the chosen backend, returning how many were embedded; an unsupported kernel (XPU-N01)
    // is skipped, not fatal. `manifests`, when given, takes one entry per embedded pair.
    int emitKernelRegistration(Backend backend,
                               const std::vector<MethodPtr>& kernels,
                               llvm::Module& hostModule,
                               const std::string& arch,
                               const std::unordered_map<std::string, unsigned>&
                                   kernelMaxThreads = {},
                               std::vector<KernelManifest>* manifests = nullptr);

    // The rasterization parallel of emitKernelRegistration for @Vertex/@Fragment methods.
    // Graphics is SPIR-V/Vulkan-only, so this returns 0 on every other backend. Same
    // skip-don't-fail contract, returning the number of shaders embedded.
    int emitGraphicsRegistration(Backend backend,
                                 const std::vector<MethodPtr>& shaders,
                                 llvm::Module& hostModule,
                                 const std::string& arch);

    // Emit one global ctor per bundled backend calling __cajeta_xpu_register_backend, the
    // compile-time manifest the runtime dispatcher reads. The Backend values match the
    // runtime's priority-ordered ids, so the id is just (int) backend.
    void emitBackendManifest(const std::vector<Backend>& backends,
                             llvm::Module& hostModule);

} // namespace xpu
} // namespace cajeta
