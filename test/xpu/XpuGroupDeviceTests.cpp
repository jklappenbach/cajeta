// Group — the cooperative unit of the cajeta.xpu cooperative-tile surface
// (xpu-cooperative-tile spec §3, Phase A Unit 2). One algorithm expressed on
// Group.stripe / Group.reduce takes the wave shape on AMD (one wave per row,
// lanes stripe the columns, a wave reduce) and the serial shape on CPU (one
// work-item per row, a full stripe loop, an identity reduce) — same source.
//
// The load-bearing cases:
//   - reduce equals a serial reference on BOTH backends (the value differs
//     because the group width differs: 32 on gfx1151, 1 on CPU);
//   - reduceSegmented with segment < width produces INDEPENDENT sums (the trap
//     a whole-group reduce would fail — the wave64-over-a-32-block hazard,
//     exercised here as seg 16 over a 32-wave);
//   - stripe covers every item exactly once across the lanes;
//   - the combined stripe+reduce row-sum is bit-identical AMD vs CPU from ONE
//     source (§2.3.1) — the abstraction's whole point.
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

// --- reduce: out[t] = Group.reduce(Add, in[t]) over one group -------------
const char* kReduceSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Group;\n"
    "import cajeta.xpu.GroupOp;\n"
    "public class Gr {\n"
    "    @Kernel\n"
    "    public static void k(KernelBuffer<float32> out, KernelBuffer<float32> in) {\n"
    "        uint32 t = KernelThread.x();\n"
    "        out[t] = Group.reduce(GroupOp.Add, in[t]);\n"
    "    }\n"
    "    public static float32 run(int32 block) {\n"
    "        uint32 n = 64;\n"
    "        float32[] hin = heap float32[n];\n"
    "        for (uint32 i = 0; i < n; i = i + 1) { hin[i] = 1.0f; }\n"
    "        KernelBuffer<float32> din = heap KernelBuffer<float32>(0, n);\n"
    "        KernelBuffer<float32> dout = heap KernelBuffer<float32>(0, n);\n"
    "        din.allocate();\n"
    "        dout.allocate();\n"
    "        din.upload(hin);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        k.launch(s, grid: [1], block: [block])(dout, din);\n"
    "        s.sync();\n"
    "        float32[] hout = heap float32[n];\n"
    "        dout.download(hout);\n"
    "        din.free();\n"
    "        dout.free();\n"
    "        return hout[0];\n"
    "    }\n"
    "}\n";

// --- segmented reduce: seg 16 over a 32-wave -> two independent sums --------
// Lane t loads in[t] = (t < 16) ? 1 : 100. A segment-16 reduce gives lanes
// 0..15 the sum 16 and lanes 16..31 the sum 1600. run() returns
// out[0]*1e6 + out[16] so both halves are read from one value:
//   correct segment-16 : 16*1e6 + 1600  = 16001600
//   a merge-to-32 bug  : 1616*1e6 + 1616 = 1616001616
const char* kSegSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Group;\n"
    "import cajeta.xpu.GroupOp;\n"
    "public class Gs {\n"
    "    @Kernel\n"
    "    public static void k(KernelBuffer<float32> out, KernelBuffer<float32> in) {\n"
    "        uint32 t = KernelThread.x();\n"
    "        out[t] = Group.reduceSegmented(16, GroupOp.Add, in[t]);\n"
    "    }\n"
    "    public static float32 run() {\n"
    "        uint32 n = 32;\n"
    "        float32[] hin = heap float32[n];\n"
    "        for (uint32 i = 0; i < n; i = i + 1) {\n"
    "            if (i < 16) { hin[i] = 1.0f; } else { hin[i] = 100.0f; }\n"
    "        }\n"
    "        KernelBuffer<float32> din = heap KernelBuffer<float32>(0, n);\n"
    "        KernelBuffer<float32> dout = heap KernelBuffer<float32>(0, n);\n"
    "        din.allocate();\n"
    "        dout.allocate();\n"
    "        din.upload(hin);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        k.launch(s, grid: [1], block: [32])(dout, din);\n"
    "        s.sync();\n"
    "        float32[] hout = heap float32[n];\n"
    "        dout.download(hout);\n"
    "        din.free();\n"
    "        dout.free();\n"
    "        return hout[0] * 1000000.0f + hout[16];\n"
    "    }\n"
    "}\n";

// --- stripe: each lane writes its laneId to the columns it owns ------------
// After launch, out[j] must equal j % width (lane j%width owns column j), and
// every column is written exactly once. run() uploads out = -1, launches, and
// returns the number of columns whose value != j % expectedWidth (0 = pass; a
// skipped column keeps -1 and counts as a mismatch).
const char* kStripeSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.Group;\n"
    "public class Gt {\n"
    "    @Kernel\n"
    "    public static void k(KernelBuffer<int32> out) {\n"
    "        for (int32 j : Group.stripe(100)) {\n"
    "            out[j] = Group.laneId();\n"
    "        }\n"
    "    }\n"
    "    public static int32 run(int32 block, int32 width) {\n"
    "        uint32 n = 100;\n"
    "        int32[] hout = heap int32[n];\n"
    "        for (uint32 i = 0; i < n; i = i + 1) { hout[i] = -1; }\n"
    "        KernelBuffer<int32> dout = heap KernelBuffer<int32>(0, n);\n"
    "        dout.allocate();\n"
    "        dout.upload(hout);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        k.launch(s, grid: [1], block: [block])(dout);\n"
    "        s.sync();\n"
    "        dout.download(hout);\n"
    "        dout.free();\n"
    "        int32 bad = 0;\n"
    "        for (int32 j = 0; j < 100; j = j + 1) {\n"
    "            if (hout[j] != (j % width)) { bad = bad + 1; }\n"
    "        }\n"
    "        return bad;\n"
    "    }\n"
    "}\n";

// --- combined stripe + reduce: a single-group row sum ----------------------
// out[0] = sum_{j<100} A[j], with A[j] = j+1 -> 5050. Bit-identical on AMD
// (lanes stripe, wave reduce) and CPU (serial stripe, identity reduce) — the
// same source, both correct (§2.3.1).
const char* kRowSumSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.Group;\n"
    "import cajeta.xpu.GroupOp;\n"
    "public class Gd {\n"
    "    @Kernel\n"
    "    public static void k(KernelBuffer<float32> out, KernelBuffer<float32> a) {\n"
    "        float32 acc = 0.0f;\n"
    "        for (int32 j : Group.stripe(100)) { acc = acc + a[j]; }\n"
    "        float32 total = Group.reduce(GroupOp.Add, acc);\n"
    "        if (Group.laneId() == 0) { out[0] = total; }\n"
    "    }\n"
    "    public static float32 run(int32 block) {\n"
    "        uint32 n = 100;\n"
    "        float32[] ha = heap float32[n];\n"
    "        for (uint32 i = 0; i < n; i = i + 1) { ha[i] = (float32)(i + 1); }\n"
    "        KernelBuffer<float32> da = heap KernelBuffer<float32>(0, n);\n"
    "        KernelBuffer<float32> dout = heap KernelBuffer<float32>(0, 1);\n"
    "        da.allocate();\n"
    "        dout.allocate();\n"
    "        da.upload(ha);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        k.launch(s, grid: [1], block: [block])(dout, da);\n"
    "        s.sync();\n"
    "        float32[] hout = heap float32[1];\n"
    "        dout.download(hout);\n"
    "        da.free();\n"
    "        dout.free();\n"
    "        return hout[0];\n"
    "    }\n"
    "}\n";

// --- rows(n): one group per row, launch sized from the descriptor ----------
// A real R×C matvec: each group computes row r = sum_j A[r*C+j]*x[j]. The
// launch geometry comes ENTIRELY from the descriptor — grid = Group.rowGrid(R),
// block = Group.laneBlock() (the wave size on a GPU, 1 on CPU); the row comes
// from Group.rowId(); the columns are striped and wave-reduced. No wave-width,
// grid, or block literal at any call site. run() checks every row against the
// dense reference and returns the mismatch count (0 = pass).
const char* kMatVecSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.Group;\n"
    "import cajeta.xpu.GroupOp;\n"
    "public class Gm {\n"
    "    @Kernel\n"
    "    public static void k(KernelBuffer<float32> out, KernelBuffer<float32> a,\n"
    "                         KernelBuffer<float32> x) {\n"
    "        int32 r = Group.rowId();\n"
    "        int32 base = r * 48;\n"
    "        float32 acc = 0.0f;\n"
    "        for (int32 j : Group.stripe(48)) { acc = acc + a[base + j] * x[j]; }\n"
    "        float32 dot = Group.reduce(GroupOp.Add, acc);\n"
    "        if (Group.laneId() == 0) { out[r] = dot; }\n"
    "    }\n"
    "    public static int32 run() {\n"
    "        uint32 rows = 40;\n"
    "        uint32 cols = 48;\n"
    "        float32[] ha = heap float32[rows * cols];\n"
    "        float32[] hx = heap float32[cols];\n"
    "        for (uint32 j = 0; j < cols; j = j + 1) { hx[j] = (float32)(j % 3); }\n"
    "        for (uint32 i = 0; i < rows * cols; i = i + 1) {\n"
    "            ha[i] = (float32)(i % 7);\n"
    "        }\n"
    "        KernelBuffer<float32> da = heap KernelBuffer<float32>(0, rows * cols);\n"
    "        KernelBuffer<float32> dx = heap KernelBuffer<float32>(0, cols);\n"
    "        KernelBuffer<float32> dout = heap KernelBuffer<float32>(0, rows);\n"
    "        da.allocate();\n"
    "        dx.allocate();\n"
    "        dout.allocate();\n"
    "        da.upload(ha);\n"
    "        dx.upload(hx);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        k.launch(s, grid: [Group.rowGrid((int32) rows)],\n"
    "                    block: [Group.laneBlock()])(dout, da, dx);\n"
    "        s.sync();\n"
    "        float32[] hout = heap float32[rows];\n"
    "        dout.download(hout);\n"
    "        da.free();\n"
    "        dx.free();\n"
    "        dout.free();\n"
    "        int32 bad = 0;\n"
    "        for (uint32 r = 0; r < rows; r = r + 1) {\n"
    "            float32 ref = 0.0f;\n"
    "            for (uint32 j = 0; j < cols; j = j + 1) {\n"
    "                ref = ref + ha[r * cols + j] * hx[j];\n"
    "            }\n"
    "            float32 d = hout[r] - ref;\n"
    "            if (d < 0.0f) { d = -d; }\n"
    "            if (d > 0.001f) { bad = bad + 1; }\n"
    "        }\n"
    "        return bad;\n"
    "    }\n"
    "}\n";

} // namespace

// 2.1.1 (AMD): the wave (group of 32) reduces its lane values -> 32 * 1.0.
TEST(XpuGroupDevice, reduceAddEqualsSerialOnAmd) {
    CAJETA_SKIP_IF_NO_HIP();
    auto jit = CajetaJit::compile(kReduceSource, "test.Gr", amdOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)(int)>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(fn(32), 32.0f)
        << "Group.reduce(Add) over a 32-lane wave must sum all 32 lanes";
}

// 2.1.1 (CPU): the group is one work-item (width 1) -> the identity, in[0].
TEST(XpuGroupDevice, reduceAddEqualsSerialOnCpu) {
    auto jit = CajetaJit::compile(kReduceSource, "test.Gr", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)(int)>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(fn(1), 1.0f)
        << "Group.reduce(Add) on the CPU backend (width 1) is the identity; a "
           "cross-SIMD-lane sum would wrongly merge independent rows";
}

// 2.1.2: segment < width FIRES — two independent 16-lane sums on a 32-wave.
TEST(XpuGroupDevice, reduceSegmentedIsIndependentPerSegmentOnAmd) {
    CAJETA_SKIP_IF_NO_HIP();
    auto jit = CajetaJit::compile(kSegSource, "test.Gs", amdOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(fn(), 16001600.0f)
        << "Group.reduceSegmented(16, …) must give two independent sums on a "
           "32-wave; a merge-to-32 bug reads 1616001616";
}

// 2.1.3 (AMD): 32 lanes stripe 100 columns -> out[j] == j % 32, every column
// written exactly once.
TEST(XpuGroupDevice, stripeCoversEveryItemOnceOnAmd) {
    CAJETA_SKIP_IF_NO_HIP();
    auto jit = CajetaJit::compile(kStripeSource, "test.Gt", amdOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)(int, int)>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(32, 32), 0)
        << "Group.stripe did not cover [0,100) exactly once across 32 lanes";
}

// 2.1.3 (CPU): one work-item (width 1) owns every column -> out[j] == 0.
TEST(XpuGroupDevice, stripeCoversEveryItemOnceOnCpu) {
    auto jit = CajetaJit::compile(kStripeSource, "test.Gt", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)(int, int)>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(1, 1), 0)
        << "Group.stripe on the CPU backend must be a full serial loop (lane 0 "
           "owns every column)";
}

// 2.3.1 (AMD): the combined stripe+reduce row sum from one source.
TEST(XpuGroupDevice, rowSumStripeThenReduceOnAmd) {
    CAJETA_SKIP_IF_NO_HIP();
    auto jit = CajetaJit::compile(kRowSumSource, "test.Gd", amdOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)(int)>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(fn(32), 5050.0f)
        << "stripe + reduce row sum wrong on AMD";
}

// 2.3.1 (CPU): the SAME source, correct on the CPU backend.
TEST(XpuGroupDevice, rowSumStripeThenReduceOnCpu) {
    auto jit = CajetaJit::compile(kRowSumSource, "test.Gd", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)(int)>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_FLOAT_EQ(fn(1), 5050.0f)
        << "stripe + reduce row sum wrong on CPU (width-1 shape) — the "
           "abstraction is not portable";
}

// 2.2.1/2.2.2/2.3.1 (AMD): a full R×C matvec — one group per row, launch
// geometry from Group.rowGrid/laneBlock, row from Group.rowId, columns striped
// and wave-reduced. No grid/block/wave-width literal at any call site.
TEST(XpuGroupDevice, matVecOneGroupPerRowOnAmd) {
    CAJETA_SKIP_IF_NO_HIP();
    auto jit = CajetaJit::compile(kMatVecSource, "test.Gm", amdOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0)
        << "matvec rows disagreed with the dense reference on AMD (launch sized "
           "from the descriptor, one wave per row)";
}

// 2.2.1/2.2.2/2.3.1 (CPU): the SAME source and SAME launch helpers, correct on
// the width-1 CPU shape (one work-item per row, serial stripe, identity reduce).
TEST(XpuGroupDevice, matVecOneGroupPerRowOnCpu) {
    auto jit = CajetaJit::compile(kMatVecSource, "test.Gm", cpuOptions());
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0)
        << "matvec rows disagreed with the dense reference on the CPU backend "
           "(Group.laneBlock() must give a width-1 launch)";
}
