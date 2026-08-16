//
// HalfPrecisionTests — cajeta-llama plan Unit 2 (2.1.1–2.1.4): f16/bf16
// compute across the Tensor op surface and the Ewise kernel seam.
//
// Scope correction, recorded here because the test shapes follow from it:
// Unit 2 was planned on the belief that Tensor's ops are f32-pinned. They are
// not — probed against main 2026-08-09, Tensor<float16> and Tensor<bfloat16>
// construct, and add / sum / matmul all return correct values on the host
// path. The spec's "no half-precision tensor is instantiated anywhere in the
// repo" (§2.5) describes the repo's USAGE, not a capability gap.
//
// So 2.1.1 below is a CHARACTERIZATION test: green on arrival, pinning
// behaviour that already works so the device work cannot silently regress it.
// The red tests are the device ones — Ewise has 65 f32 references and zero
// f16/bf16 entry points, so half-precision has no kernel path at all, and
// (per 1.2.2) a dtype-generic Tensor op can never reach one.
//
// Device tests use the portable CPU XPU backend in-process, the same
// discipline as PlacementDispatchTests. Real-silicon validation rides the
// device-tests workflow on the GPU runners.
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
    "import cajeta.math.Ewise;\n";

} // namespace

// 2.1.1 — f16/bf16 construct and compute on the host path without widening.
// GREEN ON ARRIVAL by design (see the header): this pins today's behaviour.
//
// Values are chosen to be exactly representable in both f16 and bf16 so the
// assertions test the op, not the rounding. bf16 has 8 mantissa bits; f16 has
// 10; small integers and halves are exact in both.
TEST(HalfPrecisionTests, halfPrecisionComputesOnHost) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] s = heap int64[1]; s[0] = 4;\n"
        // ---- float16 ----
        "        float16[] hd = [ (float16) 1.0, (float16) 2.0, (float16) 3.0, (float16) 4.0 ];\n"
        "        Tensor<float16> h #= Tensor.of<float16>(hd, s);\n"
        "        Tensor<float16> h2 #= Tensor.add<float16>(h, h);\n"
        "        if ((float32) h2.get1(0) != 2.0f) { return -1; }\n"
        "        if ((float32) h2.get1(3) != 8.0f) { return -2; }\n"
        // the result stayed f16 — no silent widening of the tensor's dtype
        "        if (h2.dtype().bits() != 16) { return -3; }\n"
        // ---- bfloat16 ----
        "        bfloat16[] bd = [ (bfloat16) 1.0, (bfloat16) 2.0, (bfloat16) 3.0, (bfloat16) 4.0 ];\n"
        "        Tensor<bfloat16> b #= Tensor.of<bfloat16>(bd, s);\n"
        "        Tensor<bfloat16> b2 #= Tensor.add<bfloat16>(b, b);\n"
        "        if ((float32) b2.get1(2) != 6.0f) { return -4; }\n"
        "        if (b2.dtype().bits() != 16) { return -5; }\n"
        // ---- matmul at both dtypes ----
        "        int64[] m = heap int64[2]; m[0] = 2; m[1] = 2;\n"
        "        Tensor<float16> p #= Tensor.ones<float16>(m);\n"
        "        Tensor<float16> r #= Tensor.matmul<float16>(p, p);\n"
        "        if ((float32) r.get2(0, 0) != 2.0f) { return -6; }\n"
        "        Tensor<bfloat16> q #= Tensor.ones<bfloat16>(m);\n"
        "        Tensor<bfloat16> rb #= Tensor.matmul<bfloat16>(q, q);\n"
        "        if ((float32) rb.get2(1, 1) != 2.0f) { return -7; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 2.1.4 — a reduction over f16/bf16 storage accumulates at f32.
//
// This is the numerically load-bearing case: 2048 copies of 1.0 summed IN f16
// stops at 2048 exactly (f16 has 10 mantissa bits, so 2048+1 rounds back to
// 2048 — the classic stagnation), while an f32 accumulator reaches 4096. The
// assertion is therefore a real discriminator between the two accumulator
// widths, not a tolerance check that either would pass.
TEST(HalfPrecisionTests, reductionAccumulatesAtF32OverHalfStorage) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] s = heap int64[1]; s[0] = 4096;\n"
        "        Tensor<float16> h #= Tensor.ones<float16>(s);\n"
        "        float32 wide = Tensor.sum<float16, float32>(h);\n"
        "        if (wide != 4096.0f) { return -1; }\n"       // f16 accumulation would stall at 2048
        "        Tensor<bfloat16> b #= Tensor.ones<bfloat16>(s);\n"
        "        float32 wideB = Tensor.sum<bfloat16, float32>(b);\n"
        "        if (wideB != 4096.0f) { return -2; }\n"      // bf16 stalls even earlier (8 bits)
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 2.1.2 — an f16 GEMM through the Ewise kernel seam agrees with an f32
// reference within a stated tolerance.
//
// RED until 2.2.2/2.2.3: `Ewise.matmulF16Op` does not exist. Ewise carries 65
// f32 references and zero f16 entry points, so half-precision has no device
// path — and per 1.2.2 a generic `Tensor.matmul<float16>` can never reach a
// kernel, which is why this must go through a concrete Ewise entry point.
//
// The op returns Tensor<float32>, not Tensor<float16>: WMMA's f16 config
// accumulates at f32 and `store` writes the accumulator's dtype, so f32 is the
// shape the hardware hands back. Narrowing to f16 (for layer chaining /
// memory) is the caller's explicit, visibly lossy step.
//
// Tolerance: operands are exact in f16 and the reduction is over k=16, so the
// only error is the accumulator's. With the f32 accumulator that
// XpuTorchConfigProbeTests already uses for WMMA, agreement should be tight;
// 1e-2 leaves room for a software-tier fallback without admitting a wrong
// answer.
TEST(HalfPrecisionTests, f16GemmAgreesWithF32Reference) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] m = heap int64[2]; m[0] = 16; m[1] = 16;\n"
        "        Tensor<float16> a #= Tensor.zeros<float16>(m);\n"
        "        Tensor<float16> b #= Tensor.zeros<float16>(m);\n"
        "        Tensor<float32> af #= Tensor.zeros<float32>(m);\n"
        "        Tensor<float32> bf #= Tensor.zeros<float32>(m);\n"
        "        int64 i = 0;\n"
        "        while (i < 16) {\n"
        "            int64 j = 0;\n"
        "            while (j < 16) {\n"
        "                float32 va = (float32) ((i + j) % 4);\n"
        "                float32 vb = (float32) ((i * j) % 3);\n"
        "                a.set2(i, j, (float16) va);\n"
        "                b.set2(i, j, (float16) vb);\n"
        "                af.set2(i, j, va);\n"
        "                bf.set2(i, j, vb);\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        Tensor<float32> ref #= Ewise.matmulF32Op(af, bf);\n"
        "        Tensor<float32> got #= Ewise.matmulF16Op(a, b);\n"
        "        int64 r = 0;\n"
        "        while (r < 16) {\n"
        "            int64 c = 0;\n"
        "            while (c < 16) {\n"
        "                float32 d = ref.get2(r, c) - got.get2(r, c);\n"
        "                if (d < 0.0f) { d = 0.0f - d; }\n"
        "                if (d > 0.01f) { return -1; }\n"
        "                c = c + 1;\n"
        "            }\n"
        "            r = r + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 2.1.3 — bf16 on a backend with no native cooperative-matrix config falls to
// the software tier and still agrees with the host reference.
//
// The CPU XPU backend exposes no native config for ANY dtype (the
// `[mma-tiering]` note fires for f32 and f64 alike on it), so this test's
// backend is exactly the "no native config" case by construction.
//
// Unlike matmulF16Op, this op returns Tensor<bfloat16>: no Vulkan driver
// exposes a bf16 cooperative-matrix config, so on SPIR-V a bf16 tile is
// software-tier while an f32 accumulator tile would be native-tier, and a
// kernel's tiles must share one tier. All-bf16 tiles keep the GEMM lowerable
// everywhere — the accumulator-width cost is documented on the op (2.3.2).
//
// RED until 2.2.2: `Ewise.matmulBf16Op` does not exist.
TEST(HalfPrecisionTests, bf16FallsToSoftwareTierAndAgrees) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] m = heap int64[2]; m[0] = 16; m[1] = 16;\n"
        "        Tensor<bfloat16> a #= Tensor.ones<bfloat16>(m);\n"
        "        Tensor<bfloat16> b #= Tensor.ones<bfloat16>(m);\n"
        "        a.gpu();\n"
        "        b.gpu();\n"
        "        Tensor<bfloat16> got #= Ewise.matmulBf16Op(a, b);\n"
        "        got.cpu();\n"
        "        int64 r = 0;\n"
        "        while (r < 16) {\n"
        "            int64 c = 0;\n"
        "            while (c < 16) {\n"
        // every row-col dot is 16 * (1*1) = 16, exact at every width in play
        "                if ((float32) got.get2(r, c) != 16.0f) { return -1; }\n"
        "                c = c + 1;\n"
        "            }\n"
        "            r = r + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 2.2.2 (elementwise + reduction families) — half-precision has a device path
// beyond GEMM, and the device reduction widens to f32 (2.1.4 on the device
// side, stated in `reduceSumF16`'s signature per 2.3.2).
//
// The sum is the discriminator: 4096 elements of 2.0 total 8192 in f32, while
// an f16 accumulator stalls at 4096 (the spacing there is 4, so +2 rounds
// away) and a bf16 one stalls far earlier. Exact equality tests the
// accumulator's width, not a tolerance.
TEST(HalfPrecisionTests, deviceElementwiseAndWideningReductionAtHalf) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] s = heap int64[1]; s[0] = 4096;\n"
        // ---- float16 ----
        "        Tensor<float16> a #= Tensor.ones<float16>(s);\n"
        "        Tensor<float16> b #= Tensor.ones<float16>(s);\n"
        "        a.gpu();\n"
        "        b.gpu();\n"
        "        Tensor<float16> c #= Ewise.arithF16Op(a, b, 0);\n"   // 2.0 everywhere, device-resident
        "        float32 total = Ewise.sumF16(c);\n"                 // device reduction, f32 accumulator
        "        if (total != 8192.0f) { return -1; }\n"
        "        c.cpu();\n"
        "        if ((float32) c.get1(0) != 2.0f) { return -2; }\n"
        "        if (c.dtype().bits() != 16) { return -3; }\n"       // elementwise kept the storage dtype
        // ---- bfloat16 ----
        "        Tensor<bfloat16> ba #= Tensor.ones<bfloat16>(s);\n"
        "        Tensor<bfloat16> bb #= Tensor.ones<bfloat16>(s);\n"
        "        ba.gpu();\n"
        "        bb.gpu();\n"
        "        Tensor<bfloat16> bc #= Ewise.arithBf16Op(ba, bb, 0);\n"
        "        float32 btotal = Ewise.sumBf16(bc);\n"
        "        if (btotal != 8192.0f) { return -4; }\n"
        "        bc.cpu();\n"
        "        if ((float32) bc.get1(4095) != 2.0f) { return -5; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}
