// vulkan-virtual-waves 6.1.1: the native float wave reduce on Vulkan and amdgpu takes the driver's
// order. These inputs make a linear sum differ from the reference butterfly on 110 of 128 waves,
// so a match pins that the driver sums in the butterfly order today.
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"
#include <cstring>
#include <cstdio>
using cajeta_test::CajetaJit;
namespace {
const char* kSrc = R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Wave;
import cajeta.xpu.Workgroup;
import cajeta.xpu.GroupOp;
public class N {
    static float32[] got;
    @Kernel
    @Wave(width = 32)
    public static void red(KernelBuffer<float32> out, KernelBuffer<float32> in) {
        uint32 g = KernelThread.globalIdX();
        out[g * 2] = Wave.reduceSumF32(in[g]);
        out[g * 2 + 1] = Workgroup.reduce(GroupOp.Add, in[g]);
    }
    public static int32 run() {
        uint32 n = 4096;
        float32[] h = heap float32[n];
        float32 sc = 1.0f;
        for (uint32 i = 0; i < n; i = i + 1) {
            uint32 k = (i * 7919) % 997;
            sc = 1.0f;
            if (k % 5 == 1) { sc = 1000.0f; }
            if (k % 5 == 2) { sc = 1000000.0f; }
            if (k % 5 == 3) { sc = 0.001f; }
            float32 v = ((float32) k + 0.123f) * sc;
            if (k % 2 == 1) { v = 0.0f - v; }
            h[i] = v;
        }
        KernelBuffer<float32> in = heap KernelBuffer<float32>(n);
        KernelBuffer<float32> out = heap KernelBuffer<float32>(n * 2);
        in.upload(h);
        KernelStream s #= KernelStream.current();
        red.launch(s, grid: [16], block: [256])(out, in);
        s.sync();
        got = heap float32[n * 2];
        out.download(got);
        return 1;
    }
    public static float32 at(uint32 i) { return got[i]; }
}
)CJ";
std::vector<float> runOn(cajeta::xpu::Backend be) {
    std::vector<float> o;
    CajetaJit::Options opts;
    opts.xpuBackends = {be};
    auto jit = CajetaJit::compile(kSrc, "test.N", opts);
    if (!jit) return o;
    auto fn = jit->lookup<int32_t (*)()>("run");
    auto at = jit->lookup<float (*)(uint32_t)>("at");
    if (!fn || !at || fn() != 1) return o;
    for (uint32_t k = 0; k < 8192; ++k) o.push_back(at(k));
    return o;
}
void expectButterfly(const char* name, const std::vector<float>& d, const std::vector<float>& c) {
    ASSERT_EQ(d.size(), c.size()) << name << " did not run";
    int wave = 0, wg = 0;
    for (size_t k = 0; k < c.size(); k += 2) {
        uint32_t a, b; std::memcpy(&a, &d[k], 4); std::memcpy(&b, &c[k], 4); if (a != b) ++wave;
        std::memcpy(&a, &d[k + 1], 4); std::memcpy(&b, &c[k + 1], 4); if (a != b) ++wg;
    }
    EXPECT_EQ(wave, 0) << name << ": the native wave reduce left the butterfly order";
    EXPECT_EQ(wg, 0) << name << ": Workgroup.reduce left the butterfly order";
}
}
TEST(XpuNativeFloatReduceOrder, theNativeFloatReduceSumsInTheReferenceOrder) {
    auto cpu = runOn(cajeta::xpu::Backend::Cpu);
    ASSERT_EQ(cpu.size(), 8192u);
    if (cajeta::xpu::test::vulkanAvailable()) expectButterfly("vulkan", runOn(cajeta::xpu::Backend::Spirv), cpu);
    if (cajeta::xpu::test::hipAvailable()) expectButterfly("amdgpu", runOn(cajeta::xpu::Backend::Amdgpu), cpu);
}
