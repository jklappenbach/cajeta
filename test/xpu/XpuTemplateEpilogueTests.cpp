// cajeta-ml-gpu Unit 9 (9.1.2, 9.2.1, 9.2.3; spec §5.4, §11.1): under the JIT, a kernel in a
// class template over an epilogue TYPE calls that type's @Device helper, and the harness
// resolves a short method name to the ENTRY class when another class declares the same name.
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {

const char* kEpilogue =
    "package test;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "/** A consumer's epilogue: bias, then relu. */\n"
    "public class BiasRelu {\n"
    "    public BiasRelu() { return; }\n"
    "    @Device\n"
    "    public static float32 apply(float32 acc, float32 bias) {\n"
    "        float32 v = acc + bias;\n"
    "        if (v < 0.0f) { v = 0.0f; }\n"
    "        return v;\n"
    "    }\n"
    "}\n"
    "/** A library kernel skeleton whose epilogue is the type parameter. */\n"
    "public class Fused<E> {\n"
    "    public Fused() { return; }\n"
    "    @Kernel\n"
    "    public static void scaleThen(KernelBuffer<float32> y, KernelBuffer<float32> x,\n"
    "                                 KernelBuffer<float32> b, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { y[i] = E.apply(x[i] * 2.0f, b[i]); }\n"
    "    }\n"
    "    public void run(KernelBuffer<float32> y, KernelBuffer<float32> x, KernelBuffer<float32> b, uint32 n) {\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        scaleThen.launch(s, grid: [(n + 63) / 64], block: [64])(y, x, b, n);\n"
    "        s.sync();\n"
    "    }\n"
    "}\n"
    "public class D {\n"
    "    public static int32 run() {\n"
    "        uint32 n = 100;\n"
    "        float32[] hx = heap float32[100];\n"
    "        float32[] hb = heap float32[100];\n"
    "        int32 i = 0;\n"
    "        while (i < 100) { hx[i] = (float32) i - 50.0f; hb[i] = 3.0f; i = i + 1; }\n"
    "        KernelBuffer<float32> x = heap KernelBuffer<float32>(100);\n"
    "        KernelBuffer<float32> b = heap KernelBuffer<float32>(100);\n"
    "        KernelBuffer<float32> y = heap KernelBuffer<float32>(100);\n"
    "        x.upload(hx);\n"
    "        b.upload(hb);\n"
    "        Fused<BiasRelu> f = heap Fused<BiasRelu>();\n"
    "        f.run(y, x, b, n);\n"
    "        float32[] g = heap float32[100];\n"
    "        y.download(g);\n"
    "        int32 bad = 0;\n"
    "        i = 0;\n"
    "        while (i < 100) {\n"
    "            float32 want = hx[i] * 2.0f + hb[i];\n"
    "            if (want < 0.0f) { want = 0.0f; }\n"
    "            if (g[i] != want) { bad = bad + 1; }\n"
    "            i = i + 1;\n"
    "        }\n"
    "        return bad;\n"
    "    }\n"
    "}\n";

// A::run is emitted before the entry class's D::run.
const char* kCollision =
    "package test;\n"
    "public class A {\n"
    "    public A() { return; }\n"
    "    public int32 run(int32 x) { return x + 7; }\n"
    "}\n"
    "public class D {\n"
    "    public static int32 run() { return 1; }\n"
    "}\n";

} // namespace

// 9.1.2 / 9.2.3: bias then relu from the consumer's class, inside the library kernel,
// matches the unfused pair on every element (the JIT path; the AOT path is
// test/cli/XpuTemplateEpilogueTests.cpp).
TEST(XpuTemplateEpilogue, aTypeParameterEpilogueRunsInsideTheKernelUnderTheJit) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(kEpilogue, "test.D", o);
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(run, nullptr);
    EXPECT_EQ(run(), 0);
}

// 9.2.3: lookup("run") is the entry class's run, not the earlier A::run (which took an
// argument and, called with none, read garbage: the crash this item recorded).
TEST(XpuTemplateEpilogue, aShortNameLookupPrefersTheEntryClass) {
    auto jit = CajetaJit::compile(kCollision, "test.D");
    ASSERT_NE(jit, nullptr);
    auto run = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(run, nullptr);
    EXPECT_EQ(run(), 1);
}
