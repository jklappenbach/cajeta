// Witness B — a NON-LLM kernel on the cajeta.xpu cooperative-tile surface
// (xpu-cooperative-tile spec §8.2, Phase A Unit 5). Witness A (the MXFP4 matvec,
// in cajeta-llm) proves the surface reproduces the decode win; this proves it
// generalizes past decode. It is a per-row sum-of-squares reduction — a general
// numeric primitive (normalization, least-squares, statistics), no LLM types,
// no dp4a. One group per row: the lanes stripe the columns and the group
// reduces. The SAME source is correct on AMD (a 32-wide wave per row) and on
// the CPU backend (a width-1 group per row, a serial stripe, an identity
// reduce), with the launch geometry taken entirely from the descriptor
// (Group.rowGrid / Group.laneBlock) — the guard that no decode assumption
// leaked into the surface.
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

CajetaJit::Options amdOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Amdgpu};
    return o;
}

CajetaJit::Options cpuOptions() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    return o;
}

// out[r] = sum_j a[r*cols + j]^2, one group per row. run() checks every row
// against the dense reference and returns the mismatch count (0 = pass).
const char* kSumSqSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.Group;\n"
    "import cajeta.xpu.GroupOp;\n"
    "public class Ssq {\n"
    "    @Kernel\n"
    "    public static void k(KernelBuffer<float32> out, KernelBuffer<float32> a,\n"
    "                         uint32 rows, int32 cols) {\n"
    "        int32 r = Group.rowId();\n"
    "        if ((uint32) r < rows) {\n"
    "            float32 acc = 0.0f;\n"
    "            for (int32 j : Group.stripe(cols)) {\n"
    "                float32 v = a[(int64) r * (int64) cols + (int64) j];\n"
    "                acc = acc + v * v;\n"
    "            }\n"
    "            float32 total = Group.reduce(GroupOp.Add, acc);\n"
    "            if (Group.laneId() == 0) { out[r] = total; }\n"
    "        }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 rows = 40;\n"
    "        int32 cols = 48;\n"
    "        float32[] ha = heap float32[rows * (uint32) cols];\n"
    "        for (uint32 i = 0; i < rows * (uint32) cols; i = i + 1) {\n"
    "            ha[i] = (float32) ((int32)(i % 7) - 3);\n"
    "        }\n"
    "        KernelBuffer<float32> da = heap KernelBuffer<float32>(\n"
    "            0, rows * (uint32) cols);\n"
    "        KernelBuffer<float32> dout = heap KernelBuffer<float32>(0, rows);\n"
    "        da.allocate();\n"
    "        dout.allocate();\n"
    "        da.upload(ha);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        k.launch(s, grid: [Group.rowGrid((int32) rows)],\n"
    "                    block: [Group.laneBlock()])(dout, da, rows, cols);\n"
    "        s.sync();\n"
    "        float32[] hout = heap float32[rows];\n"
    "        dout.download(hout);\n"
    "        da.free();\n"
    "        dout.free();\n"
    "        int32 bad = 0;\n"
    "        for (uint32 r = 0; r < rows; r = r + 1) {\n"
    "            float32 ref = 0.0f;\n"
    "            for (int32 j = 0; j < cols; j = j + 1) {\n"
    "                float32 v = ha[r * (uint32) cols + (uint32) j];\n"
    "                ref = ref + v * v;\n"
    "            }\n"
    "            float32 d = hout[r] - ref;\n"
    "            if (d < 0.0f) { d = -d; }\n"
    "            if (d > 0.001f) { bad = bad + 1; }\n"
    "        }\n"
    "        return bad;\n"
    "    }\n"
    "}\n";

} // namespace

// 5.1.1 / 5.3.1 (AMD): one wave per row, lanes stripe the columns, wave reduce.
TEST(XpuCoopWitnessDevice, sumOfSquaresReductionOnAmd) {
    CAJETA_SKIP_IF_NO_HIP();
    auto jit = CajetaJit::compile(kSumSqSource, "test.Ssq", amdOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0)
        << "non-LLM sum-of-squares reduction disagreed with the dense "
           "reference on AMD";
}

// 5.1.1 / 5.3.1 (CPU): the SAME source on the width-1 shape (one work-item per
// row, serial stripe, identity reduce). Correctness here is the guard that no
// wave/decode assumption leaked into the surface.
TEST(XpuCoopWitnessDevice, sumOfSquaresReductionOnCpu) {
    auto jit = CajetaJit::compile(kSumSqSource, "test.Ssq", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0)
        << "non-LLM sum-of-squares reduction disagreed with the dense "
           "reference on the CPU backend (width-1 shape)";
}
