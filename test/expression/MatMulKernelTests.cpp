//
// SIMD numeric kernels Unit 3 — vectorized matmul kernel.
// The ikj kernel with a Vector<float64,8> inner row-update (scalar-broadcast
// FMA) + scalar tail, checked against a hand-computable result. With A = all-1
// and B[k][j] = k, C[i][j] = Σ_{k<n} k, so Σ C = n*n * (n-1)*n/2.
// n=8 (no tail) -> 64*28 = 1792; n=10 (tail j=8,9) -> 100*45 = 4500.
// Traces simd-numeric-kernels-spec.md §3.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

// A vectorized ikj matmul for an n x n matrix (A=all 1.0, B[k][j]=k), returning
// the checksum of C. n is interpolated into the source.
double matmulSum(int n) {
    std::string ns = std::to_string(n);
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static float64 run() {\n"
        "        int32 n = " + ns + ";\n"
        "        int32 sz = n * n;\n"
        "        float64[] a = heap float64[sz];\n"
        "        float64[] b = heap float64[sz];\n"
        "        float64[] c = heap float64[sz];\n"
        "        int32 i = 0;\n"
        "        while (i < n) {\n"
        "            int32 j = 0;\n"
        "            while (j < n) {\n"
        "                a[i * n + j] = 1.0;\n"
        "                b[i * n + j] = (float64) i;\n"
        "                c[i * n + j] = 0.0;\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        // Register-accumulated vectorized matmul (the shipped MatMulBench kernel):
        // keep the C j-block in one register across the k loop, store once.
        "        int32 jLimit = n - (n % 8);\n"
        "        int32 ii = 0;\n"
        "        while (ii < n) {\n"
        "            int32 iBase = ii * n;\n"
        "            int32 jb = 0;\n"
        "            while (jb < jLimit) {\n"
        "                Vector<float64,8> cv =\n"
        "                    stack Vector<float64,8>(0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0);\n"
        "                int32 kk = 0;\n"
        "                while (kk < n) {\n"
        "                    cv = cv + Cajeta.vload8f64(b, kk * n + jb) * a[iBase + kk];\n"
        "                    kk = kk + 1;\n"
        "                }\n"
        "                Cajeta.vstore8f64(cv, c, iBase + jb);\n"
        "                jb = jb + 8;\n"
        "            }\n"
        "            while (jb < n) {\n"
        "                float64 acc = 0.0;\n"
        "                int32 kk = 0;\n"
        "                while (kk < n) {\n"
        "                    acc = acc + a[iBase + kk] * b[kk * n + jb];\n"
        "                    kk = kk + 1;\n"
        "                }\n"
        "                c[iBase + jb] = acc;\n"
        "                jb = jb + 1;\n"
        "            }\n"
        "            ii = ii + 1;\n"
        "        }\n"
        "        float64 s = 0.0;\n"
        "        int32 m = 0;\n"
        "        while (m < sz) { s = s + c[m]; m = m + 1; }\n"
        "        return s;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto f = jit->lookup<double (*)()>("run");
    return f ? f() : -1.0;
}

} // namespace

// 3.1.1 — vectorized matmul correct on a non-identity B (n divisible by 8, no tail).
TEST(MatMulKernelTests, vectorizedExactNoTail) {
    EXPECT_DOUBLE_EQ(matmulSum(8), 1792.0);
}

// 3.1.2 — tail correctness: n not a multiple of 8 (j=8,9 handled scalar-ly).
TEST(MatMulKernelTests, vectorizedExactWithTail) {
    EXPECT_DOUBLE_EQ(matmulSum(10), 4500.0);
}
