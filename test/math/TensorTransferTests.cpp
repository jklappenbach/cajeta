//
// TensorTransferTests — cajeta-ml-gpu Unit 5 (5.1.1; spec §3.4).
//
// Moving a tensor to the device and back is one bulk transfer each way and
// round-trips bit-exact; the Tensor-to-KernelBuffer bridge moves a view's
// elements in one transfer without a staging copy.
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
    "import cajeta.xpu.Device;\n"
    "import cajeta.xpu.KernelBuffer;\n";

} // namespace

// 5.1.1 — a 1e7-element tensor moves to the device in one transfer and back in
// one, and every element returns bit-exact.
//
// RED until the transfer counter exists (Device.transferCount).
TEST(TensorTransferTests, deviceMoveIsOneBulkTransferEachWay) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 n = 10000000L;\n"
        "        float32[] d = heap float32[n];\n"
        "        int64 i = 0;\n"
        "        while (i < n) { d[(int32) i] = (float32) i * 0.37f - 1.0f; i = i + 1; }\n"
        "        int64[] s = heap int64[1]; s[0] = n;\n"
        "        Tensor<float32> t #= Tensor.of<float32>(d, s);\n"
        "        int64 t0 = Device.transferCount();\n"
        "        t.gpu();\n"
        "        if (Device.transferCount() != t0 + 1) { return 1; }\n"
        "        t.cpu();\n"
        "        if (Device.transferCount() != t0 + 2) { return 2; }\n"
        "        float32[] back = t.hostBuffer();\n"
        "        int64 base = t.hostBase();\n"
        "        i = 0;\n"
        "        while (i < n) {\n"
        "            if (back[(int32) (base + i)] != d[(int32) i]) { return 3; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runOnCpu(src), 0);
}

// 5.2.1 — the bridge: a contiguous host view uploads into a caller's KernelBuffer
// in one transfer and downloads back in one, with no staging array.
TEST(TensorTransferTests, bridgeMovesAViewInOneTransfer) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float32[] d = heap float32[1000];\n"
        "        int32 i = 0;\n"
        "        while (i < 1000) { d[i] = (float32) i; i = i + 1; }\n"
        "        int64[] s = heap int64[1]; s[0] = 1000;\n"
        "        Tensor<float32> t #= Tensor.of<float32>(d, s);\n"
        "        Tensor<float32> v #= t.slice(0, 3, 503);\n"
        "        KernelBuffer<float32> buf = heap KernelBuffer<float32>(500);\n"
        "        int64 t0 = Device.transferCount();\n"
        "        v.uploadTo(buf);\n"
        "        if (Device.transferCount() != t0 + 1) { return 1; }\n"
        "        float32[] got = heap float32[500];\n"
        "        buf.download(got);\n"
        "        if (got[0] != 3.0f || got[499] != 502.0f) { return 2; }\n"
        "        Tensor<float32> w #= Tensor.zeros<float32>(heap int64[1]);\n"
        "        int64[] ws = heap int64[1]; ws[0] = 500;\n"
        "        Tensor<float32> z #= Tensor.zeros<float32>(ws);\n"
        "        int64 t1 = Device.transferCount();\n"
        "        z.downloadFrom(buf);\n"
        "        if (Device.transferCount() != t1 + 1) { return 3; }\n"
        "        if (z.get1(0) != 3.0f || z.get1(499) != 502.0f) { return 4; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runOnCpu(src), 0);
}
