// CPU backend — the LLVM seam of the fall-to-CPU path: a host TargetMachine and
// native object emit, kept apart from CpuKernelLowering as the other backends are.

#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <string>

namespace llvm {
    class Function;
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

    // The codegen TUNING of a lowered kernel's per-block wrapper. The wrapper
    // is codegen'd by the HOST program's TargetMachine (CpuRegistration links
    // the kernel into the host module and builds the wrapper there), so it
    // would take the host CPU's scheduling model, and on a Zen host that model
    // turns on LLVM's post-RA list scheduler, which is quadratic in basic-block
    // size. A fission wrapper after unrolling is one enormous basic block: the
    // cajeta-llm test binary took 57 minutes to compile on znver2
    // (xpu-kernel-adaptor 6.4.12). The wrapper carries `tune-cpu` instead,
    // "generic" by default (the tuning clang gives every program built
    // without -mtune, no post-RA scheduling), and the ISA attributes are never
    // touched: the wrapper inherits the host machine's SIMD and the vectorizer
    // still sees the host. CAJETA_XPU_CPU_TUNE names another tune target;
    // "host" keeps the host program's own tuning (the A/B knob for measuring
    // what the tuning costs at runtime). Returns the target, empty for "host".
    std::string kernelTuneCpu();
    void tuneKernelWrapper(llvm::Function& wrapper);

} // namespace cpu
} // namespace xpu
} // namespace cajeta
