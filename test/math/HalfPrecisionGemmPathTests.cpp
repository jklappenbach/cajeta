//
// HalfPrecisionGemmPathTests — cajeta-llama 2.2.5 + 2.2.6: the half-precision
// GEMM paths that Unit 2 deferred.
//
//   2.2.5 — ragged (non-multiple-of-16) f16/bf16 shapes take a PADDED DEVICE
//   path (padF16/padBf16 + crop siblings, the matmulF32Into recipe) instead
//   of staging device operands back to the host floor. The §2.6 bug class —
//   the tiled kernel owns whole 16x16 tiles and truncates every dimension —
//   is what the pad/crop guards against, so the ragged tests would catch a
//   silent 16-tile truncation as a value mismatch.
//
//   2.2.6 — bf16 GEMM gets a WIDE-ACCUMULATOR path where the backend can
//   launch it: `matmulBf16Wide` (bf16 A/B tiles, f32 accumulator — native
//   silicon on NVIDIA sm_80+ and AMD RDNA3+, software-tier on CPU, skipped on
//   Vulkan which has no bf16 coop config), selected per-backend at the op
//   layer via Device.supports(Capability.CoopMatrixBf16F32Acc). The all-bf16
//   kernel stays as the portable fallback so Vulkan keeps a device path.
//
// The accumulator-width discriminator needs care here — the obvious all-ones
// product does NOT discriminate: each 16-deep mma step adds 16, and every
// multiple of 16 up to 4096 is exactly representable in bf16, so a bf16
// accumulator reaches 4096 without a single rounding (measured: the all-bf16
// kernel returned 4096 on the first draft of these tests). The honest
// discriminator adds ONE per K-step: A all-ones, B a picket fence
// (b[p][j] = 1 when p % 16 == 0, else 0) at k = 9600 — 600 mma steps each
// contributing exactly 1. An f32 accumulator reaches 600 (exact in bf16:
// 600 = 2^9 * 1.171875, 6 mantissa bits, so the narrow is lossless); a bf16
// accumulator sticks at 256, where +1 lands mid-ulp and ties-to-even rounds
// straight back. 600 PROVES f32 accumulation; 256 proves bf16.
//
// Device tests use the portable CPU XPU backend in-process (the
// StorageCoherenceTests discipline); real-silicon confirmation rides the
// device-tests workflow.
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32Xpu(const std::string& src) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(src, "test.D", o);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* PRE =
    "package test;\n"
    "import cajeta.math.Tensor;\n"
    "import cajeta.math.Ewise;\n"
    "import cajeta.xpu.Device;\n"
    "import cajeta.xpu.Capability;\n";

} // namespace

// 2.2.5 — a ragged f16 GEMM on device operands matches the host floor
// exactly, and the operands' placement survives. Values are small integers
// (exact in f16), every partial sum exact in f32, so device tiling order
// cannot introduce rounding and the comparison is `!=`, not a tolerance.
// Shape (17,33)x(33,18) is ragged on every dimension — a kernel that
// truncated to whole 16-tiles would drop row 16, col 16+, and the K tail,
// all of which carry nonzero values here.
//
// GREEN ON ARRIVAL, deliberately: today's staging fallback computes the same
// numbers on the host floor, and no in-language observable separates the two
// routes. What this test guards is the padded device path REPLACING that
// fallback — a pad/crop defect (the §2.6 truncation class) breaks equality
// here the moment 2.2.5's rewrite lands.
TEST(HalfPrecisionGemmPathTests, raggedF16DeviceGemmMatchesHostFloor) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] as = heap int64[2]; as[0] = 17; as[1] = 33;\n"
        "        int64[] bs = heap int64[2]; bs[0] = 33; bs[1] = 18;\n"
        "        Tensor<float16> a #= Tensor.zeros<float16>(as);\n"
        "        Tensor<float16> b #= Tensor.zeros<float16>(bs);\n"
        "        int64 i = 0;\n"
        "        while (i < 17) {\n"
        "            int64 j = 0;\n"
        "            while (j < 33) {\n"
        "                a.set2(i, j, (float16) ((float32) ((i * 3 + j * 5) % 7 - 3)));\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        i = 0;\n"
        "        while (i < 33) {\n"
        "            int64 j = 0;\n"
        "            while (j < 18) {\n"
        "                b.set2(i, j, (float16) ((float32) ((i * 2 + j * 3) % 5 - 2)));\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        Tensor<float32> refr #= Ewise.matmulF16Op(a, b);\n"
        "        a.gpu(); b.gpu();\n"
        "        Tensor<float32> dev #= Ewise.matmulF16Op(a, b);\n"
        "        if (!a.isOnGpu()) { return -1; }\n"
        "        if (!b.isOnGpu()) { return -2; }\n"
        "        dev.cpu();\n"
        "        i = 0;\n"
        "        while (i < 17) {\n"
        "            int64 j = 0;\n"
        "            while (j < 18) {\n"
        "                if (dev.get2(i, j) != refr.get2(i, j)) { return -3; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 2.2.5 at bfloat16. Values in [-2,2] keep every partial sum's magnitude
// under 133 — integers are exact in bf16 to 256, so even a per-K-step bf16
// accumulation stays exact and the device/host comparison is equality
// whichever accumulator the selected kernel carries.
TEST(HalfPrecisionGemmPathTests, raggedBf16DeviceGemmMatchesHostFloor) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] as = heap int64[2]; as[0] = 17; as[1] = 33;\n"
        "        int64[] bs = heap int64[2]; bs[0] = 33; bs[1] = 18;\n"
        "        Tensor<bfloat16> a #= Tensor.zeros<bfloat16>(as);\n"
        "        Tensor<bfloat16> b #= Tensor.zeros<bfloat16>(bs);\n"
        "        int64 i = 0;\n"
        "        while (i < 17) {\n"
        "            int64 j = 0;\n"
        "            while (j < 33) {\n"
        "                a.set2(i, j, (bfloat16) ((float32) ((i * 3 + j * 5) % 5 - 2)));\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        i = 0;\n"
        "        while (i < 33) {\n"
        "            int64 j = 0;\n"
        "            while (j < 18) {\n"
        "                b.set2(i, j, (bfloat16) ((float32) ((i * 2 + j * 3) % 5 - 2)));\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        Tensor<bfloat16> refr #= Ewise.matmulBf16Op(a, b);\n"
        "        a.gpu(); b.gpu();\n"
        "        Tensor<bfloat16> dev #= Ewise.matmulBf16Op(a, b);\n"
        "        if (!a.isOnGpu()) { return -1; }\n"
        "        if (!b.isOnGpu()) { return -2; }\n"
        "        dev.cpu();\n"
        "        i = 0;\n"
        "        while (i < 17) {\n"
        "            int64 j = 0;\n"
        "            while (j < 18) {\n"
        "                if ((float32) dev.get2(i, j) != (float32) refr.get2(i, j)) { return -3; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 2.2.6 — the capability is queryable, and on the CPU backend it reports
// LAUNCHABLE (the software cooperative-matrix tier runs the bf16/f32 tile
// mix), which is what routes matmulBf16Op's device path to the wide kernel
// in this very suite.
TEST(HalfPrecisionGemmPathTests, bf16WideCapabilityLaunchableOnCpuBackend) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        if (Device.supports(Capability.CoopMatrixBf16F32Acc)) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 2.2.6 — matmulBf16WideOp accumulates at f32 and returns f32: the
// picket-fence product (see the header) reaches exactly 600, impossible for
// a bf16 accumulator (sticks at 256). Both routes checked: host floor first,
// then the device path.
TEST(HalfPrecisionGemmPathTests, bf16WideOpAccumulatesAtF32) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] as = heap int64[2]; as[0] = 16; as[1] = 9600;\n"
        "        int64[] bs = heap int64[2]; bs[0] = 9600; bs[1] = 16;\n"
        "        Tensor<bfloat16> a #= Tensor.ones<bfloat16>(as);\n"
        "        Tensor<bfloat16> b #= Tensor.zeros<bfloat16>(bs);\n"
        "        int64 p = 0;\n"
        "        while (p < 9600) {\n"
        "            int64 j = 0;\n"
        "            while (j < 16) {\n"
        "                b.set2(p, j, (bfloat16) 1.0);\n"
        "                j = j + 1;\n"
        "            }\n"
        "            p = p + 16;\n"
        "        }\n"
        "        Tensor<float32> host #= Ewise.matmulBf16WideOp(a, b);\n"
        "        if (host.get2(0, 0) != 600.0f) { return -1; }\n"
        "        if (host.get2(15, 15) != 600.0f) { return -2; }\n"
        "        a.gpu(); b.gpu();\n"
        "        Tensor<float32> dev #= Ewise.matmulBf16WideOp(a, b);\n"
        "        dev.cpu();\n"
        "        if (dev.get2(0, 0) != 600.0f) { return -3; }\n"
        "        if (dev.get2(15, 15) != 600.0f) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 2.2.6 — matmulBf16Op itself rides the wide kernel where the capability
// reports launchable: the picket-fence product returns (bf16) 600 — exactly
// representable, losslessly narrowed — where the all-bf16 device path sticks
// at 256. THE discriminating pin that the per-backend selection actually
// switched paths: RED at 256 until the selection lands.
TEST(HalfPrecisionGemmPathTests, bf16OpTakesWideAccumulatorWhereLaunchable) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] as = heap int64[2]; as[0] = 16; as[1] = 9600;\n"
        "        int64[] bs = heap int64[2]; bs[0] = 9600; bs[1] = 16;\n"
        "        Tensor<bfloat16> a #= Tensor.ones<bfloat16>(as);\n"
        "        Tensor<bfloat16> b #= Tensor.zeros<bfloat16>(bs);\n"
        "        int64 p = 0;\n"
        "        while (p < 9600) {\n"
        "            int64 j = 0;\n"
        "            while (j < 16) {\n"
        "                b.set2(p, j, (bfloat16) 1.0);\n"
        "                j = j + 1;\n"
        "            }\n"
        "            p = p + 16;\n"
        "        }\n"
        "        a.gpu(); b.gpu();\n"
        "        Tensor<bfloat16> dev #= Ewise.matmulBf16Op(a, b);\n"
        "        dev.cpu();\n"
        "        if ((float32) dev.get2(0, 0) != 600.0f) { return -1; }\n"
        "        if ((float32) dev.get2(15, 15) != 600.0f) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}
