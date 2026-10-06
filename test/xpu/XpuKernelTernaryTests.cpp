// The ternary operator lowers in a kernel (xpu-kernel-independence 4.8.1.1,
// spec §3.2). Three shapes a kernel writes: a plain select between two
// values, a GUARD whose untaken arm must not run (`i < n ? y[i] : 0.0f`
// reads past the buffer if both arms are evaluated), and arms of two types
// that unify as a binary operation's operands do. Lowered for nvptx, and
// run on cpu with bounds checks on and off, the two modes a kernel sees.
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "KernelLoweringProbe.h"
#include "cajeta/xpu/XpuTarget.h"

#include <memory>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// One program holds the three kernels, so a mode is one compile. `run`
// launches kernel `kind` and reads lane `at`.
std::string program() {
    return R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public class M {
    // A plain select, with a nested ternary in the else arm.
    @Kernel
    public static void sel(KernelBuffer<float32> y, uint32 n, uint32 which) {
        uint32 i = KernelThread.globalIdX();
        y[i] = which == 0 ? y[i] * 2.0f : (which == 1 ? y[i] + 10.0f : -1.0f);
    }
    // A guard: lanes at or past n read nothing and write the sentinel. The
    // buffer holds 8 and the block is 8, so with n = 4 the untaken arm would
    // read y[i] only if both arms ran; the answer says which lanes read.
    @Kernel
    public static void guard(KernelBuffer<float32> y, uint32 n, uint32 which) {
        uint32 i = KernelThread.globalIdX();
        float32 v = i < n ? y[i] : -5.0f;
        y[i] = v;
    }
    // Arms of two types: an int and a float unify to float, as `+` would.
    @Kernel
    public static void mixed(KernelBuffer<float32> y, uint32 n, uint32 which) {
        uint32 i = KernelThread.globalIdX();
        y[i] = which == 0 ? (int32) i : y[i] * 0.5f;
    }
    public static float32 run(uint32 kind, uint32 n, uint32 which, uint32 at) {
        float32[] h = heap float32[8];
        for (uint32 j = 0; j < 8; j = j + 1) { h[j] = (float32) j; }
        KernelBuffer<float32> b = heap KernelBuffer<float32>(8);
        b.upload(h);
        KernelStream s #= KernelStream.current();
        if (kind == 0) { sel.launch(s, grid: [1], block: [8])(b, n, which); }
        if (kind == 1) { guard.launch(s, grid: [1], block: [8])(b, n, which); }
        if (kind == 2) { mixed.launch(s, grid: [1], block: [8])(b, n, which); }
        s.sync();
        b.download(h);
        return h[at];
    }
}
)CJ";
}

using RunFn = float (*)(unsigned, unsigned, unsigned, unsigned);
struct OnCpu {
    std::unique_ptr<CajetaJit> jit;
    RunFn fn = nullptr;
    explicit OnCpu(bool bounds) {
        CajetaJit::Options o;
        o.xpuBackends = {cajeta::xpu::Backend::Cpu};
        o.boundsCheckEnabled = bounds;
        jit = CajetaJit::compile(program(), "test.M", o);
        EXPECT_NE(jit, nullptr);
        if (jit) fn = jit->lookup<RunFn>("run");
        EXPECT_NE(fn, nullptr);
    }
    float operator()(unsigned kind, unsigned n, unsigned which, unsigned at) {
        return fn ? fn(kind, n, which, at) : -1000.0f;
    }
};

} // namespace

TEST(XpuKernelTernary, aTernaryLowersForNvptx) {
    using namespace cajeta::xpu::probe;
    for (const char* k : {"sel", "guard", "mixed"}) {
        Lowered l = lowerForNvptx(program(), k);
        EXPECT_TRUE(l.ok) << k << ": " << l.why;
    }
}

TEST(XpuKernelTernary, aTernaryAnswersOnCpuInBothBoundsModes) {
    for (bool bounds : {true, false}) {
        OnCpu run(bounds);
        // sel, lane 3: which 0 -> 6, which 1 -> 13, which 2 -> -1
        EXPECT_EQ(run(0, 8, 0, 3), 6.0f);
        EXPECT_EQ(run(0, 8, 1, 3), 13.0f);
        EXPECT_EQ(run(0, 8, 2, 3), -1.0f);
        // guard, n = 4: lane 3 keeps its value, lane 5 takes the sentinel
        EXPECT_EQ(run(1, 4, 0, 3), 3.0f);
        EXPECT_EQ(run(1, 4, 0, 5), -5.0f);
        // mixed, lane 6: which 0 -> 6 (the int arm, as float), which 1 -> 3
        EXPECT_EQ(run(2, 8, 0, 6), 6.0f);
        EXPECT_EQ(run(2, 8, 1, 6), 3.0f);
    }
}
