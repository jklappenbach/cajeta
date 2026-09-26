//
// A kernel that LOWERED but could not be ASSEMBLED must say so, by name.
//
// THE DEFECT. AmdgpuRegistration ran a kernel through the lowering, called
// the assembler, got nothing back — no ld.lld on the box — and `continue`d.
// No code object, no manifest, no registration, and no note carrying the
// kernel's name; only an unnamed "cajeta.xpu.amd: ld.lld not found" line
// that no harness counts. Measured 2026-09-21: a cajeta-xgboost build with
// amdgpu enabled emitted 31 manifests each for cpu, nvptx and spirv, and
// ZERO for amdgpu, with no [xpu-kernel-skipped] anywhere in the log. The
// nvptx registration had the identical shape for a missing or failing
// ptxas. This is the invisible-absence failure the xpu plan forbids —
// "no silent skips" — on the two backends where it matters most.
//
// WHAT THIS PINS. When the assembler is absent, every lowered kernel gets
// the standard, countable `[xpu-kernel-skipped] <kernel>: no <backend>
// device code — no assembler ...` note. The phrase "no assembler" is the
// contract: XpuCoopConformanceTests keys on it to tell "this box cannot
// build it" from "the lowering refused it", which are different verdicts.
//
// WHY IT SKIPS ON A BOX THAT HAS THE TOOL. The absence cannot be forced:
// findLld() checks ROCM_PATH, /opt/rocm and PATH, and there is no knob to
// hide a tool that is present. So this test asserts the note WHEN lld is
// genuinely absent and skips, visibly, when it is not — the same gating
// pattern the device tests use, and the honest one. The AMD box runs the
// other half: with lld present, NO such note, and 31 real manifests.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/amd/AmdgpuBackend.h"
#include "cajeta/xpu/nvidia/NvptxBackend.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public final class D {\n"
    "    @Kernel\n"
    "    public static void plain(KernelBuffer<float32> out, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { out[(int64) i] = 2.0f; }\n"
    "    }\n"
    "    public static int32 run() { return 1; }\n"
    "}\n";

std::string compileOn(cajeta::xpu::Backend be) {
    CajetaJit::Options o;
    o.xpuBackends = {be};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(kSource, "test.D", o);
    (void) jit;
    return testing::internal::GetCapturedStderr();
}

}  // namespace

TEST(XpuUnassembledKernelNoteTests, aKernelWithNoAssemblerIsSkippedByName) {
    if (!cajeta::xpu::amd::findLld().empty())
        GTEST_SKIP() << "ld.lld is present; the assembler-absent path cannot "
                        "be forced here. The AMD box asserts the other half.";
    std::string err = compileOn(cajeta::xpu::Backend::Amdgpu);
    // The kernel lowered — a plain arithmetic kernel has nothing to refuse —
    // so the ONLY reason it can be absent is the assembler.
    EXPECT_NE(err.find("[xpu-kernel-skipped] plain"), std::string::npos)
        << "the drop must be a countable note naming the kernel:\n" << err;
    EXPECT_NE(err.find("no assembler"), std::string::npos)
        << "and must say the cause is the assembler, not the lowering:\n"
        << err;
    EXPECT_NE(err.find("no amdgpu device code"), std::string::npos) << err;
}

// The other direction, on every box: a kernel that lowers AND assembles
// prints no such note. nvptx has ptxas here, so this is the positive twin.
TEST(XpuUnassembledKernelNoteTests, aKernelThatAssemblesPrintsNoAssemblerNote) {
    std::string err = compileOn(cajeta::xpu::Backend::Nvptx);
    if (cajeta::xpu::nvidia::findPtxas().empty()) {
        EXPECT_NE(err.find("[xpu-kernel-skipped] plain"), std::string::npos)
            << "no ptxas here, so the drop must be a countable note naming the "
               "kernel:\n" << err;
        EXPECT_NE(err.find("no assembler"), std::string::npos)
            << "and must say the cause is the assembler:\n" << err;
        return;
    }
    EXPECT_EQ(err.find("no assembler"), std::string::npos)
        << "ptxas is on this box, so nothing may claim otherwise:\n" << err;
    EXPECT_EQ(err.find("[xpu-kernel-skipped] plain"), std::string::npos)
        << "a plain kernel must not be skipped on nvptx:\n" << err;
}
