//
// TensorDeviceOpsTests — cajeta-ml-gpu Unit 2 (2.1.1, 2.1.2; spec §2.3, §3.6).
//
// Tensor's generic elementwise ops reach a kernel when both operands are
// device-resident, through the generic kernel instantiation Unit 1 added, and an
// op with no kernel for its placement fails by name instead of pulling the
// caller's tensors back to the host.
//
// Device placement uses the portable CPU XPU backend in-process, the same
// discipline as StorageCoherenceTests.
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::unique_ptr<CajetaJit> compileXpu(const std::string& src) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    return CajetaJit::compile(src, "test.D", o);
}

const char* PRE =
    "package test;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.math.Tensor;\n"
    "import cajeta.math.PlacementException;\n"
    "import cajeta.xpu.Device;\n";

// a[i] = i + 1, b[i] = 2 over eight elements: every result of the four ops is
// exact in float32, float16 and bfloat16. Returns 0, or a code naming the first
// failed check: 1 launch count did not rise by four, 2 an input left the device,
// 3 a result is not on the device, 10 + op a wrong value.
std::string arithSource(const std::string& dt, const std::string& kernel) {
    return std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        " + dt + "[] da = heap " + dt + "[8];\n"
        "        " + dt + "[] db = heap " + dt + "[8];\n"
        "        int32 i = 0;\n"
        "        while (i < 8) {\n"
        "            da[i] = (" + dt + ") ((float32) i + 1.0f);\n"
        "            db[i] = (" + dt + ") 2.0f;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        int64[] s = heap int64[1]; s[0] = 8;\n"
        "        Tensor<" + dt + "> a #= Tensor.of<" + dt + ">(da, s);\n"
        "        Tensor<" + dt + "> b #= Tensor.of<" + dt + ">(db, s);\n"
        "        a.gpu(); b.gpu();\n"
        "        int64 before = Device.kernelLaunchCount(\"" + kernel + "\");\n"
        "        Tensor<" + dt + "> c #= Tensor.add<" + dt + ">(a, b);\n"
        "        Tensor<" + dt + "> d #= Tensor.sub<" + dt + ">(a, b);\n"
        "        Tensor<" + dt + "> e #= Tensor.mul<" + dt + ">(a, b);\n"
        "        Tensor<" + dt + "> f #= Tensor.div<" + dt + ">(a, b);\n"
        "        int64 after = Device.kernelLaunchCount(\"" + kernel + "\");\n"
        "        if (after - before != 4) { return 1; }\n"
        "        if (!a.isOnGpu() || !b.isOnGpu()) { return 2; }\n"
        "        if (!c.isOnGpu() || !d.isOnGpu() || !e.isOnGpu() || !f.isOnGpu()) { return 3; }\n"
        "        c.cpu(); d.cpu(); e.cpu(); f.cpu();\n"
        "        i = 0;\n"
        "        while (i < 8) {\n"
        "            float32 x = (float32) i + 1.0f;\n"
        "            if ((float32) c.get1(i) != x + 2.0f) { return 10; }\n"
        "            if ((float32) d.get1(i) != x - 2.0f) { return 11; }\n"
        "            if ((float32) e.get1(i) != x * 2.0f) { return 12; }\n"
        "            if ((float32) f.get1(i) != x / 2.0f) { return 13; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
}

int32_t runArith(const std::string& dt, const std::string& kernel) {
    auto jit = compileXpu(arithSource(dt, kernel));
    if (!jit) return -1;
    auto fn = jit->lookup<int32_t (*)()>("run");
    if (!fn) return -2;
    return fn();
}

} // namespace

// 2.1.1 — add/sub/mul/div on two device-resident float32 tensors launch the
// generic arith kernel's float32 instantiation and leave every tensor on the
// device.
//
// RED until 2.2.1/2.2.2: the generic ops download both operands in place and
// run the host loop; there is no `div<E>` and no generic arith kernel.
TEST(TensorDeviceOpsTests, deviceArithmeticRunsAKernelAndStaysOnDeviceF32) {
    EXPECT_EQ(runArith("float32", "arith_float32"), 0);
}

// 2.1.1 — the same at float16.
TEST(TensorDeviceOpsTests, deviceArithmeticRunsAKernelAndStaysOnDeviceF16) {
    EXPECT_EQ(runArith("float16", "arith_float16"), 0);
}

// 2.1.1 — the same at bfloat16.
TEST(TensorDeviceOpsTests, deviceArithmeticRunsAKernelAndStaysOnDeviceBf16) {
    EXPECT_EQ(runArith("bfloat16", "arith_bfloat16"), 0);
}

// 2.1.1 — device operands that are offset views bind as views of their buffers:
// adding [3..6] and [5..8] sliced out of one device tensor reads the views'
// elements, and the result lives on the device with no host mirror to release.
TEST(TensorDeviceOpsTests, deviceArithmeticOnOffsetViewsReadsTheViews) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float32[] d = [ 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f ];\n"
        "        int64[] s = heap int64[1]; s[0] = 8;\n"
        "        Tensor<float32> t #= Tensor.of<float32>(d, s);\n"
        "        t.gpu();\n"
        "        Tensor<float32> v1 #= t.slice(0, 2, 6);\n"
        "        Tensor<float32> v2 #= t.slice(0, 4, 8);\n"
        "        Tensor<float32> c #= Tensor.add<float32>(v1, v2);\n"
        "        if (!c.isOnGpu() || !t.isOnGpu()) { return 1; }\n"
        "        if (c.size() != 4) { return 2; }\n"
        "        c.cpu();\n"
        "        int32 i = 0;\n"
        "        while (i < 4) {\n"
        "            float32 want = (float32) (2 * i + 8);\n"
        "            if (c.get1(i) != want) { return 10 + i; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = compileXpu(src);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0);
}

// 2.1.2 — an op with no kernel for its placement throws naming the op, and the
// caller's tensors keep their placement: neither `sort` nor the generic `matmul`
// has a device path.
//
// RED until 2.2.1: the guard notes `[op-tiering]` and downloads in place.
TEST(TensorDeviceOpsTests, anOpWithoutAKernelThrowsByNameAndLeavesPlacement) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float32[] d = [ 3.0f, 1.0f, 2.0f, 4.0f ];\n"
        "        int64[] s = heap int64[1]; s[0] = 4;\n"
        "        Tensor<float32> t #= Tensor.of<float32>(d, s);\n"
        "        t.gpu();\n"
        "        boolean named = false;\n"
        "        try {\n"
        "            Tensor<float32> r #= Tensor.sort<float32>(t, 0);\n"
        "        } catch (PlacementException ex) {\n"
        "            named = ex.message.contains(\"Tensor.sort\");\n"
        "        }\n"
        "        if (!named) { return 1; }\n"
        "        if (!t.isOnGpu()) { return 2; }\n"
        "        int64[] s2 = heap int64[2]; s2[0] = 2; s2[1] = 2;\n"
        "        Tensor<float32> sq #= Tensor.of<float32>(d, s2);\n"
        "        sq.gpu();\n"
        "        named = false;\n"
        "        try {\n"
        "            Tensor<float32> r2 #= Tensor.matmul<float32>(sq, sq);\n"
        "        } catch (PlacementException ex) {\n"
        "            named = ex.message.contains(\"Tensor.matmul\");\n"
        "        }\n"
        "        if (!named) { return 3; }\n"
        "        if (!t.isOnGpu() || !sq.isOnGpu()) { return 4; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    auto jit = compileXpu(src);
    ASSERT_NE(jit, nullptr);
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0);
}
