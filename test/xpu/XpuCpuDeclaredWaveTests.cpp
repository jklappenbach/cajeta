//
// @Wave(width = N) is honoured on the cpu backend (xpu-kernel-adaptor 3.2.4).
//
// A kernel written for a 32-lane wave (`lane = tid % 32`, a whole-wave reduce)
// declares it with @Wave(width = 32). The cpu backend's wave is the host SIMD
// width, 8 under AVX2 and 16 under AVX-512, so without the declaration such a
// kernel either lowered with a 16-lane reduce or did not lower at all, by host.
// With it the work-item loop is pinned to the declared width, the same pin a
// distributed cooperative-matrix kernel takes, and the answer is the host's no
// longer. NVPTX's warp is 32 and nothing else, so it refuses another width by name.
//

#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"
#include "KernelLoweringProbe.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string source(const std::string& waveAnn) {
    return R"CJ(
package test;
import cajeta.xpu.Barrier;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
import cajeta.xpu.Shared;
import cajeta.xpu.Wave;
public class M {
    @Kernel
)CJ" + waveAnn + R"CJ(
    public static void sumk(KernelBuffer<float32> out) {
        uint32 t = KernelThread.globalIdX();
        uint32 lane = KernelThread.x() % 32;
        float32 s = Wave.reduceSumF32((float32) lane);
        out[t] = s + (float32) Wave.width() * 1000.0f;
    }
    @Kernel
)CJ" + waveAnn + R"CJ(
    public static void fissk(KernelBuffer<float32> out) {
        Shared<float32> part = shared float32[8];
        uint32 tid = KernelThread.x();
        float32 s = Wave.reduceSumF32(1.0f);
        if (tid % 32 == 0) { part[tid / 32] = s; }
        Barrier.workgroup();
        out[KernelThread.globalIdX()] = part[0] + part[1] + part[2] + part[3]
            + part[4] + part[5] + part[6] + part[7];
    }

    @Kernel
    public static void widthk(KernelBuffer<float32> out) {
        out[KernelThread.globalIdX()] = (float32) Wave.width();
    }

    public static float32 run(int32 which) {
        uint32 n = 256;
        float32[] h = heap float32[n];
        KernelBuffer<float32> b = heap KernelBuffer<float32>(n);
        KernelStream s #= KernelStream.current();
        if (which == 0) {
            sumk.launch(s, grid: [1], block: [256])(b);
        } else if (which == 1) {
            fissk.launch(s, grid: [1], block: [256])(b);
        } else {
            widthk.launch(s, grid: [1], block: [256])(b);
        }
        s.sync();
        b.download(h);
        float32 first = h[0];
        for (uint32 i = 1; i < n; i = i + 1) {
            if (h[i] != first) { return -1.0f - (float32) i; }
        }
        return first;
    }
}
)CJ";
}

float runOnCpu(const std::string& src, int which) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(src, "test.M", o);
    EXPECT_NE(jit, nullptr);
    if (!jit) return -1000.0f;
    auto fn = jit->lookup<float (*)(int)>("run");
    EXPECT_NE(fn, nullptr);
    return fn ? fn(which) : -1000.0f;
}

}  // namespace

// Every lane reads a 32-wide wave and the sum of lanes 0..31, which is 496.
TEST(XpuCpuDeclaredWave, aDeclaredThirtyTwoLaneWaveRunsAtThirtyTwoOnCpu) {
    float r = runOnCpu(source("    @Wave(width = 32)\n"), 0);
    EXPECT_EQ(r, 32.0f * 1000.0f + 496.0f)
        << "width*1000 + sum of lanes; a negative value names the first lane that disagreed";
}

// Eight 32-lane waves in a 256-thread block, across a barrier: 8 x 32.
TEST(XpuCpuDeclaredWave, theDeclaredWidthSurvivesBarrierFission) {
    float r = runOnCpu(source("    @Wave(width = 32)\n"), 1);
    EXPECT_EQ(r, 256.0f);
}

// Undeclared, the wave is the host's width, below 32 on every cpu we build for, so the
// same kernel reads two different sums inside one 32-lane block: the defect the pin removes.
TEST(XpuCpuDeclaredWave, anUndeclaredThirtyTwoLaneKernelIsWrongAtTheHostWidth) {
    float w = runOnCpu(source(""), 2);
    ASSERT_GE(w, 2.0f);
    ASSERT_LT(w, 32.0f);
    float r = runOnCpu(source(""), 0);
    EXPECT_LT(r, 0.0f) << "one 32-lane block agreed at a " << w << "-lane wave: " << r;
}

// NVPTX's warp is 32: it lowers @Wave(width = 32) and refuses 64 by name.
TEST(XpuCpuDeclaredWave, nvptxRefusesAWaveWidthItCannotRun) {
    using namespace cajeta::xpu::probe;
    Lowered ok = lowerForNvptx(source("    @Wave(width = 32)\n"), "sumk");
    EXPECT_TRUE(ok.ok) << ok.why;
    Lowered bad = lowerForNvptx(source("    @Wave(width = 64)\n"), "sumk");
    EXPECT_FALSE(bad.ok);
    EXPECT_NE(bad.why.find("@Wave(width = 64)"), std::string::npos) << bad.why;
}
