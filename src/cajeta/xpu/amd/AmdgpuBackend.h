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

} // namespace amd
} // namespace xpu
} // namespace cajeta
