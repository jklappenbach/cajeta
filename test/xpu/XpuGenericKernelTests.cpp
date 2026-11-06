//
// Kernels launched from generic code (cajeta-ml-gpu Unit 1, spec §2): a @Kernel generic over
// its element type, launched from a body generic over that type, is instantiated for the
// concrete type at the call and runs.
//
#include "gtest/gtest.h"

#include "../jit/JitTestHelper.h"
#include "XpuDeviceTestUtil.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

CajetaJit::Options withBackend(cajeta::xpu::Backend b) {
    CajetaJit::Options o;
    o.xpuBackends = {b};
    return o;
}

const char* kGenericSource =
    "package test;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.xpu.Device;\n"
    "import cajeta.xpu.KernelBuffer;\n"
    "import cajeta.xpu.KernelStream;\n"
    "import cajeta.xpu.KernelThread;\n"
    "public class Gen {\n"
    "    @Kernel\n"
    "    public static void twiceK<E>(KernelBuffer<E> y, KernelBuffer<E> x, uint32 n) {\n"
    "        uint32 i = KernelThread.globalIdX();\n"
    "        if (i < n) { y[i] = x[i] + x[i]; }\n"
    "    }\n"
    "    public static void twice<E>(KernelBuffer<E> y, KernelBuffer<E> x, uint32 n) {\n"
    "        KernelStream s #= KernelStream.current();\n"
    "        twiceK.launch(s, grid: [(n + 63) / 64], block: [64])(y, x, n);\n"
    "        s.sync();\n"
    "    }\n"
    "    /** Mismatches of y = x + x over 100 float32 elements, through Gen.twice<float32>. */\n"
    "    public static int32 f32() {\n"
    "        uint32 n = 100;\n"
    "        float32[] h = heap float32[100];\n"
    "        int32 i = 0;\n"
    "        while (i < 100) { h[i] = (float32) i * 0.5f + 1.0f; i = i + 1; }\n"
    "        KernelBuffer<float32> x = heap KernelBuffer<float32>(0, n);\n"
    "        x.allocate();\n"
    "        x.upload(h);\n"
    "        KernelBuffer<float32> y = heap KernelBuffer<float32>(0, n);\n"
    "        y.allocate();\n"
    "        Gen.twice<float32>(y, x, n);\n"
    "        float32[] g = heap float32[100];\n"
    "        y.download(g);\n"
    "        int32 bad = 0;\n"
    "        i = 0;\n"
    "        while (i < 100) { if (g[i] != h[i] + h[i]) { bad = bad + 1; } i = i + 1; }\n"
    "        x.free();\n"
    "        y.free();\n"
    "        return bad;\n"
    "    }\n"
    "    /** The same over float16. */\n"
    "    public static int32 f16() {\n"
    "        uint32 n = 100;\n"
    "        float16[] h = heap float16[100];\n"
    "        int32 i = 0;\n"
    "        while (i < 100) { h[i] = (float16) ((float32) i * 0.25f + 1.0f); i = i + 1; }\n"
    "        KernelBuffer<float16> x = heap KernelBuffer<float16>(0, n);\n"
    "        x.allocate();\n"
    "        x.upload(h);\n"
    "        KernelBuffer<float16> y = heap KernelBuffer<float16>(0, n);\n"
    "        y.allocate();\n"
    "        Gen.twice<float16>(y, x, n);\n"
    "        float16[] g = heap float16[100];\n"
    "        y.download(g);\n"
    "        int32 bad = 0;\n"
    "        i = 0;\n"
    "        while (i < 100) { if ((float32) g[i] != (float32) h[i] * 2.0f) { bad = bad + 1; } i = i + 1; }\n"
    "        x.free();\n"
    "        y.free();\n"
    "        return bad;\n"
    "    }\n"
    "    /** Registered kernels whose name contains twiceK. */\n"
    "    public static int32 instantiations() {\n"
    "        int32 c = 0;\n"
    "        int32 n = Device.registeredKernelCount();\n"
    "        int32 i = 0;\n"
    "        while (i < n) {\n"
    "            String name #= Device.registeredKernel(i);\n"
    "            if (name.contains(\"twiceK\")) { c = c + 1; }\n"
    "            i = i + 1;\n"
    "        }\n"
    "        return c;\n"
    "    }\n"
    "    /** A second float32 call site, which must reuse the first instantiation. */\n"
    "    public static int32 f32Again() { return Gen.f32(); }\n"
    "}\n"
    "class Holder<E> {\n"
    "    public Holder() { return; }\n"
    "    public void run(KernelBuffer<E> y, KernelBuffer<E> x, uint32 n) { Gen.twice<E>(y, x, n); }\n"
    "}\n"
    "public class ViaClass {\n"
    "    /** Mismatches through a generic class's instance method (the shape Tensor<T> has). */\n"
    "    public static int32 viaClassF32() {\n"
    "        uint32 n = 100;\n"
    "        float32[] h = heap float32[100];\n"
    "        int32 i = 0;\n"
    "        while (i < 100) { h[i] = (float32) i + 3.0f; i = i + 1; }\n"
    "        KernelBuffer<float32> x = heap KernelBuffer<float32>(0, n);\n"
    "        x.allocate();\n"
    "        x.upload(h);\n"
    "        KernelBuffer<float32> y = heap KernelBuffer<float32>(0, n);\n"
    "        y.allocate();\n"
    "        Holder<float32> b = heap Holder<float32>();\n"
    "        b.run(y, x, n);\n"
    "        float32[] g = heap float32[100];\n"
    "        y.download(g);\n"
    "        int32 bad = 0;\n"
    "        i = 0;\n"
    "        while (i < 100) { if (g[i] != h[i] + h[i]) { bad = bad + 1; } i = i + 1; }\n"
    "        x.free();\n"
    "        y.free();\n"
    "        return bad;\n"
    "    }\n"
    "}\n";

std::unique_ptr<CajetaJit> genericOnCpu() {
    return CajetaJit::compile(kGenericSource, "test.Gen", withBackend(cajeta::xpu::Backend::Cpu));
}

} // namespace

// 1.1.1 (spec 2.1): a generic kernel launched from a method generic over E runs for float32.
TEST(XpuGenericKernel, aGenericKernelLaunchedFromGenericCodeRunsForFloat32) {
    auto jit = genericOnCpu();
    ASSERT_NE(jit, nullptr);
    auto f32 = jit->lookup<int32_t (*)()>("f32");
    ASSERT_NE(f32, nullptr);
    EXPECT_EQ(f32(), 0);
}

// 1.1.1: and for float16.
TEST(XpuGenericKernel, aGenericKernelLaunchedFromGenericCodeRunsForFloat16) {
    auto jit = genericOnCpu();
    ASSERT_NE(jit, nullptr);
    auto f16 = jit->lookup<int32_t (*)()>("f16");
    ASSERT_NE(f16, nullptr);
    EXPECT_EQ(f16(), 0);
}

// 1.1.2: through a generic class's instance method, the shape Tensor<T> has.
TEST(XpuGenericKernel, aGenericClassReachesTheKernelThroughItsTypeParameter) {
    auto jit = genericOnCpu();
    ASSERT_NE(jit, nullptr);
    auto viaClass = jit->lookup<int32_t (*)()>("viaClassF32");
    ASSERT_NE(viaClass, nullptr);
    EXPECT_EQ(viaClass(), 0);
}

// 1.1.3: each concrete type registers one instantiation, shared by every call site of that type.
TEST(XpuGenericKernel, eachConcreteTypeRegistersOneInstantiation) {
    auto jit = genericOnCpu();
    ASSERT_NE(jit, nullptr);
    auto instantiations = jit->lookup<int32_t (*)()>("instantiations");
    auto f32Again = jit->lookup<int32_t (*)()>("f32Again");
    ASSERT_TRUE(instantiations && f32Again);
    EXPECT_EQ(f32Again(), 0);
    EXPECT_EQ(instantiations(), 2);
}
