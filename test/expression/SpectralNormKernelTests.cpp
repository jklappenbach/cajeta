//
// benchmark-fidelity Unit 6 — vectorized CLBG spectral-norm.
// The hot loop is two matrix-vector products with A(i,j)=1/((i+j)(i+j+1)/2+i+1)
// recomputed on the fly (no stored matrix, same algorithm as the C++/Rust CLBG
// entries — which the native compilers auto-vectorize at -O3 -march=native).
// This vectorizes the inner j-loop in float64: the integer denominator
// (i+j)(i+j+1)/2 is ≤ 39402, exactly representable in float64, so ones/dv is
// bit-identical to the scalar reciprocal; only the reduction reassociates.
// The end-to-end eigenvalue (≈1.274224 at N=100) is the correctness oracle.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

// Full vectorized spectral-norm at n; returns sqrt(vBv/vv). The atAv body is the
// exact logic ported into SpectralNormBench (the test guards that port).
double spectralNorm(int n) {
    std::string ns = std::to_string(n);
    std::string src =
        "package test;\n"
        "import cajeta.lang.Math;\n"
        "public final class SN {\n"
        "    public static void atAv(float64[] v, float64[] out, float64[] tmp, int32 n) {\n"
        "        int32 jlim = n - (n % 8);\n"
        "        Vector<float64,8> iota = stack Vector<float64,8>(0.0,1.0,2.0,3.0,4.0,5.0,6.0,7.0);\n"
        "        Vector<float64,8> ones = stack Vector<float64,8>(1.0,1.0,1.0,1.0,1.0,1.0,1.0,1.0);\n"
        // tmp = A * v :  d(i,j) = (i+j)(i+j+1)/2 + i + 1
        "        int32 i = 0;\n"
        "        while (i < n) {\n"
        "            Vector<float64,8> acc = stack Vector<float64,8>(0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0);\n"
        "            int32 j = 0;\n"
        "            while (j < jlim) {\n"
        "                float64 base = (float64)(i + j);\n"
        "                Vector<float64,8> ijv = iota + base;\n"
        "                Vector<float64,8> dv = ijv * (ijv + ones) * 0.5 + (float64)(i + 1);\n"
        "                Vector<float64,8> av = ones / dv;\n"
        "                acc = acc + av * Cajeta.vload8f64(v, j);\n"
        "                j = j + 8;\n"
        "            }\n"
        "            float64 sum = Cajeta.vsum8f64(acc);\n"
        "            while (j < n) {\n"
        "                int32 ij = i + j; int32 d = ij * (ij + 1) / 2 + i + 1;\n"
        "                sum = sum + (1.0 / (float64) d) * v[j];\n"
        "                j = j + 1;\n"
        "            }\n"
        "            tmp[i] = sum;\n"
        "            i = i + 1;\n"
        "        }\n"
        // out = A^T * tmp :  d(j,i) = (i+j)(i+j+1)/2 + j + 1
        "        i = 0;\n"
        "        while (i < n) {\n"
        "            Vector<float64,8> acc = stack Vector<float64,8>(0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0);\n"
        "            int32 j = 0;\n"
        "            while (j < jlim) {\n"
        "                float64 base = (float64)(i + j);\n"
        "                Vector<float64,8> ijv = iota + base;\n"
        "                Vector<float64,8> addv = iota + (float64)(j + 1);\n"
        "                Vector<float64,8> dv = ijv * (ijv + ones) * 0.5 + addv;\n"
        "                Vector<float64,8> av = ones / dv;\n"
        "                acc = acc + av * Cajeta.vload8f64(tmp, j);\n"
        "                j = j + 8;\n"
        "            }\n"
        "            float64 sum = Cajeta.vsum8f64(acc);\n"
        "            while (j < n) {\n"
        "                int32 ij = j + i; int32 d = ij * (ij + 1) / 2 + j + 1;\n"
        "                sum = sum + (1.0 / (float64) d) * tmp[j];\n"
        "                j = j + 1;\n"
        "            }\n"
        "            out[i] = sum;\n"
        "            i = i + 1;\n"
        "        }\n"
        "    }\n"
        "    public static float64 run() {\n"
        "        int32 n = " + ns + ";\n"
        "        float64[] u = heap float64[n];\n"
        "        float64[] v = heap float64[n];\n"
        "        float64[] tmp = heap float64[n];\n"
        "        int32 i = 0;\n"
        "        while (i < n) { u[i] = 1.0; i = i + 1; }\n"
        "        int32 it = 0;\n"
        "        while (it < 10) { atAv(u, v, tmp, n); atAv(v, u, tmp, n); it = it + 1; }\n"
        "        float64 vBv = 0.0; float64 vv = 0.0; i = 0;\n"
        "        while (i < n) { vBv = vBv + u[i] * v[i]; vv = vv + v[i] * v[i]; i = i + 1; }\n"
        "        return Math.sqrt(vBv / vv);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.SN");
    auto f = jit->lookup<double (*)()>("run");
    return f ? f() : -1.0;
}

} // namespace

// 6.a.1 — vectorized spectral-norm converges to the known N=100 eigenvalue,
// proving the vectorized atAv matches the scalar matrix-vector products. The
// tail path (n%8 != 0) is exercised since 100 % 8 == 4.
TEST(SpectralNormKernelTests, eigenvalueN100) {
    EXPECT_NEAR(spectralNorm(100), 1.274224, 1e-4);
}

// N=96 is a multiple of 8 (no scalar tail), guarding the vectorized-only path.
// The eigenvalue has nearly converged by N≈100, so it sits close to 1.2742.
TEST(SpectralNormKernelTests, eigenvalueN96NoTail) {
    EXPECT_NEAR(spectralNorm(96), 1.2742, 1e-2);
}
