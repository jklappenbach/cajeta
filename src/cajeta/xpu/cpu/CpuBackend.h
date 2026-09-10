// CPU backend — the LLVM seam of the fall-to-CPU path: a host TargetMachine and
// native object emit, kept apart from CpuKernelLowering as the other backends are.

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

namespace llvm {
    class Module;
    class TargetMachine;
}

namespace cajeta {
namespace xpu {
namespace cpu {

    // Host TargetMachine with generic CPU/features; null if the native target is not registered.
    std::unique_ptr<llvm::TargetMachine> createCpuTargetMachine();

    // Sets the host triple + DataLayout on `m` so the lowered IR is laid out for
    // native codegen or the JIT.
    void configureHostModule(llvm::Module& m, llvm::TargetMachine& tm);

    // Emits a native relocatable object for `m` through a host-configured `tm`;
    // returns the object bytes, or empty on a (logged) codegen error.
    std::vector<uint8_t> emitObject(llvm::Module& m, llvm::TargetMachine& tm);

} // namespace cpu
} // namespace xpu
} // namespace cajeta
