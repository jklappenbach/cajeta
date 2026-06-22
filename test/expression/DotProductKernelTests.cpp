//
// SIMD numeric kernels Unit 4 — vectorized dot-product kernel.
// Σ x[i]·y[i] with 4 independent Vector<float64,8> accumulators (hides FMA
// latency on a memory-bound reduction) + horizontal sum + scalar tail. With
// x[i]=y[i]=i+1 the dot is Σ k² = n(n+1)(2n+1)/6: n=32 (no tail) -> 11440;
// n=40 (tail 33²..40²) -> 22140. Integer-valued, so reassociation is exact.
// Traces simd-numeric-kernels-spec.md §4.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

double dotSquares(int n) {
    std::string ns = std::to_string(n);
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static float64 run() {\n"
        "        int32 n = " + ns + ";\n"
        "        float64[] x = heap float64[n];\n"
        "        float64[] y = heap float64[n];\n"
        "        int32 i = 0;\n"
        "        while (i < n) { x[i] = (float64)(i + 1); y[i] = (float64)(i + 1); i = i + 1; }\n"
        "        Vector<float64,8> a0 = stack Vector<float64,8>(0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0);\n"
        "        Vector<float64,8> a1 = stack Vector<float64,8>(0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0);\n"
        "        Vector<float64,8> a2 = stack Vector<float64,8>(0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0);\n"
        "        Vector<float64,8> a3 = stack Vector<float64,8>(0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0);\n"
        "        int32 limit = n - (n % 32);\n"
        "        i = 0;\n"
        "        while (i < limit) {\n"
        "            a0 = a0 + Cajeta.vload8f64(x, i)      * Cajeta.vload8f64(y, i);\n"
        "            a1 = a1 + Cajeta.vload8f64(x, i + 8)  * Cajeta.vload8f64(y, i + 8);\n"
        "            a2 = a2 + Cajeta.vload8f64(x, i + 16) * Cajeta.vload8f64(y, i + 16);\n"
        "            a3 = a3 + Cajeta.vload8f64(x, i + 24) * Cajeta.vload8f64(y, i + 24);\n"
        "            i = i + 32;\n"
        "        }\n"
        "        float64 sum = Cajeta.vsum8f64(a0 + a1 + a2 + a3);\n"
        "        while (i < n) { sum = sum + x[i] * y[i]; i = i + 1; }\n"
        "        return sum;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto f = jit->lookup<double (*)()>("run");
    return f ? f() : -1.0;
}

} // namespace

// 4.1.1 — vectorized dot correct, n divisible by 32 (no tail). Σ_{1..32} k² = 11440.
TEST(DotProductKernelTests, vectorizedExactNoTail) {
    EXPECT_DOUBLE_EQ(dotSquares(32), 11440.0);
}

// 4.1.2 — tail correctness, n not a multiple of 32. Σ_{1..40} k² = 22140.
TEST(DotProductKernelTests, vectorizedExactWithTail) {
    EXPECT_DOUBLE_EQ(dotSquares(40), 22140.0);
}
