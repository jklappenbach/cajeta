//
// KernelLoweringProbeTests — the diagnostics in KernelLoweringProbe.h,
// tested against kernels whose frame shape is known by construction.
//
// A probe that lies is worse than no probe. `Cajeta.liveCount()` cost a day
// on 2026-08-14 by returning a balanced count for a transfer that never
// happened, and the lut4 repro cost an afternoon by "measuring no spill" in
// two kernels that had produced no device code at all. So this file exists
// before anything trusts `classifyFrame`, and every case comes in a pair:
// one that must FIRE and one that must stay quiet.
//
// The three shapes, each built deliberately:
//
//   None                a plain arithmetic kernel
//   LegalizedConstruct  a runtime lane read on the DEFAULT lowering, which
//                       NVPTX legalizes through the stack frame
//   ScratchTile         a portable software CooperativeMatrix, whose tile
//                       IS the scratch
//
// The LegalizedConstruct case is built by asking for a vector WIDER than
// NvptxKernelLowering's 16-lane select-chain cap, so it keeps the default
// extractelement. That is deliberate: it pins the classifier against the
// real legalization rather than a mock, and it stays valid if the cap moves,
// because the test asserts the SHAPE, not which kernel produced it.
//

#include <gtest/gtest.h>

#include "KernelLoweringProbe.h"
#include "XpuDeviceTestUtil.h"

#include <string>

using namespace cajeta::xpu::probe;

namespace {

const char* kSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class M {\n"
    // No frame: arithmetic only.
    "    @Kernel\n"
    "    public static void plain(KernelBuffer<float32> out,\n"
    "            KernelBuffer<float32> xs, KernelBuffer<int32> sel,\n"
    "            uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { out[(int64) i] = xs[(int64) i] * 2.0f; }\n"
    "    }\n"
    // A frame from legalization: 32 lanes is past the select-chain cap, so
    // the dynamic read keeps the default extractelement.
    //
    // The lanes are ASSIGNED, not loaded. A vector read straight out of a
    // buffer lets LLVM forward `v[k]` back into a scalar load at `k`, and
    // then there is no frame at all — measured, 2026-09-19, when the first
    // version of this kernel classified as None and briefly looked like a
    // classifier bug. The same trap is why `q4kMatVecKernelIL` spills and a
    // naive repro of it does not.
    "    @Kernel\n"
    "    public static void wideLane(KernelBuffer<float32> out,\n"
    "            KernelBuffer<float32> xs, KernelBuffer<int32> sel,\n"
    "            uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            int64 b = (int64) i * 32L;\n"
    "            Vector<float32,32> v = xs.vload<32>(b) * 0.0f;\n"
    "            float32 d = xs[b];\n"
    "            v[0] = d * 1.0f + 0.0f;\n"
    "            v[1] = d * 2.0f + 1.0f;\n"
    "            v[2] = d * 3.0f + 2.0f;\n"
    "            v[3] = d * 4.0f + 3.0f;\n"
    "            v[4] = d * 5.0f + 4.0f;\n"
    "            v[5] = d * 6.0f + 5.0f;\n"
    "            v[6] = d * 7.0f + 6.0f;\n"
    "            v[7] = d * 8.0f + 7.0f;\n"
    "            v[8] = d * 9.0f + 8.0f;\n"
    "            v[9] = d * 10.0f + 9.0f;\n"
    "            v[10] = d * 11.0f + 10.0f;\n"
    "            v[11] = d * 12.0f + 11.0f;\n"
    "            v[12] = d * 13.0f + 12.0f;\n"
    "            v[13] = d * 14.0f + 13.0f;\n"
    "            v[14] = d * 15.0f + 14.0f;\n"
    "            v[15] = d * 16.0f + 15.0f;\n"
    "            v[16] = d * 17.0f + 16.0f;\n"
    "            v[17] = d * 18.0f + 17.0f;\n"
    "            v[18] = d * 19.0f + 18.0f;\n"
    "            v[19] = d * 20.0f + 19.0f;\n"
    "            v[20] = d * 21.0f + 20.0f;\n"
    "            v[21] = d * 22.0f + 21.0f;\n"
    "            v[22] = d * 23.0f + 22.0f;\n"
    "            v[23] = d * 24.0f + 23.0f;\n"
    "            v[24] = d * 25.0f + 24.0f;\n"
    "            v[25] = d * 26.0f + 25.0f;\n"
    "            v[26] = d * 27.0f + 26.0f;\n"
    "            v[27] = d * 28.0f + 27.0f;\n"
    "            v[28] = d * 29.0f + 28.0f;\n"
    "            v[29] = d * 30.0f + 29.0f;\n"
    "            v[30] = d * 31.0f + 30.0f;\n"
    "            v[31] = d * 32.0f + 31.0f;\n"
    "            int32 k = sel[(int64) i];\n"
    "            out[(int64) i] = v[k];\n"
    "        }\n"
    "    }\n"
    // Under the cap: the select chain, so no frame.
    "    @Kernel\n"
    "    public static void narrowLane(KernelBuffer<float32> out,\n"
    "            KernelBuffer<float32> xs, KernelBuffer<int32> sel,\n"
    "            uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            int64 b = (int64) i * 8L;\n"
    "            Vector<float32,8> v = xs.vload<8>(b) * 0.0f;\n"
    "            float32 d = xs[b];\n"
    "            v[0] = d * 1.0f;  v[1] = d * 2.0f;\n"
    "            v[2] = d * 3.0f;  v[3] = d * 4.0f;\n"
    "            v[4] = d * 5.0f;  v[5] = d * 6.0f;\n"
    "            v[6] = d * 7.0f;  v[7] = d * 8.0f;\n"
    "            int32 k = sel[(int64) i];\n"
    "            out[(int64) i] = v[k];\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() { return 1; }\n"
    "}\n";

Lowered lower(const std::string& kernel) {
    return lowerForNvptx(kSource, kernel, "test.M", "sm_89", "probetest");
}

} // namespace

// --- compile + lower -------------------------------------------------------

TEST(KernelLoweringProbeTests, lowersAKernelAndReportsItsText) {
    const Lowered l = lower("plain");
    ASSERT_TRUE(l.ok) << l.why;
    EXPECT_NE(l.ir.find("define"), std::string::npos) << "no device IR";
    EXPECT_NE(l.ptx.find(".visible .entry plain"), std::string::npos)
        << "no PTX entry:\n" << l.ptx;
}

// A refusal must arrive as a RESULT with a reason, not an exception and not
// a silent empty string — half the lowering tests are about what refuses.
TEST(KernelLoweringProbeTests, aMissingKernelIsAReasonNotAThrow) {
    const Lowered l = lower("noSuchKernel");
    EXPECT_FALSE(l.ok);
    EXPECT_NE(l.why.find("noSuchKernel"), std::string::npos)
        << "the reason must name what was looked for, got: " << l.why;
}

// --- classifyFrame, each shape with its opposite ---------------------------

TEST(KernelLoweringProbeTests, plainKernelHasNoFrame) {
    const Lowered l = lower("plain");
    ASSERT_TRUE(l.ok) << l.why;
    const FrameReport r = classifyFrame(l.ptx);
    EXPECT_EQ(r.shape, FrameShape::None);
    EXPECT_EQ(r.depotBytes, 0u);
}

TEST(KernelLoweringProbeTests, legalizedConstructIsSeenAsOne) {
    const Lowered l = lower("wideLane");
    ASSERT_TRUE(l.ok) << l.why;
    const FrameReport r = classifyFrame(l.ptx);
    EXPECT_EQ(r.shape, FrameShape::LegalizedConstruct)
        << "depot=" << r.depotBytes << " sp=" << r.spRefs
        << " local=" << r.localOps;
    EXPECT_GT(r.depotBytes, 0u);
    EXPECT_GT(r.spRefs, 0u);
    EXPECT_EQ(r.localOps, 0u) << "a legalized construct writes through %SP";
}

// The does-NOT-fire half: the same operation under the select chain must
// come back clean. Without this the classifier could report
// LegalizedConstruct for everything.
TEST(KernelLoweringProbeTests, narrowLaneIsNotSeenAsALegalizedConstruct) {
    const Lowered l = lower("narrowLane");
    ASSERT_TRUE(l.ok) << l.why;
    const FrameReport r = classifyFrame(l.ptx);
    EXPECT_EQ(r.shape, FrameShape::None)
        << "depot=" << r.depotBytes << " sp=" << r.spRefs
        << " local=" << r.localOps;
}

// --- ptxasFrame, the verdict ----------------------------------------------

// The distinction the spill gate cannot currently make: a stack frame with
// ZERO spill stores is not register pressure. Both halves asserted, so this
// fails if ptxas ever stops separating them.
TEST(KernelLoweringProbeTests, ptxasSeparatesAFrameFromARegisterSpill) {
    CAJETA_SKIP_IF_NO_CUDA();
    const Lowered l = lower("wideLane");
    ASSERT_TRUE(l.ok) << l.why;
    const PtxasFrame f = ptxasFrame(l.ptx, "wideLane");
    if (!f.ok) GTEST_SKIP() << "ptxas unavailable or below the version floor";
    EXPECT_GT(f.stackFrame, 0u) << "ptxas should see the legalized frame";
    EXPECT_EQ(f.spillStores, 0u)
        << "a legalized construct is not register pressure, and ptxas says so";
    EXPECT_GT(f.registers, 0u);
}

// The counterpart: a kernel the probe reports frame-free must be frame-free
// to ptxas too. This is the one that catches "the PTX depot is not the
// verdict" going the other way.
TEST(KernelLoweringProbeTests, ptxasAgreesWhenThereIsNoFrame) {
    CAJETA_SKIP_IF_NO_CUDA();
    const Lowered l = lower("narrowLane");
    ASSERT_TRUE(l.ok) << l.why;
    const PtxasFrame f = ptxasFrame(l.ptx, "narrowLane");
    if (!f.ok) GTEST_SKIP() << "ptxas unavailable or below the version floor";
    EXPECT_EQ(f.stackFrame, 0u);
    EXPECT_EQ(f.spillStores, 0u);
}
