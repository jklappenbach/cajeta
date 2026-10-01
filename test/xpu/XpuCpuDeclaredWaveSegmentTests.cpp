//
// The second half of the declared-wave tests (XpuCpuDeclaredWaveTests.cpp holds
// the first: a declared 32 is 32 on cpu, survives barrier fission, the
// undeclared defect, NVPTX's refusal). Two sessions pinned the same feature on
// the same day (cajeta 403345fb and a538aa77); the lowering kept the target
// hook, and these four tests kept what the other file does not cover: the
// SEGMENTED reduce inside a declared wave (a declared 32 with eight-lane
// segments summed four rows until cajeta 064b75ba), and the lane-strided loop
// with an early return before a barrier that cajeta-llm's gate/up GLU kernels
// are shaped like.
//
//
// XpuCpuDeclaredWaveTests — `@Wave(width = N)` is honored by the cpu backend.
//
// The spec (CajetaXPU.md §3.2) lets a kernel that needs a fixed wave width
// declare it, and says a target that cannot satisfy the request rejects the
// kernel. The cpu backend CAN satisfy it: its wave is the width the work-item
// loop is vectorized at, and a distributed cooperative tile already forces
// that loop to 32 through the `cajeta.xpu.coop-wavew` marker. Until now the
// annotation reached the MIR and nothing else, so twenty-eight cajeta-llm
// kernels written for a 32-lane wave (`lane = tid % 32`, a reduce across the
// 32 lanes that hold a 256-element block) lowered on cpu at the host width
// of 8 and answered wrong, and were closed by name on every backend whose
// wave is not 32 (KernelCap.WAVE32_ONLY, xpu-kernel-adaptor Unit 9).
//
// With the declaration honored, the same source runs at 32 on cpu: the loop
// is vectorized at 32 (four AVX2 vectors per value), `Wave.width()` folds to
// 32, and the reduces span 32 lanes. Nothing about the kernel changes.
//
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <cstdio>

using cajeta_test::CajetaJit;

namespace {

CajetaJit::Options cpuOnly() {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    return o;
}

// `declared`: one 256-element block per 32 lanes, eight elements a lane, the
// block max reduced across the wave: the shape of every q8 pack in cajeta-llm.
// The block max of block b is 8 * (32 b + 31) + 7 only if the reduce spans all
// 32 lanes; at width 8 it would be the max of a quarter block.
// `host`: the same kernel without the declaration, which reduces at the host
// width, whatever that is; it reports the width so the test can tell the two
// apart on any host.
const char* kSource =
    "package test;\n"
    "import cajeta.xpu.Device;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "import cajeta.xpu.Wave;\n"
    "import cajeta.xpu.Barrier;\n"
    "import cajeta.xpu.Shared;\n"
    "import cajeta.xpu.Workgroup;\n"
    "public class W {\n"
    "    @Kernel @Wave(width = 32)\n"
    "    public static void declared(KernelBuffer<float32> out, KernelBuffer<float32> x) {\n"
    "        uint32 t = KernelThread.x();\n"
    "        uint32 lane = t % 32;\n"
    "        uint32 blk = t / 32;\n"
    "        float32 amax = 0.0f;\n"
    "        uint32 k = 0;\n"
    "        while (k < 8) {\n"
    "            float32 v = x[blk * 256 + lane * 8 + k];\n"
    "            if (v > amax) { amax = v; }\n"
    "            k = k + 1;\n"
    "        }\n"
    "        out[t] = Wave.reduceMaxF32(amax) + 100000.0f * (float32) Wave.width();\n"
    "    }\n"
    "    @Kernel\n"
    "    public static void host(KernelBuffer<float32> out, KernelBuffer<float32> x) {\n"
    "        uint32 t = KernelThread.x();\n"
    "        uint32 lane = t % 32;\n"
    "        uint32 blk = t / 32;\n"
    "        float32 amax = 0.0f;\n"
    "        uint32 k = 0;\n"
    "        while (k < 8) {\n"
    "            float32 v = x[blk * 256 + lane * 8 + k];\n"
    "            if (v > amax) { amax = v; }\n"
    "            k = k + 1;\n"
    "        }\n"
    "        out[t] = Wave.reduceMaxF32(amax) + 100000.0f * (float32) Wave.width();\n"
    "    }\n"
    "    /** A segmented reduce inside the declared wave: eight-lane segments of a\n"
    "     *  32-lane wave sum their lane ids, so lane t answers 8 * (t / 8 % 4) * ... :\n"
    "     *  the sum of t & ~7 .. t | 7. */\n"
    "    @Kernel @Wave(width = 32)\n"
    "    public static void segmented(KernelBuffer<float32> sum, KernelBuffer<float32> max) {\n"
    "        uint32 t = KernelThread.x();\n"
    "        sum[t] = Wave.reduceSumF32Segmented((float32) t, 8);\n"
    "        max[t] = Wave.reduceMaxF32Segmented((float32) t, 8);\n"
    "    }\n"
    "    /** Lanes whose segmented sum or max is not its own eight-lane segment's. */\n"
    "    public static int32 wrongSegments() {\n"
    "        KernelBuffer<float32> sm = heap KernelBuffer<float32>(0, 256);\n"
    "        KernelBuffer<float32> mx = heap KernelBuffer<float32>(0, 256);\n"
    "        sm.allocate(); mx.allocate();\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        segmented.launch(s, grid: [1], block: [256])(sm, mx);\n"
    "        s.sync();\n"
    "        float32[] gs #= heap float32[256];\n"
    "        float32[] gm #= heap float32[256];\n"
    "        sm.download(gs); mx.download(gm);\n"
    "        sm.free(); mx.free();\n"
    "        int32 bad = 0;\n"
    "        int32 t = 0;\n"
    "        while (t < 256) {\n"
    "            int32 base = t - (t % 8);\n"
    "            float32 wantSum = (float32) (8 * base + 28);\n"
    "            float32 wantMax = (float32) (base + 7);\n"
    "            if (gs[t] != wantSum || gm[t] != wantMax) { bad = bad + 1; }\n"
    "            t = t + 1;\n"
    "        }\n"
    "        return bad;\n"
    "    }\n"
    "    /**\n"
    "     * The grouped-id GLU kernels' shape at a declared 32: a lane-strided loop\n"
    "     * whose start is lane / 8 and whose step is 4 (four blocks in flight, eight\n"
    "     * lanes each), the reduce, a uniform early return, a barrier, and a tail\n"
    "     * that only the last wave of the block writes. rows = grid, one wave a row.\n"
    "     */\n"
    "    @Kernel @Wave(width = 32)\n"
    "    public static void strided(KernelBuffer<float32> out, KernelBuffer<float32> in,\n"
    "            uint32 nb, uint32 packOn) {\n"
    "        Shared<float32> tail = shared float32[8];\n"
    "        uint32 tid = KernelThread.x();\n"
    "        uint32 lane = tid % 32;\n"
    "        uint32 wave = KernelThread.globalIdX() / 32;\n"
    "        float32 acc = 0.0f;\n"
    "        uint32 b = lane >> 3;\n"
    "        while (b < nb) {\n"
    "            acc = acc + in[(int64) (wave * nb + b)] * (float32) (1 + (lane & 7));\n"
    "            b = b + 4;\n"
    "        }\n"
    "        float32 tot = Wave.reduceSumF32(acc);\n"
    "        if (lane == 0) { out[(int64) wave] = tot; }\n"
    "        if (packOn == 0) { return; }\n"
    "        Barrier.workgroup();\n"
    "        if (lane == 0) { tail[tid / 32] = tot; }\n"
    "        Barrier.workgroup();\n"
    "        if (tid == 0) {\n"
    "            float32 s = 0.0f;\n"
    "            uint32 w = 0;\n"
    "            while (w < 8) { s = s + tail[w]; w = w + 1; }\n"
    "            out[(int64) (8 * Workgroup.x() + 7)] = s;\n"
    "        }\n"
    "    }\n"
    "    /** Rows (waves) whose strided sum is wrong, or -1000 when the launch did not\n"
    "     *  finish inside ten seconds of host time (a hang). */\n"
    "    public static int32 wrongStrided(uint32 packOn) {\n"
    "        uint32 nb = 6;\n"
    "        uint32 waves = 16;\n"
    "        float32[] hin #= heap float32[waves * nb];\n"
    "        int32 i = 0;\n"
    "        while (i < (int32) (waves * nb)) { hin[i] = (float32) (i % 7); i = i + 1; }\n"
    "        KernelBuffer<float32> bin = heap KernelBuffer<float32>(0, waves * nb);\n"
    "        KernelBuffer<float32> bout = heap KernelBuffer<float32>(0, waves);\n"
    "        bin.allocate(); bout.allocate();\n"
    "        bin.upload(hin);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        strided.launch(s, grid: [waves / 8], block: [256])(bout, bin, nb, packOn);\n"
    "        s.sync();\n"
    "        float32[] got #= heap float32[waves];\n"
    "        bout.download(got);\n"
    "        bin.free(); bout.free();\n"
    "        int32 bad = 0;\n"
    "        uint32 w = 0;\n"
    "        while (w < waves) {\n"
    "            // Every block b of the row is read by eight lanes weighted 1..8: 36 x.\n"
    "            float32 want = 0.0f;\n"
    "            uint32 b = 0;\n"
    "            while (b < nb) { want = want + 36.0f * hin[w * nb + b]; b = b + 1; }\n"
    "            if (packOn != 0 && (w % 8) == 7) {\n"
    "                // The pack tail overwrote row 7 of each block with the block's sum.\n"
    "                float32 bs = 0.0f;\n"
    "                uint32 r = w - 7;\n"
    "                while (r <= w) {\n"
    "                    uint32 c = 0;\n"
    "                    while (c < nb) { bs = bs + 36.0f * hin[r * nb + c]; c = c + 1; }\n"
    "                    r = r + 1;\n"
    "                }\n"
    "                want = bs;\n"
    "            }\n"
    "            if (got[w] != want) { bad = bad + 1; }\n"
    "            w = w + 1;\n"
    "        }\n"
    "        return bad;\n"
    "    }\n"
    "    /** Lanes whose answer is not the block max under a 32-lane wave, or -1 when\n"
    "     *  the kernel reported a width other than 32. */\n"
    "    public static int32 wrongLanes(int32 useDeclared) {\n"
    "        uint32 n = 256;\n"
    "        float32[] xs #= heap float32[2048];\n"
    "        int32 i = 0;\n"
    "        while (i < 2048) { xs[i] = (float32) i; i = i + 1; }\n"
    "        KernelBuffer<float32> x = heap KernelBuffer<float32>(0, 2048);\n"
    "        KernelBuffer<float32> o = heap KernelBuffer<float32>(0, n);\n"
    "        x.allocate(); o.allocate();\n"
    "        x.upload(xs);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        if (useDeclared != 0) {\n"
    "            declared.launch(s, grid: [1], block: [256])(o, x);\n"
    "        } else {\n"
    "            host.launch(s, grid: [1], block: [256])(o, x);\n"
    "        }\n"
    "        s.sync();\n"
    "        float32[] got #= heap float32[256];\n"
    "        o.download(got);\n"
    "        x.free(); o.free();\n"
    "        int32 bad = 0;\n"
    "        int32 t = 0;\n"
    "        while (t < 256) {\n"
    "            float32 width = (float32) ((int32) (got[t] / 100000.0f));\n"
    "            if (width != 32.0f) { return 0 - (int32) got[t]; }\n"
    "            float32 want = (float32) ((t / 32) * 256 + 255);\n"
    "            if (got[t] - 3200000.0f != want) { bad = bad + 1; }\n"
    "            t = t + 1;\n"
    "        }\n"
    "        return bad;\n"
    "    }\n"
    "    public static int32 hostWave() { return Device.waveSize(); }\n"
    "}\n";

} // namespace

TEST(XpuCpuDeclaredWave, aDeclaredWidthOf32IsHonoredOnTheCpuBackend) {
    auto jit = CajetaJit::compile(kSource, "test.W", cpuOnly());
    ASSERT_NE(jit, nullptr);
    auto wrong = jit->lookup<int32_t (*)(int32_t)>("wrongLanes");
    auto hostWave = jit->lookup<int32_t (*)()>("hostWave");
    ASSERT_TRUE(wrong && hostWave);
    std::printf(" RESULT host wave %d declared %d undeclared %d\n", (int) hostWave(),
                (int) wrong(1), (int) wrong(0));
    EXPECT_EQ(wrong(1), 0)
        << "negative: the declared kernel reported a width other than 32 (the value is "
           "minus the first such lane's answer, 100000 * width + block max); positive: lanes "
           "whose block max was not the max over all 32 lanes";
}

// The control: without the declaration the kernel runs at the host width,
// and on a host whose wave is not 32 it reports that width (-1 here) rather
// than 32. On a host whose SIMD wave IS 32 the two are indistinguishable.
TEST(XpuCpuDeclaredWave, anUndeclaredKernelRunsAtTheHostWidth) {
    auto jit = CajetaJit::compile(kSource, "test.W", cpuOnly());
    ASSERT_NE(jit, nullptr);
    auto wrong = jit->lookup<int32_t (*)(int32_t)>("wrongLanes");
    auto hostWave = jit->lookup<int32_t (*)()>("hostWave");
    ASSERT_TRUE(wrong && hostWave);
    if (hostWave() == 32) GTEST_SKIP() << "the host wave is 32: nothing to tell apart";
    EXPECT_LT(wrong(0), 0) << "an undeclared kernel reported width 32 on a host whose wave is "
                           << hostWave();
}

// A segmented reduce keeps its segments on a wave wider than the segment. The
// cpu backend routed every segmented reduce to the whole-wave reduce, which
// was right while its wave was never wider than a quant block's 8 or 32 lanes
// and wrong the moment a declared 32-lane wave held four 8-lane segments (the
// flash decode kernels: eight lanes per K/V row, measured 2026-09-30).
TEST(XpuCpuDeclaredWave, aSegmentedReduceKeepsItsSegmentsInsideTheDeclaredWave) {
    auto jit = CajetaJit::compile(kSource, "test.W", cpuOnly());
    ASSERT_NE(jit, nullptr);
    auto wrong = jit->lookup<int32_t (*)()>("wrongSegments");
    ASSERT_NE(wrong, nullptr);
    EXPECT_EQ(wrong(), 0) << "lanes whose eight-lane segmented sum or max spanned more than its segment";
}

// The grouped-id GLU shape: a lane-strided loop (start lane / 8, step 4), the
// reduce, a uniform early return before a barrier, and a last-wave tail. On
// the first cpu leg with the wave declared, iq3xxsQ8IdGateUpGluKernel hung the
// box for twenty minutes in exactly this shape (2026-09-30).
TEST(XpuCpuDeclaredWave, aLaneStridedLoopWithAnEarlyReturnBeforeABarrierEnds) {
    auto jit = CajetaJit::compile(kSource, "test.W", cpuOnly());
    ASSERT_NE(jit, nullptr);
    auto wrong = jit->lookup<int32_t (*)(uint32_t)>("wrongStrided");
    ASSERT_NE(wrong, nullptr);
    EXPECT_EQ(wrong(0), 0) << "rows wrong with the early return taken by every work item";
    EXPECT_EQ(wrong(1), 0) << "rows wrong with the pack tail run";
}
