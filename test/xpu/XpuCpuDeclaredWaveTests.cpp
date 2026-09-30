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
