// vulkan-virtual-waves: a declared @Wave(width) runs on a Vulkan device whose subgroup cannot be
// pinned to it, and matches the cpu (the reference order at that width) bit for bit.
#include "gtest/gtest.h"
#include "../PortableEnv.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include "cajeta/xpu/vulkan/VulkanDriver.h"
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using cajeta_test::CajetaJit;

namespace {

const uint32_t kN = 768;

const char* kSource = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Wave;
public class V {
    static float32[] got32;
    static float32[] got64;
    @Kernel
    @Wave(width = 32)
    public static void vw32(KernelBuffer<float32> out, KernelBuffer<float32> in) {
        uint32 g = KernelThread.globalIdX();
        float32 v = in[g];
        uint32 lane = Wave.laneId();
        out[g * 4] = Wave.reduceSumF32(v);
        float32 s = -1.0f;
        if (lane < 16) {
            s = Wave.shuffleXorSyncF32(v, 3);
        }
        out[g * 4 + 1] = s;
        float32 t = -2.0f;
        if ((lane & 1) == 0) {
            t = Wave.shuffleXorSyncF32(v, 10);
        }
        out[g * 4 + 2] = t;
        out[g * 4 + 3] = (float32) lane;
    }
    @Kernel
    @Wave(width = 64)
    public static void vw64(KernelBuffer<float32> out, KernelBuffer<float32> in) {
        uint32 g = KernelThread.globalIdX();
        float32 v = in[g];
        uint32 lane = Wave.laneId();
        out[g * 4] = Wave.reduceSumF32(v);
        float32 s = -1.0f;
        if (lane < 40) {
            s = Wave.shuffleXorSyncF32(v, 33);
        }
        out[g * 4 + 1] = s;
        float32 t = -2.0f;
        if ((lane & 1) == 0) {
            t = Wave.shuffleXorSyncF32(v, 18);
        }
        out[g * 4 + 2] = t;
        out[g * 4 + 3] = (float32) lane;
    }
    public static int32 run() {
        uint32 n = 768;
        float32[] h = heap float32[n];
        for (uint32 i = 0; i < n; i = i + 1) {
            h[i] = (float32) i * 0.3701f - 19.3f;
        }
        KernelBuffer<float32> in = heap KernelBuffer<float32>(n);
        KernelBuffer<float32> out32 = heap KernelBuffer<float32>(n * 4);
        KernelBuffer<float32> out64 = heap KernelBuffer<float32>(n * 4);
        in.upload(h);
        KernelStream s #= KernelStream.current();
        vw32.launch(s, grid: [3], block: [256])(out32, in);
        s.sync();
        vw64.launch(s, grid: [3], block: [256])(out64, in);
        s.sync();
        got32 = heap float32[n * 4];
        got64 = heap float32[n * 4];
        out32.download(got32);
        out64.download(got64);
        return 1;
    }
    public static float32 a(uint32 i) { return got32[i]; }
    public static float32 b(uint32 i) { return got64[i]; }
}
)CJ";

struct Outputs {
    std::vector<float> a, b;
    std::string stderrText;
    bool ran = false;
};

Outputs runOn(cajeta::xpu::Backend be) {
    Outputs o;
    CajetaJit::Options opts;
    opts.xpuBackends = {be};
    auto jit = CajetaJit::compile(kSource, "test.V", opts);
    EXPECT_NE(jit, nullptr);
    if (!jit) return o;
    auto fn = jit->lookup<int32_t (*)()>("run");
    auto aAt = jit->lookup<float (*)(uint32_t)>("a");
    auto bAt = jit->lookup<float (*)(uint32_t)>("b");
    EXPECT_TRUE(fn && aAt && bAt);
    if (!(fn && aAt && bAt)) return o;
    testing::internal::CaptureStderr();
    int32_t r = fn();
    o.stderrText = testing::internal::GetCapturedStderr();
    EXPECT_EQ(r, 1);
    for (uint32_t k = 0; k < kN * 4; ++k) {
        o.a.push_back(aAt(k));
        o.b.push_back(bAt(k));
    }
    o.ran = true;
    return o;
}

uint32_t bits(float v) { uint32_t b; std::memcpy(&b, &v, 4); return b; }

void sameBits(const std::vector<float>& dev, const std::vector<float>& cpu, const char* what) {
    ASSERT_EQ(dev.size(), cpu.size());
    for (size_t k = 0; k < cpu.size(); ++k)
        ASSERT_EQ(bits(dev[k]), bits(cpu[k])) << what << "[" << k << "] (field " << k % 4
            << "): " << dev[k] << " vs cpu " << cpu[k];
}

}  // namespace

// 1.1.1, 1.1.2: on a device that cannot pin 32 or 64, both kernels run as virtual waves and
// match the cpu bit for bit: the float reduce, a shuffle under a slot-divergent branch, one
// under a lane-parity branch, and the logical lane id.
TEST(XpuVulkanVirtualWave, aWaveTheDeviceCannotPinRunsVirtuallyAndMatchesTheCpu) {
    if (cajeta::xpu::vulkan::VulkanDriver::canRunSubgroupWidth(32)
            && cajeta::xpu::vulkan::VulkanDriver::canRunSubgroupWidth(64))
        GTEST_SKIP() << "this Vulkan device pins 32 and 64, so nothing runs virtually";
    Outputs dev = runOn(cajeta::xpu::Backend::Spirv);
    ASSERT_TRUE(dev.ran);
    EXPECT_EQ(dev.stderrText.find("xpu-launch-refused"), std::string::npos) << dev.stderrText;
    EXPECT_EQ(dev.stderrText.find("[xpu-kernel-skipped]"), std::string::npos) << dev.stderrText;
    Outputs cpu = runOn(cajeta::xpu::Backend::Cpu);
    ASSERT_TRUE(cpu.ran);
    sameBits(dev.a, cpu.a, "vw32");
    sameBits(dev.b, cpu.b, "vw64");
}
