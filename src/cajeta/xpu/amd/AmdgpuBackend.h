//
// AMDGPU backend — LLVM device-module → AMDGCN ISA / hsaco code object.
//
// The AMD analog of NvptxBackend (cajeta-amd.md §1-2). Where NVPTX goes
// LLVM → PTX text → ptxas → cubin, AMDGPU goes LLVM → AMDGCN object (the
// TargetMachine emits a relocatable ELF directly — no external assembler,
// unlike ptxas) → lld → hsaco code object. ISA *text* emission
// (emitIsa) is the GPU-free Tier-0 hook the emit tests assert against; the
// hsaco path additionally needs lld and is what the on-device launch loads.
//
// This header is the LLVM-side seam; it does not depend on HIP/HSA and is
// testable (ISA text) without a GPU.
//

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace llvm {
    class Module;
    class TargetMachine;
    class Function;
}

namespace cajeta {
namespace xpu {
namespace amd {

    // The device triple. AMDGCN, AMDHSA OS (the ROCm/HIP runtime ABI).
    inline constexpr const char* kAmdgpuTriple = "amdgcn-amd-amdhsa";

    // Create an AMDGPU TargetMachine for the given GFX arch (e.g. "gfx1151"
    // for Strix Halo / RDNA 3.5). Returns nullptr if the amdgcn target isn't
    // registered in this LLVM build. Self-initializes the LLVM target
    // registry on first use, so it works without a Compiler.
    std::unique_ptr<llvm::TargetMachine>
    createAmdgpuTargetMachine(const std::string& arch = "gfx1151");

    // Set the AMDGPU triple + the TargetMachine's DataLayout on `m` so
    // codegen against it produces correctly-laid-out device IR (the AMDGPU
    // DataLayout pins the alloca/private address space to 5, which the kernel
    // lowerer relies on).
    void configureDeviceModule(llvm::Module& m, llvm::TargetMachine& tm);

    // Emit AMDGCN assembly text for `deviceModule` via `tm`. The module's
    // triple / DataLayout must already be AMDGPU (see configureDeviceModule).
    // Returns the ISA text, or an empty string if the target machine can't
    // emit assembly. GPU-free.
    std::string emitIsa(llvm::Module& deviceModule, llvm::TargetMachine& tm);

    // Locate `ld.lld`: $ROCM_PATH/llvm/bin and /opt/rocm/llvm/bin first, then
    // PATH. Returns an empty string if not found.
    std::string findLld();

    // Assemble `deviceModule` into an hsaco code object for `arch`: the
    // AMDGPU TargetMachine emits a relocatable ELF object, then `ld.lld
    // -shared` links it into a loadable code object. Returns the hsaco bytes,
    // or empty on failure (lld missing, or a codegen/link error — which is
    // logged). Single-arch; multi-arch bundling is a later increment.
    std::vector<uint8_t> assembleHsaco(llvm::Module& deviceModule,
                                       llvm::TargetMachine& tm,
                                       const std::string& arch = "gfx1151");

    // Assemble `deviceModule` for several gfx arches and bundle the per-arch
    // hsacos into a single clang-offload-bundle ("HIP fatbin") that
    // hipModuleLoadData loads, selecting the running device's arch. With one
    // arch this is just assembleHsaco. Returns the bundle bytes, or empty on
    // failure (clang-offload-bundler missing, or a per-arch codegen error). Each
    // arch is assembled from a fresh module clone (assembleHsaco mutates it).
    std::vector<uint8_t> assembleHsacoBundle(
        llvm::Module& deviceModule, const std::vector<std::string>& arches);

    // Split a comma-separated arch string ("gfx1100,gfx1151") into a list,
    // trimming spaces and dropping empties. A single arch yields one element.
    std::vector<std::string> splitArchList(const std::string& arch);

    // Per-kernel resource usage parsed from emitted AMDGCN assembly metadata
    // (.amdhsa_kernel / .amdgpu_metadata). The single parser shared by the
    // GPU-free probes and the occupancy backoff (kernel-occupancy-autotune §2).
    struct KernelResourceInfo {
        std::string name;   // .name (kernel symbol)
        int vgpr = -1;      // .vgpr_count       (-1 if absent)
        int spill = -1;     // .vgpr_spill_count (-1 if absent)
    };
    std::vector<KernelResourceInfo> parseKernelResourceUsage(
        const std::string& isa);

    // kernel-occupancy-autotune §2 — set the kernel's actual launch workgroup
    // size as "amdgpu-flat-work-group-size" so the backend budgets registers for
    // the real (small) occupancy instead of its pessimistic 1024-thread default.
    // Measured on gfx1151: a kernel left at the default capped at 192 VGPR and
    // spilled; with the real size it uses up to 256 and spills 0 — logistics
    // only, never changes semantics. `maxThreads` = product of the launch block
    // dims (the largest across launch sites). No-op if maxThreads==0 (unknown) or
    // the function already carries an explicit flat-work-group-size override.
    void setKernelWorkgroupSize(llvm::Function* fn, unsigned maxThreads);

} // namespace amd
} // namespace xpu
} // namespace cajeta
