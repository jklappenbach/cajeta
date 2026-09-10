// AMDGPU backend: LLVM device module → AMDGCN object → lld → hsaco. The LLVM-side
// seam only — no HIP or HSA dependency, and the ISA-text path needs no GPU.

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

    // Null when amdgcn is unregistered; the target registry self-initializes.
    std::unique_ptr<llvm::TargetMachine>
    createAmdgpuTargetMachine(const std::string& arch = "gfx1151");

    // Stamps the AMDGPU triple and DataLayout on `m`; the kernel lowerer relies on
    // that DataLayout pinning the alloca/private address space to 5.
    void configureDeviceModule(llvm::Module& m, llvm::TargetMachine& tm);

    // AMDGCN assembly text, or empty; `deviceModule` must already be configured.
    std::string emitIsa(llvm::Module& deviceModule, llvm::TargetMachine& tm);

    // $ROCM_PATH/llvm/bin, then /opt/rocm/llvm/bin, then PATH; empty when absent.
    std::string findLld();

    // Emits a relocatable ELF and `ld.lld -shared`s it into a loadable code object.
    // Single-arch; empty (and logged) when lld is missing or codegen fails.
    std::vector<uint8_t> assembleHsaco(llvm::Module& deviceModule,
                                       llvm::TargetMachine& tm,
                                       const std::string& arch = "gfx1151");

    // The per-arch hsacos in one clang-offload-bundle for hipModuleLoadData to
    // select from. Each arch assembles from a fresh clone: assembleHsaco mutates.
    std::vector<uint8_t> assembleHsacoBundle(
        llvm::Module& deviceModule, const std::vector<std::string>& arches);

    // The two halves of assembleHsacoBundle, for callers that need the per-arch
    // objects themselves — the manifest hashes the very bytes that register.
    struct ArchHsaco {
        std::string arch;
        std::vector<uint8_t> hsaco;
    };
    std::vector<ArchHsaco> assembleHsacoPerArch(
        llvm::Module& deviceModule, const std::vector<std::string>& arches);
    std::vector<uint8_t> bundleHsacos(const std::vector<ArchHsaco>& perArch);

    // The per-kernel footprint exactly as the code object's `amdhsa.kernels`
    // metadata note states it; empty when there is no such note, 0 per absent field.
    struct AmdCodeObjectFootprint {
        std::string name;                 // .name (kernel symbol)
        unsigned vgpr = 0;                // .vgpr_count
        unsigned sgpr = 0;                // .sgpr_count
        unsigned vgprSpill = 0;           // .vgpr_spill_count
        unsigned sgprSpill = 0;           // .sgpr_spill_count
        unsigned privateSegmentBytes = 0; // .private_segment_fixed_size (scratch)
        unsigned groupSegmentBytes = 0;   // .group_segment_fixed_size (static LDS)
        unsigned wavefrontSize = 0;       // .wavefront_size
        unsigned maxFlatWorkgroupSize = 0;// .max_flat_workgroup_size
    };
    std::vector<AmdCodeObjectFootprint> readCodeObjectFootprint(
        const std::vector<uint8_t>& elf);

    // "gfx1100, gfx1151" -> two elements; spaces trimmed and empties dropped.
    std::vector<std::string> splitArchList(const std::string& arch);

    // Resource usage parsed out of emitted assembly metadata: the one parser the
    // GPU-free probes and the occupancy backoff share.
    struct KernelResourceInfo {
        std::string name;   // .name (kernel symbol)
        int vgpr = -1;      // .vgpr_count       (-1 if absent)
        int spill = -1;     // .vgpr_spill_count (-1 if absent)
    };
    std::vector<KernelResourceInfo> parseKernelResourceUsage(
        const std::string& isa);

    // Records the real launch size as "amdgpu-flat-work-group-size": the backend's
    // pessimistic 1024-thread default caps VGPRs and spills. No-op at 0.
    void setKernelWorkgroupSize(llvm::Function* fn, unsigned maxThreads);

} // namespace amd
} // namespace xpu
} // namespace cajeta
