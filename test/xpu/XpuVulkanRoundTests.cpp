#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/vulkan/VulkanDriver.h"
#include <string>
#include <vector>

using cajeta_test::CajetaJit;

namespace {

const char* kRoundSource = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public class Rd {
    static int32[] got;
    @Kernel
    public static void rnd(KernelBuffer<int32> out, KernelBuffer<float32> in) {
        uint32 g = KernelThread.globalIdX();
        out[g] = (int32) Math.round(in[g]);
    }
    public static int32 run() {
        float32[] h = heap float32[64];
        for (uint32 i = 0; i < 56; i = i + 1) { h[i] = (float32) i * 0.5f - 14.0f; }
        h[56] = 0.49999997f;
        h[57] = 0.0f - 0.49999997f;
        h[58] = 8388607.5f;
        h[59] = 0.0f - 8388607.5f;
        h[60] = 16777216.0f;
        h[61] = 0.0f - 0.0f;
        h[62] = 1.5000001f;
        h[63] = 0.0f - 2.4999998f;
        KernelBuffer<float32> in = heap KernelBuffer<float32>(64);
        KernelBuffer<int32> out = heap KernelBuffer<int32>(64);
        in.upload(h);
        KernelStream s #= KernelStream.current();
        rnd.launch(s, grid: [1], block: [64])(out, in);
        s.sync();
        got = heap int32[64];
        out.download(got);
        return 1;
    }
    public static int32 at(uint32 i) { return got[i]; }
}
)CJ";

std::vector<int32_t> roundsOn(cajeta::xpu::Backend be) {
    std::vector<int32_t> r;
    CajetaJit::Options opts;
    opts.xpuBackends = {be};
    auto jit = CajetaJit::compile(kRoundSource, "test.Rd", opts);
    EXPECT_NE(jit, nullptr);
    if (!jit) return r;
    auto fn = jit->lookup<int32_t (*)()>("run");
    auto at = jit->lookup<int32_t (*)(uint32_t)>("at");
    EXPECT_TRUE(fn && at);
    if (!(fn && at)) return r;
    EXPECT_EQ(fn(), 1);
    for (uint32_t k = 0; k < 64; ++k) r.push_back(at(k));
    return r;
}

}  // namespace

// Math.round on Vulkan rounds halves away from zero, as the cpu does, where GLSL Round
// leaves the direction to the driver and Mesa rounds to even.
TEST(XpuVulkanRound, halvesRoundAwayFromZeroAsOnTheCpu) {
    if (!cajeta::xpu::vulkan::VulkanDriver::available())
        GTEST_SKIP() << "no Vulkan compute device available";
    std::vector<int32_t> vk = roundsOn(cajeta::xpu::Backend::Spirv);
    std::vector<int32_t> cpu = roundsOn(cajeta::xpu::Backend::Cpu);
    ASSERT_EQ(vk.size(), 64u);
    ASSERT_EQ(cpu.size(), 64u);
    EXPECT_EQ(cpu[1], -14) << "-13.5 rounds to -14 on the cpu";
    EXPECT_EQ(cpu[29], 1) << "0.5 rounds to 1 on the cpu";
    EXPECT_EQ(cpu[33], 3) << "2.5 rounds to 3 on the cpu";
    for (uint32_t k = 0; k < 64; ++k) EXPECT_EQ(vk[k], cpu[k]) << "element " << k;
}
