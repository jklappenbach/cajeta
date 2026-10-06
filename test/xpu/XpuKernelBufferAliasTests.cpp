// A buffer reaches a kernel's reads by more than its parameter name
// (xpu-kernel-independence 4.8.1.2 and 4.8.1.3, spec §3.2). A KernelBuffer
// local assigned from a parameter indexes like the parameter, picked by a
// condition or not; a static @Device helper that is passed a buffer indexes
// it. Both are how cajeta-llm's audit found kernels written around the
// lowerer: a helper took `(buf, i)` and had to be inlined by hand, and a
// kernel that chose between two buffers duplicated its body per choice.
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "KernelLoweringProbe.h"
#include "cajeta/xpu/XpuTarget.h"

#include <memory>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string program(const std::string& kernelBody, const std::string& helpers = "") {
    return R"CJ(
package test;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelStream;
import cajeta.xpu.KernelThread;
public class M {
)CJ" + helpers + R"CJ(
    @Kernel
    public static void k(KernelBuffer<float32> y, KernelBuffer<float32> w, uint32 which) {
        uint32 i = KernelThread.globalIdX();
)CJ" + kernelBody + R"CJ(
    }
    public static float32 run(uint32 which, uint32 at) {
        float32[] h = heap float32[8];
        float32[] hw = heap float32[8];
        for (uint32 j = 0; j < 8; j = j + 1) { h[j] = (float32) j; hw[j] = 100.0f + (float32) j; }
        KernelBuffer<float32> b = heap KernelBuffer<float32>(8);
        KernelBuffer<float32> bw = heap KernelBuffer<float32>(8);
        b.upload(h);
        bw.upload(hw);
        KernelStream s #= KernelStream.current();
        k.launch(s, grid: [1], block: [8])(b, bw, which);
        s.sync();
        b.download(h);
        return h[at];
    }
}
)CJ";
}

// 4.8.1.2: a local bound to a parameter, then one chosen by a condition.
const char* kAlias =
    "        KernelBuffer<float32> src = w;\n"
    "        y[i] = src[i] + 1.0f;\n";
const char* kChosen =
    "        KernelBuffer<float32> src = y;\n"
    "        if (which == 1) { src = w; }\n"
    "        y[i] = src[i] + 1.0f;\n";
// 4.8.1.3: a helper that is passed the buffer and indexes it.
const char* kHelper =
    "    @Device\n"
    "    public static float32 at(KernelBuffer<float32> b, uint32 j) { return b[j] * 2.0f; }\n";
const char* kViaHelper =
    "        y[i] = at(w, i) + at(y, i);\n";

using RunFn = float (*)(unsigned, unsigned);
struct OnCpu {
    std::unique_ptr<CajetaJit> jit;
    RunFn fn = nullptr;
    explicit OnCpu(const std::string& src) {
        CajetaJit::Options o;
        o.xpuBackends = {cajeta::xpu::Backend::Cpu};
        jit = CajetaJit::compile(src, "test.M", o);
        EXPECT_NE(jit, nullptr);
        if (jit) fn = jit->lookup<RunFn>("run");
        EXPECT_NE(fn, nullptr);
    }
    float operator()(unsigned which, unsigned at) { return fn ? fn(which, at) : -1000.0f; }
};

} // namespace

TEST(XpuKernelBufferAlias, aBufferLocalAssignedFromAParameterIndexesLikeIt) {
    using namespace cajeta::xpu::probe;
    for (const char* body : {kAlias, kChosen}) {
        Lowered l = lowerForNvptx(program(body), "k");
        EXPECT_TRUE(l.ok) << l.why << "\n" << body;
    }
    OnCpu alias(program(kAlias));
    EXPECT_EQ(alias(0, 3), 104.0f);          // w[3] + 1
    OnCpu chosen(program(kChosen));
    EXPECT_EQ(chosen(0, 3), 4.0f);           // y[3] + 1
    EXPECT_EQ(chosen(1, 3), 104.0f);         // w[3] + 1
}

TEST(XpuKernelBufferAlias, aDeviceHelperIndexesTheBufferItIsPassed) {
    using namespace cajeta::xpu::probe;
    Lowered l = lowerForNvptx(program(kViaHelper, kHelper), "k");
    EXPECT_TRUE(l.ok) << l.why;
    OnCpu via(program(kViaHelper, kHelper));
    EXPECT_EQ(via(0, 3), 2.0f * 103.0f + 2.0f * 3.0f);
}
