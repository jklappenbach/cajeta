// The xpu kernel gate (xpu-kernel-adaptor 4.2.2 / 4.2.3, decided by Julian
// 2026-09-25: "make it an error").
//
// Two build diagnostics used to be a note and a warning, and a build missing
// fifty kernels was green: `[xpu-kernel-skipped]`, a @Kernel that produced no
// device code for a backend the build declares, and `[xpu-kernel-unbounded]`,
// a kernel launched with a non-constant block and no declared ceiling. Both
// now FAIL THE BUILD, after every offending kernel has been named, unless the
// kernel names the plan item holding it:
//
//   @Unlowered(backend = "cpu", tracked = "xpu-kernel-adaptor 4.2.1")
//   @Unbounded(tracked = "xpu-kernel-adaptor 7.0.5")
//
// A held kernel prints a NOTE carrying `[tracked: <item>]`, the countable
// shape the device-skip gate (1.6.5) and the kernel census (Unit 6) use, and
// a held kernel whose cause has gone -- it lowers now, or every launch site
// passes a constant block -- is STALE and fails until the declaration is
// removed, so the inventory cannot outlive its cause.
//
// `CAJETA_XPU_KERNEL_GATE=warn` demotes both to warnings for a sweep, the
// §5.5 switch pattern of the ownership checks; a test may override it.
// The state is per build: the compiler resets it at the start of
// emitXpuKernels and throws CAJETA_ERROR_XPU_KERNEL_GATE at the end.

#pragma once

#include <string>
#include <vector>

namespace cajeta { class Method; }

namespace cajeta {
namespace xpu {

    // Error unless CAJETA_XPU_KERNEL_GATE=warn, or a test override says warn.
    bool kernelGateWarns();
    void setKernelGateWarns(bool on);
    void clearKernelGateWarnsOverride();

    // The cpu backend's host wave (its native f32 vector width: 8 on AVX2, 16
    // on AVX-512), set by the cpu registration for the build it runs, 0 when
    // unknown. An @Unlowered(backend = "cpu", hostWaveBelow = N) holds only
    // while the host wave is below N: a kernel that vectorizes at a declared
    // 32 lanes on a 16-wide host and not on an 8-wide one is held on the
    // second host and expected on the first, and neither reads STALE on the
    // other (found 2026-09-30: the id down-combine family, right on proton,
    // declined on Phoenix).
    void setKernelGateHostWave(unsigned lanes);
    unsigned kernelGateHostWave();

    // @Unlowered(backend = B, tracked = ..., hold = true): the backend does not
    // ATTEMPT the kernel. A plain @Unlowered only tracks a decline the compiler
    // reaches on its own (and reads STALE when the kernel lowers); it cannot
    // hold a kernel whose lowering never ends (the cpu vectorize deadline,
    // xpu-kernel-adaptor 6.4.16). A backend asks this before lowering, skips
    // the kernel with reportUnloweredKernel, and the hold is never stale.
    bool unloweredHoldsBeforeLowering(const Method& kernel, const std::string& backend);

    // Per-build state.
    void resetKernelGate();
    unsigned kernelGateErrors();
    const std::vector<std::string>& kernelGateFailures();

    // Under --diag-format=json each report below is a diagnostic at the kernel's declaration
    // whose `artifact` names the kernel and its backend/arch.
    // A kernel with no device code for `backend` ("cpu", "nvptx", "amdgpu",
    // "vulkan"), with the lowering's reason. A note if @Unlowered holds it
    // for that backend, a warning under the sweep switch, else an error.
    void reportUnloweredKernel(const Method& kernel,
                               const std::string& kernelName,
                               const std::string& backend,
                               const std::string& reason,
                               const std::string& arch = "");

    // A kernel that registered device code for `backend`: an error, STALE,
    // if @Unlowered names that backend.
    void noteKernelLowered(const Method& kernel,
                           const std::string& kernelName,
                           const std::string& backend,
                           const std::string& arch = "");

    // A kernel launched with a non-constant block at `sites` sites and no
    // @Occupancy(maxThreads). A note if @Unbounded holds it, a warning under
    // the sweep switch, else an error.
    void reportUnboundedKernel(const Method& kernel,
                               const std::string& kernelName,
                               unsigned sites);

    // A kernel that is bounded (`why` says how: every site constant, or a
    // declared ceiling): an error, STALE, if it carries @Unbounded.
    void noteKernelBounded(const Method& kernel,
                           const std::string& kernelName,
                           const std::string& why);

    // Throws cajeta::Exception CAJETA_ERROR_XPU_KERNEL_GATE naming every
    // failure since the last reset, if there were any.
    void throwIfKernelGateFailed();

} // namespace xpu
} // namespace cajeta
