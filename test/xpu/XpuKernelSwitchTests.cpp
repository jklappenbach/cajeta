// A switch statement, a switch expression and a `scope` block lower in a
// kernel (xpu-kernel-independence 4.8.1.6 and 4.8.1.7, spec §3.2). The
// statement keeps the host's fallthrough: a group runs into the next until
// a break. The expression picks one arm. A scope block is a block; a kernel
// has no child tasks to join.
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
    // which 0: +1 then falls into case 1: +10 -> +11; which 1: +10;
    // which 2: *2 and break; else: -1. Inside a loop, so a `continue` in a
    // case continues the loop: which 3 skips the add on odd rounds.
    @Kernel
    public static void stmt(KernelBuffer<float32> y, uint32 which) {
        uint32 i = KernelThread.globalIdX();
        for (uint32 r = 0; r < 2; r = r + 1) {
            switch (which) {
                case 0: y[i] = y[i] + 1.0f;
                case 1: y[i] = y[i] + 10.0f; break;
                case 2: y[i] = y[i] * 2.0f; break;
                case 3: if (r == 1) { continue; } y[i] = y[i] + 100.0f; break;
                default: y[i] = -1.0f;
            }
        }
    }
    @Kernel
    public static void expr(KernelBuffer<float32> y, uint32 which) {
        uint32 i = KernelThread.globalIdX();
        float32 f = switch (which) { case 0 -> 1.0f; case 1, 2 -> y[i] * 2.0f; default -> -1.0f; };
        y[i] = f;
    }
    @Kernel
    public static void scoped(KernelBuffer<float32> y, uint32 which) {
        uint32 i = KernelThread.globalIdX();
        scope { float32 t = y[i] * 3.0f; y[i] = t; }
    }
    public static float32 run(uint32 kind, uint32 which, uint32 at) {
        float32[] h = heap float32[8];
        for (uint32 j = 0; j < 8; j = j + 1) { h[j] = (float32) j; }
        KernelBuffer<float32> b = heap KernelBuffer<float32>(8);
        b.upload(h);
        KernelStream s #= KernelStream.current();
        if (kind == 0) { stmt.launch(s, grid: [1], block: [8])(b, which); }
        if (kind == 1) { expr.launch(s, grid: [1], block: [8])(b, which); }
        if (kind == 2) { scoped.launch(s, grid: [1], block: [8])(b, which); }
        s.sync();
        b.download(h);
        return h[at];
    }
}
)CJ";
}

} // namespace

TEST(XpuKernelSwitch, switchStatementExpressionAndScopeBlockLowerForNvptx) {
    using namespace cajeta::xpu::probe;
    for (const char* k : {"stmt", "expr", "scoped"}) {
        Lowered l = lowerForNvptx(program(), k);
        EXPECT_TRUE(l.ok) << k << ": " << l.why;
    }
}

TEST(XpuKernelSwitch, switchStatementExpressionAndScopeBlockAnswerOnCpu) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(program(), "test.M", o);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<float (*)(unsigned, unsigned, unsigned)>("run");
    ASSERT_NE(fn, nullptr);
    // stmt, lane 3, two rounds: which 0 -> 3 + 11 + 11; 1 -> 3 + 20; 2 -> 12;
    // 3 -> 103 (round 1 continues); 7 -> -1
    EXPECT_EQ(fn(0, 0, 3), 25.0f);
    EXPECT_EQ(fn(0, 1, 3), 23.0f);
    EXPECT_EQ(fn(0, 2, 3), 12.0f);
    EXPECT_EQ(fn(0, 3, 3), 103.0f);
    EXPECT_EQ(fn(0, 7, 3), -1.0f);
    // expr, lane 3: which 0 -> 1; 1 -> 6; 2 -> 6; 5 -> -1
    EXPECT_EQ(fn(1, 0, 3), 1.0f);
    EXPECT_EQ(fn(1, 1, 3), 6.0f);
    EXPECT_EQ(fn(1, 2, 3), 6.0f);
    EXPECT_EQ(fn(1, 5, 3), -1.0f);
    // scoped, lane 3: 9
    EXPECT_EQ(fn(2, 0, 3), 9.0f);
}
