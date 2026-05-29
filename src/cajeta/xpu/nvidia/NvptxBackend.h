//
// NVPTX backend — LLVM device-module → PTX assembly.
//
// CajetaXPU.md §5.2: the NVIDIA path lowers a device llvm::Module
// (triple nvptx64-nvidia-cuda) through an NVPTX TargetMachine to PTX
// text, which `ptxas` then assembles into a .cubin (see assembleCubin,
// later increment). This header is the LLVM-side seam; it does not
// depend on the CUDA driver and is testable without a GPU.
//

#pragma once

#include <memory>
#include <string>

namespace llvm {
    class Module;
    class TargetMachine;
}

namespace cajeta {
namespace xpu {
namespace nvidia {

    // The device triple. NVPTX 64-bit, CUDA flavor.
    inline constexpr const char* kNvptxTriple = "nvptx64-nvidia-cuda";

    // Create an NVPTX TargetMachine for the given SM arch (e.g.
    // "sm_89" for an RTX 4090). Returns nullptr if the nvptx64 target
    // isn't registered in this LLVM build. Self-initializes the LLVM
    // target registry on first use, so it works without a Compiler.
    std::unique_ptr<llvm::TargetMachine>
    createNvptxTargetMachine(const std::string& arch = "sm_89");

    // Set the NVPTX triple + the TargetMachine's DataLayout on `m` so
    // codegen against it produces correctly-laid-out device IR.
    void configureDeviceModule(llvm::Module& m, llvm::TargetMachine& tm);

    // Emit PTX assembly text for `deviceModule` via `tm`. The module's
    // triple / DataLayout must already be NVPTX (see
    // configureDeviceModule). Returns the PTX, or an empty string if
    // the target machine can't emit assembly.
    std::string emitPtx(llvm::Module& deviceModule, llvm::TargetMachine& tm);

} // namespace nvidia
} // namespace xpu
} // namespace cajeta
