// cajeta-ml-gpu 11.2.2: a float32 or integer narrowed to bfloat16 inside a Vulkan kernel
// rounds to nearest even, as the host cast does. Measured 2026-11-07 on RADV gfx1151: the
// SPIR-V path truncated (259 -> 258, -93.75 -> -93.5) where cpu, hip and the host round
// (260, -94); the kernels package's convert and iota checks are what caught it.
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
    "import cajeta.xpu.KernelThread;\n"
    "public class D {\n"
    "    @Kernel\n"
    "    public static void narrow(KernelBuffer<bfloat16> outF, KernelBuffer<bfloat16> outI,\n"
    "                              KernelBuffer<float32> in, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) {\n"
    "            outF[i] = (bfloat16) in[i];\n"
    "            outI[i] = (bfloat16) (i + 256);\n"
    "        }\n"
    "    }\n"
    "    /** Mismatches against the host cast over the tie and non-tie cases: 0 when bit-exact. */\n"
    "    public static int32 run() {\n"
    "        int32 n = 8;\n"
    "        float32[] hin = heap float32[8];\n"
    "        hin[0] = 259.0f;          // tie: 260\n"
    "        hin[1] = -93.75f;         // tie: -94\n"
    "        hin[2] = 1.00390625f;     // tie: 1.0 (even)\n"
    "        hin[3] = 1.01171875f;     // tie: 1.015625 (even)\n"
    "        hin[4] = 0.1f;            // round up\n"
    "        hin[5] = 1.00390624f;     // just under the tie: 1.0\n"
    "        hin[6] = -2.5e-3f;\n"
    "        hin[7] = 65504.0f;\n"
    "        KernelBuffer<float32> in = heap KernelBuffer<float32>(8);\n"
    "        KernelBuffer<bfloat16> outF = heap KernelBuffer<bfloat16>(8);\n"
    "        KernelBuffer<bfloat16> outI = heap KernelBuffer<bfloat16>(8);\n"
    "        in.upload(hin);\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        narrow.launch(s, grid: [1], block: [64])(outF, outI, in, (uint32) n);\n"
    "        s.sync();\n"
    "        bfloat16[] gf = heap bfloat16[8];\n"
    "        bfloat16[] gi = heap bfloat16[8];\n"
    "        outF.download(gf);\n"
    "        outI.download(gi);\n"
    "        int32 bad = 0;\n"
    "        int32 i = 0;\n"
    "        while (i < n) {\n"
    "            if ((float32) gf[i] != (float32) ((bfloat16) hin[i])) { bad = bad + 1; }\n"
    "            if ((float32) gi[i] != (float32) ((bfloat16) (i + 256))) { bad = bad + 1; }\n"
    "            i = i + 1;\n"
    "        }\n"
    "        return bad;\n"
    "    }\n"
    "}\n";

int32_t mismatchesOn(cajeta::xpu::Backend be) {
    CajetaJit::Options o;
    o.xpuBackends = {be};
    auto jit = CajetaJit::compile(kSource, "test.D", o);
    if (!jit) return -1;
    auto run = jit->lookup<int32_t (*)()>("run");
    if (!run) return -2;
    return run();
}

} // namespace

// 11.2.2: the cpu backend is the host's own rounding (the control).
TEST(XpuSpirvBf16Round, theCpuBackendRoundsToNearestEven) {
    EXPECT_EQ(mismatchesOn(cajeta::xpu::Backend::Cpu), 0);
}

// 11.2.2: Vulkan agrees with the host bit for bit on ties and non-ties, for a float32
// source and for an integer source.
TEST(XpuSpirvBf16Round, vulkanRoundsToNearestEven) {
    if (!cajeta::xpu::test::vulkanAvailable()) GTEST_SKIP() << "no Vulkan device";
    EXPECT_EQ(mismatchesOn(cajeta::xpu::Backend::Spirv), 0);
}
