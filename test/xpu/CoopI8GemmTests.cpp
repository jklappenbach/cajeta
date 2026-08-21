//
// CoopI8GemmTests — cajeta-llama Unit 18 (18.1.8/18.1.9/18.1.10, the
// 18.2.3/18.2.4 device seams): the int8 → int32 cooperative-matrix GEMM
// and the DP4a dot, host-verifiable halves. Integer accumulation has no
// rounding, so "agrees within tolerance" collapses to EXACT equality —
// asserted that way.
//
// Real-silicon halves (RDNA3 v_wmma_i32_16x16x16_iu8, Vulkan DP4a) ride
// the device-tests workflow; here the in-process CPU backend runs the
// portable software tier and the tier NOTE is pinned (18.1.10).
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32Xpu(const std::string& src, std::string* errOut = nullptr) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D", o);
    auto fn = jit->lookup<int32_t (*)()>("run");
    int32_t r = fn();
    std::string err = testing::internal::GetCapturedStderr();
    if (errOut) *errOut = err;
    return r;
}

const char* PRE =
    "package test;\n"
    "import cajeta.math.Tensor;\n"
    "import cajeta.math.Ewise;\n";

} // namespace

// 18.1.8 — the int8 GEMM's device path agrees with the portable floor
// EXACTLY (integer accumulation), on a 32x32x32 product of signed values.
// 18.1.10 — the [mma-tiering] note records which path ran: on the CPU
// backend int8 has no native coop config, so the software-tier note fires.
TEST(CoopI8GemmTests, deviceAgreesWithFloorExactlyAndTierNoteFires) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] s = heap int64[2]; s[0] = 32; s[1] = 32;\n"
        "        Tensor<int8> a #= Tensor.zeros<int8>(s);\n"
        "        Tensor<int8> b #= Tensor.zeros<int8>(s);\n"
        "        int64 i = 0;\n"
        "        while (i < 32) {\n"
        "            int64 j = 0;\n"
        "            while (j < 32) {\n"
        "                a.set2(i, j, (int8) ((i * 5 + j * 3) % 17 - 8));\n"
        "                b.set2(i, j, (int8) ((i * 7 + j * 11) % 15 - 7));\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        Tensor<int32> refr #= Ewise.matmulI8Op(a, b);\n"
        "        a.gpu(); b.gpu();\n"
        "        Tensor<int32> dev #= Ewise.matmulI8Op(a, b);\n"
        "        dev.cpu();\n"
        "        i = 0;\n"
        "        while (i < 32) {\n"
        "            int64 j = 0;\n"
        "            while (j < 32) {\n"
        "                if (dev.get2(i, j) != refr.get2(i, j)) { return -1; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    std::string err;
    EXPECT_EQ(runI32Xpu(src, &err), 1);
    EXPECT_NE(err.find("[mma-tiering]"), std::string::npos)
        << "expected the tier note recording the software path; stderr:\n"
        << err;
    EXPECT_NE(err.find("int8"), std::string::npos);
}

// 18.1.9 — the DP4a seam: Vector<int8,4>.dot (idotWiden on the host,
// hardware DP4a on Vulkan) agrees with an explicit widen-multiply-reduce,
// accumulator included.
TEST(CoopI8GemmTests, dp4aDotAgreesWithWidenMultiplyReduce) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Vector<int8,4> a = heap Vector<int8,4>((int8)-8, (int8)7, (int8)127, (int8)-128);\n"
        "        Vector<int8,4> b = heap Vector<int8,4>((int8)3, (int8)-5, (int8)2, (int8)1);\n"
        "        int32 got = a.dot(b, 100);\n"
        "        int32 want = 100;\n"
        "        int32 i = 0;\n"
        "        while (i < 4) {\n"
        "            want = want + (int32) a[i] * (int32) b[i];\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (got != want) { return -1; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 18.2.3's SPIR-V half, measured: does the Vulkan lowering carry the
// KHR integer cooperative-matrix shape? Either outcome is acceptable —
// lowering (a Vulkan int8 device path) or the graceful skip (portable
// tier serves Vulkan) — but it must be RECORDED and must not abort.
// As of 2026-08-20 this pins whichever the backend does today.
TEST(CoopI8GemmTests, spirvI8CoopLowersOrSkipsGracefully) {
    std::string src =
        "package test;\n"
        "import cajeta.xpu.KernelBuffer;\n"
        "import cajeta.xpu.CooperativeMatrix;\n"
        "public final class D {\n"
        "    @Kernel\n"
        "    public static void gemmI8(KernelBuffer<int32> c, KernelBuffer<int8> a,\n"
        "                              KernelBuffer<int8> b, uint32 depth) {\n"
        "        CooperativeMatrix<int32,16,16,2> mc;\n"
        "        mc.splat(0);\n"
        "        CooperativeMatrix<int8,16,16,0> ma;\n"
        "        CooperativeMatrix<int8,16,16,1> mb;\n"
        "        ma.load(a, 0, 0, depth);\n"
        "        mb.load(b, 0, 0, depth);\n"
        "        mc.mma(ma, mb);\n"
        "        mc.store(c, 0, 0, depth);\n"
        "    }\n"
        "    public static int32 run() { return 1; }\n"
        "}\n";
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Spirv};
    testing::internal::CaptureStderr();
    auto jit = CajetaJit::compile(src, "test.D", o);
    auto fn = jit->lookup<int32_t (*)()>("run");
    int32_t r = fn();
    std::string err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(r, 1);
    bool skipped = err.find("[xpu-kernel-skipped]") != std::string::npos;
    printf("[18.2.3] SPIR-V int8 coop: %s\n",
           skipped ? "SKIPPED (portable tier serves Vulkan)"
                   : "LOWERED (Vulkan int8 device path)");
    SUCCEED();
}
