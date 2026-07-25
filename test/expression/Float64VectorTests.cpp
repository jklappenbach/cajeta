//
// SIMD numeric kernels Unit 2 — float64 vector infrastructure.
// Exercises Cajeta.vload8f64 / vstore8f64 / vsum8f64 + element ops on
// Vector<float64,8> (the float64 analogue of the int64 SIMD path xxhash3 uses).
// Traces simd-numeric-kernels-spec.md §2.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

double runF64(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto f = jit->lookup<double (*)()>("run");
    return f ? f() : -1.0;
}

const char* head =
    "package test;\n"
    "public final class D {\n"
    "    public static float64 run() {\n"
    "        float64[] a = [ 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0 ];\n";
const char* tail = "    }\n}\n";

} // namespace

// vload8f64 + element multiply + vsum8f64: Σ k² for k=1..8 = 204.
TEST(Float64VectorTests, loadMulSum) {
    std::string src = std::string(head) +
        "        Vector<float64,8> v = Cajeta.vload8f64(a, 0);\n"
        "        return Cajeta.vsum8f64(v * v);\n" + tail;
    EXPECT_DOUBLE_EQ(runF64(src), 204.0);
}

// vstore8f64 roundtrip: store squares back, reload, sum + spot-check a lane.
// a becomes {1,4,9,16,25,36,49,64}; a[3]=16, Σ=204 -> 220.
TEST(Float64VectorTests, storeRoundtrip) {
    std::string src = std::string(head) +
        "        Vector<float64,8> v = Cajeta.vload8f64(a, 0);\n"
        "        Cajeta.vstore8f64(v * v, a, 0);\n"
        "        return a[3] + Cajeta.vsum8f64(Cajeta.vload8f64(a, 0));\n" + tail;
    EXPECT_DOUBLE_EQ(runF64(src), 220.0);
}

// FMA pattern acc + a*b on Vector<float64,8>: Σ(k + k²) = 36 + 204 = 240.
TEST(Float64VectorTests, fmaPattern) {
    std::string src = std::string(head) +
        "        Vector<float64,8> v = Cajeta.vload8f64(a, 0);\n"
        "        return Cajeta.vsum8f64(v + v * v);\n" + tail;
    EXPECT_DOUBLE_EQ(runF64(src), 240.0);
}
