//
// benchmark-fidelity Unit 5 — vectorized SAXPY kernel (z = a·x + y).
// Vector<float64,8> load / broadcast-scalar FMA / store + a Vector checksum
// accumulator (vsum8f64) + scalar tail. With a=2, x[i]=i+1, y[i]=i the result
// z[i]=3i+2 is integer-valued, so the checksum Σ(3i+2) is exact under SIMD
// reassociation: n=32 (no tail) -> 1552; n=40 (tail 32..39) -> 2420.
// Traces simd-numeric-kernels-spec.md §4 (saxpy).
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

// Returns the saxpy checksum AND leaves z[] written; run() returns the checksum
// so the test can assert the closed form. (z correctness is implied: the
// checksum is a sum over every z[i].)
double saxpyChecksum(int n) {
    std::string ns = std::to_string(n);
    std::string src =
        "package test;\n"
        "public final class S {\n"
        "    public static float64 run() {\n"
        "        int32 n = " + ns + ";\n"
        "        float64 a = 2.0;\n"
        "        float64[] x = heap float64[n];\n"
        "        float64[] y = heap float64[n];\n"
        "        float64[] z = heap float64[n];\n"
        "        int32 i = 0;\n"
        "        while (i < n) { x[i] = (float64)(i + 1); y[i] = (float64) i; i = i + 1; }\n"
        "        Vector<float64,8> acc = stack Vector<float64,8>(0.0,0.0,0.0,0.0,0.0,0.0,0.0,0.0);\n"
        "        int32 limit = n - (n % 8);\n"
        "        i = 0;\n"
        "        while (i < limit) {\n"
        "            Vector<float64,8> zv = Cajeta.vload8f64(x, i) * a + Cajeta.vload8f64(y, i);\n"
        "            Cajeta.vstore8f64(zv, z, i);\n"
        "            acc = acc + zv;\n"
        "            i = i + 8;\n"
        "        }\n"
        "        float64 sum = Cajeta.vsum8f64(acc);\n"
        "        while (i < n) { z[i] = a * x[i] + y[i]; sum = sum + z[i]; i = i + 1; }\n"
        "        return sum;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.S");
    auto f = jit->lookup<double (*)()>("run");
    return f ? f() : -1.0;
}

} // namespace

// 5.a.1 — vectorized saxpy checksum, n divisible by 8 (no tail). Σ(3i+2), n=32.
TEST(SaxpyKernelTests, vectorizedExactNoTail) {
    EXPECT_DOUBLE_EQ(saxpyChecksum(32), 1552.0);
}

// 5.a.1 — tail correctness, n not a multiple of 8. Σ(3i+2), n=40.
TEST(SaxpyKernelTests, vectorizedExactWithTail) {
    EXPECT_DOUBLE_EQ(saxpyChecksum(40), 2420.0);
}
