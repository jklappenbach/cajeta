// NVPTX backend: a device llvm::Module lowered to PTX text, which `ptxas` then
// assembles into a .cubin. Touches no CUDA driver, so it is testable with no GPU.

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
namespace nvidia {

    inline constexpr const char* kNvptxTriple = "nvptx64-nvidia-cuda";

    // A TargetMachine for an SM arch, or nullptr if nvptx64 is not registered in
    // this LLVM build. Self-initializes the registry, so it needs no Compiler.
    std::unique_ptr<llvm::TargetMachine>
    createNvptxTargetMachine(const std::string& arch = "sm_89");

    // Set the NVPTX triple and `tm`'s DataLayout on `m`, before any device codegen.
    void configureDeviceModule(llvm::Module& m, llvm::TargetMachine& tm);

    // PTX text for `deviceModule`, which configureDeviceModule must already have
    // run over; empty if the target machine cannot emit assembly.
    std::string emitPtx(llvm::Module& deviceModule, llvm::TargetMachine& tm);

    // The CUDA `ptxas` assembler: $CUDA_PATH/bin first, then PATH; empty if absent.
    // CUDA_PATH is consulted FIRST so a box whose PATH holds a stale ptxas can be
    // steered without reordering PATH — see kMinPtxasVersion for why that matters.
    std::string findPtxas();

    // A ptxas release, as `ptxas --version` prints it. {0,0} means the version
    // could not be read, which is treated as unknown rather than as old.
    struct PtxasVersion {
        int major = 0;
        int minor = 0;

        constexpr bool unknown() const { return major == 0 && minor == 0; }
        constexpr bool operator==(const PtxasVersion& o) const {
            return major == o.major && minor == o.minor;
        }
        constexpr bool operator!=(const PtxasVersion& o) const { return !(*this == o); }
        // Compared NUMERICALLY on both components: 12.10 is newer than 12.9.
        constexpr bool operator<(const PtxasVersion& o) const {
            return major != o.major ? major < o.major : minor < o.minor;
        }
    };

    // The oldest ptxas cajeta will assemble with. CUDA 12.0's ptxas (12.0.140,
    // what Ubuntu's `nvidia-cuda-toolkit` installs at /usr/bin/ptxas) allocates
    // a loop-invariant local-frame base onto the enclosing loop's induction
    // uniform, so the portable software CooperativeMatrix kernel stores from
    // 1024 bytes past its accumulator — silently, with no ptxas diagnostic.
    // Measured 2026-09-17: 12.0 wrong, 12.9 and 13.3 correct.
    inline constexpr PtxasVersion kMinPtxasVersion{12, 1};

    // The `release X.Y` of `ptxas --version` output; {0,0} when absent.
    PtxasVersion parsePtxasVersion(const std::string& versionText);

    // May cajeta assemble with this release? An UNKNOWN version passes: refusing
    // because `--version` was unreadable would break a working toolchain, and
    // ptxas still rejects PTX it cannot handle. A version known to be below the
    // floor does NOT pass — that one returns wrong answers rather than errors.
    bool ptxasVersionSupported(const PtxasVersion& v);

    // `ptxas --version` for the assembler at `ptxasPath`, or unknown if it
    // cannot be run. Result is cached per path: this shells out.
    PtxasVersion queryPtxasVersion(const std::string& ptxasPath);

    // Shell out to ptxas for a single-arch .cubin; empty bytes on failure. A given
    // `verboseLog` adds `-v` and returns its per-kernel resource report.
    std::vector<uint8_t> assembleCubin(const std::string& ptx,
                                       const std::string& arch = "sm_89",
                                       std::string* verboseLog = nullptr);

    // One kernel's `ptxas -v` report; a field ptxas did not print stays 0.
    struct PtxasKernelStats {
        std::string name;
        unsigned registers = 0;
        unsigned smemBytes = 0;
        unsigned spillStoreBytes = 0;
        unsigned spillLoadBytes = 0;
        unsigned stackBytes = 0;
    };
    std::vector<PtxasKernelStats> parsePtxasVerbose(const std::string& text);

} // namespace nvidia
} // namespace xpu
} // namespace cajeta
