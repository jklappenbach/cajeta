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
    std::string findPtxas();

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
