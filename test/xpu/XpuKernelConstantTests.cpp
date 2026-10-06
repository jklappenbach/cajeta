// Constants reach a kernel the way host code writes them
// (xpu-kernel-independence 4.8.1.4, 4.8.1.8, 4.8.1.9, spec §3.2): a static
// final scalar by its bare name or as `M.NAME`, a static final array literal
// as a table indexed like a buffer, and an array literal as a local. The
// audit found each written around: a table copied into a Shared literal per
// kernel, a constant repeated as a literal, a lookup spelled as a chain of
// ifs.
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "KernelLoweringProbe.h"
#include "cajeta/xpu/XpuTarget.h"

#include <memory>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string program() {
    return R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public class M {
    public static final float32 SCALE = 2.0f;
    public static final int32 SHIFT = 3;
    public static final int32[] TABLE = [10, 20, 30, 40];
    // lane i: y[i] * 2 + TABLE[i & 3] + loc[i & 3] + 3 + half[i & 1]
    @Kernel
    public static void k(KernelBuffer<float32> y, uint32 which) {
        uint32 i = KernelThread.globalIdX();
        int32[] loc = [1, 2, 3, 4];
        float32[] half = [0.5f, y[i]];
        y[i] = y[i] * SCALE + (float32) TABLE[i & 3] + (float32) loc[i & 3]
             + (float32) M.SHIFT + half[i & 1];
    }
    public static float32 run(uint32 at) {
        float32[] h = heap float32[8];
        for (uint32 j = 0; j < 8; j = j + 1) { h[j] = (float32) j; }
        KernelBuffer<float32> b = heap KernelBuffer<float32>(8);
        b.upload(h);
        KernelStream s #= KernelStream.current();
        k.launch(s, grid: [1], block: [8])(b, 0);
        s.sync();
        b.download(h);
        return h[at];
    }
}
)CJ";
}

} // namespace

TEST(XpuKernelConstant, staticConstantsTablesAndLocalLiteralsLowerForNvptx) {
    using namespace cajeta::xpu::probe;
    Lowered l = lowerForNvptx(program(), "k");
    EXPECT_TRUE(l.ok) << l.why;
}

TEST(XpuKernelConstant, staticConstantsTablesAndLocalLiteralsAnswerOnCpu) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(program(), "test.M", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)(unsigned)>("run");
    ASSERT_NE(fn, nullptr);
    // lane 0: 0 + 10 + 1 + 3 + 0.5 = 14.5; lane 3: 6 + 40 + 4 + 3 + 3 = 56
    EXPECT_EQ(fn(0), 14.5f);
    EXPECT_EQ(fn(3), 56.0f);
    // lane 5: 10 + 20 + 2 + 3 + 5 = 40
    EXPECT_EQ(fn(5), 40.0f);
}
