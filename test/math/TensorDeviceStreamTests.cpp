//
// TensorDeviceStreamTests — cajeta-ml-gpu Unit 4 (4.1.1–4.1.3; spec §3.2, §3.3).
//
// Out-param and in-place device ops allocate nothing; a run of device ops is
// ordered on the stream and syncs once, at the host read; a broadcast-shaped or
// strided elementwise op runs a kernel.
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runOnCpu(const std::string& src) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(src, "test.D", o);
    if (!jit) return -1;
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn ? fn() : -2;
}

const char* PRE =
    "package test;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.math.Tensor;\n"
    "import cajeta.math.Ewise;\n"
    "import cajeta.math.PlacementException;\n"
    "import cajeta.xpu.Device;\n";

// a = [1..8], b = 2 everywhere, both on the device.
const char* AB =
    "        float32[] da = [ 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f ];\n"
    "        float32[] db = [ 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f ];\n"
    "        int64[] s = heap int64[1]; s[0] = 8;\n"
    "        Tensor<float32> a #= Tensor.of<float32>(da, s);\n"
    "        Tensor<float32> b #= Tensor.of<float32>(db, s);\n"
    "        a.gpu(); b.gpu();\n";

} // namespace

// 4.1.1 — `addInto(out, a, b)` and `a.addInPlace(b)` on the device allocate
// nothing: the device allocation count is unchanged across them.
//
// RED until 4.2.1: neither form exists.
TEST(TensorDeviceStreamTests, outParamAndInPlaceOpsAllocateNothing) {
    std::string real = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n" + AB +
        "        Tensor<float32> out #= Tensor.add<float32>(a, b);\n"
        "        int64 before = Device.deviceAllocationCount();\n"
        "        Tensor.subInto<float32>(out, a, b);\n"
        "        Tensor.mulInto<float32>(out, out, b);\n"
        "        Tensor.divInto<float32>(out, out, b);\n"
        "        a.addInPlace(b);\n"
        "        a.mulInPlace(b);\n"
        "        int64 after = Device.deviceAllocationCount();\n"
        "        if (after != before) { return 1; }\n"
        "        if (!out.isOnGpu() || !a.isOnGpu()) { return 2; }\n"
        "        out.cpu(); a.cpu();\n"
        "        int32 i = 0;\n"
        "        while (i < 8) {\n"
        "            float32 x = (float32) i + 1.0f;\n"
        "            if (out.get1(i) != x - 2.0f) { return 10 + i; }\n"   // ((x-2)*2)/2
        "            if (a.get1(i) != (x + 2.0f) * 2.0f) { return 20 + i; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runOnCpu(real), 0);
}

// 4.1.2 — ten queued device ops sync nowhere; the host read syncs once, and the
// result matches the synced form (a + 10b).
//
// RED until 4.2.2: every op syncs the stream.
TEST(TensorDeviceStreamTests, queuedOpsSyncOnlyAtTheHostRead) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n" + AB +
        "        int64 before = Device.streamSyncCount();\n"
        "        Tensor<float32> acc #= Tensor.add<float32>(a, b);\n"
        "        int32 k = 1;\n"
        "        while (k < 10) { Tensor.addInto<float32>(acc, acc, b); k = k + 1; }\n"
        "        int64 mid = Device.streamSyncCount();\n"
        "        if (mid != before) { return 1; }\n"
        "        acc.cpu();\n"
        "        int64 after = Device.streamSyncCount();\n"
        "        if (after < mid + 1) { return 2; }\n"
        "        int32 i = 0;\n"
        "        while (i < 8) {\n"
        "            if (acc.get1(i) != (float32) i + 1.0f + 20.0f) { return 10 + i; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runOnCpu(src), 0);
}

// 4.1.3 — a broadcast-shaped elementwise op on device tensors runs a kernel, and
// so does one with a strided (transposed) operand.
//
// RED until 4.2.3: both are refused by name.
TEST(TensorDeviceStreamTests, broadcastAndStridedOperandsRunAKernel) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n" + AB +
        "        float32[] d1 = [ 3.0f ];\n"
        "        int64[] s1 = heap int64[1]; s1[0] = 1;\n"
        "        Tensor<float32> one #= Tensor.of<float32>(d1, s1);\n"
        "        one.gpu();\n"
        "        int64 before = Device.kernelLaunchCount(\"arithBroadcast_float32\");\n"
        "        Tensor<float32> c #= Tensor.add<float32>(a, one);\n"
        "        if (Device.kernelLaunchCount(\"arithBroadcast_float32\") != before + 1) { return 1; }\n"
        "        if (!c.isOnGpu() || c.size() != 8) { return 2; }\n"
        "        c.cpu();\n"
        "        if (c.get1(0) != 4.0f || c.get1(7) != 11.0f) { return 3; }\n"
        "        float32[] md = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
        "        int64[] ms = heap int64[2]; ms[0] = 2; ms[1] = 2;\n"
        "        Tensor<float32> m #= Tensor.of<float32>(md, ms);\n"
        "        m.gpu();\n"
        "        Tensor<float32> mt #= m.transpose();\n"
        "        Tensor<float32> sum #= Tensor.add<float32>(m, mt);\n"   // [[2,5],[5,8]]
        "        sum.cpu();\n"
        "        if (sum.get2(0, 1) != 5.0f || sum.get2(1, 0) != 5.0f || sum.get2(1, 1) != 8.0f) { return 4; }\n"
        "        if (!m.isOnGpu()) { return 5; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runOnCpu(src), 0);
}

// 4.2.1 — matmul's out-param form writes into the caller's device tensor and
// allocates nothing.
TEST(TensorDeviceStreamTests, matmulIntoAllocatesNothing) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float32[] md = heap float32[256];\n"
        "        float32[] id = heap float32[256];\n"
        "        int32 q = 0;\n"
        "        while (q < 256) { md[q] = (float32) (q / 16 + 1); id[q] = 0.0f; if (q / 16 == q % 16) { id[q] = 1.0f; } q = q + 1; }\n"
        "        int64[] ms = heap int64[2]; ms[0] = 16; ms[1] = 16;\n"
        "        Tensor<float32> m #= Tensor.of<float32>(md, ms);\n"
        "        Tensor<float32> e #= Tensor.of<float32>(id, ms);\n"
        "        Tensor<float32> out #= Tensor.zeros<float32>(ms);\n"
        "        m.gpu(); e.gpu(); out.gpu();\n"
        "        int64 before = Device.deviceAllocationCount();\n"
        "        Ewise.matmulF32Into(out, m, e);\n"
        "        if (Device.deviceAllocationCount() != before) { return 1; }\n"
        "        out.cpu();\n"
        "        if (out.get2(0, 0) != 1.0f || out.get2(15, 15) != 16.0f || out.get2(3, 7) != 4.0f) { return 2; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runOnCpu(src), 0);
}
