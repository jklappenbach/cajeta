// cajeta-ml-gpu 20.2.2: an f32 x f32 cooperative multiply takes the portable tile on SPIR-V.
// RADV advertises f16 and 8-bit integer operands only (f32 as the accumulator); tiering the
// f32 operands native handed the driver an instruction it segfaulted compiling (measured
// 2026-11-07 on gfx1151 from the kernels package's f32 GEMM).
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kSource =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.CooperativeMatrix;\n"
    "public final class D {\n"
    "    @Kernel\n"
    "    public static void mm(KernelBuffer<float32> c, KernelBuffer<float32> a,\n"
    "                          KernelBuffer<float32> b, uint32 depth) {\n"
    "        CooperativeMatrix<float32,16,16,2> mc;\n"
    "        mc.splat(0.0f);\n"
    "        CooperativeMatrix<float32,16,16,0> ma;\n"
    "        CooperativeMatrix<float32,16,16,1> mb;\n"
    "        ma.load(a, 0, 0, depth);\n"
    "        mb.load(b, 0, 0, depth);\n"
    "        mc.mma(ma, mb);\n"
    "        mc.store(c, 0, 0, depth);\n"
    "    }\n"
    "    public static int32 compiled() { return 1; }\n"
    "    /** Mismatches of a 16x16x16 product of half-step values against the host: 0 when right. */\n"
    "    public static int32 run() {\n"
    "        float32[] ha = heap float32[256];\n"
    "        float32[] hb = heap float32[256];\n"
    "        int32 i = 0;\n"
    "        while (i < 256) { ha[i] = (float32) ((i * 7) % 5 - 2) * 0.5f; hb[i] = (float32) ((i * 11) % 5 - 2) * 0.5f; i = i + 1; }\n"
    "        KernelBuffer<float32> a = heap KernelBuffer<float32>(256);\n"
    "        KernelBuffer<float32> b = heap KernelBuffer<float32>(256);\n"
    "        KernelBuffer<float32> c = heap KernelBuffer<float32>(256);\n"
    "        a.upload(ha);\n"
    "        b.upload(hb);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        mm.launch(s, grid: [1], block: [64])(c, a, b, (uint32) 16);\n"
    "        s.sync();\n"
    "        float32[] hc = heap float32[256];\n"
    "        c.download(hc);\n"
    "        int32 bad = 0;\n"
    "        int32 r = 0;\n"
    "        while (r < 16) {\n"
    "            int32 col = 0;\n"
    "            while (col < 16) {\n"
    "                float32 acc = 0.0f;\n"
    "                int32 k = 0;\n"
    "                while (k < 16) { acc = acc + ha[r * 16 + k] * hb[k * 16 + col]; k = k + 1; }\n"
    "                if (hc[r * 16 + col] != acc) { bad = bad + 1; }\n"
    "                col = col + 1;\n"
    "            }\n"
    "            r = r + 1;\n"
    "        }\n"
    "        return bad;\n"
    "    }\n"
    "}\n";

} // namespace

// 20.2.2: the lowering demotes the f32 operands to the portable tile and says so.
TEST(XpuSpirvF32CoopTier, f32OperandsTakeThePortableTileOnSpirv) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(kSource, "test.D", o);
    std::string err = testing::internal::GetCapturedStderr();
    ASSERT_NE(jit, nullptr);
    auto compiled = jit->lookup<int32_t (*)()>("compiled");
    ASSERT_NE(compiled, nullptr);
    EXPECT_EQ(compiled(), 1);
    EXPECT_EQ(err.find("[xpu-kernel-skipped]"), std::string::npos) << err;
    EXPECT_NE(err.find("[mma-tiering]"), std::string::npos) << "expected the portable-tier note for float32 operands; stderr was:\n" << err;
    EXPECT_NE(err.find("float32"), std::string::npos) << err;
}

// 20.2.2: on a Vulkan device the kernel runs and is exact for half-step inputs.
TEST(XpuSpirvF32CoopTier, f32CoopProductRunsOnVulkan) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    auto jit = CajetaJit::compile(kSource, "test.D", o);
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(run, nullptr);
    EXPECT_EQ(run(), 0);
}
