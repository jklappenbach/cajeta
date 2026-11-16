//
// host-simd Unit 2 (specs/math-fma-host-spec.md): on the host, Math.fma and the
// native element-wise forms take a Vector<T,N> and keep its type, lane by lane.
// On v0.53.7 the host path widened every argument to float64, and Math.sqrt on a
// Vector<float32,4> stopped the backend ("Cannot select ... anyext from v4f32").
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    if (!jit) return -100;
    auto f = jit->lookup<int32_t (*)()>("run");
    return f ? f() : -101;
}

std::string errorOf(const std::string& src) {
    try {
        CajetaJit::compile(src, "test.D");
        return "";
    } catch (cajeta::Exception& e) {
        return e.getErrorId();
    } catch (const std::exception&) {
        return "<non-cajeta-exception>";
    }
}

} // namespace

// 2.1.1 — sixteen float32 lanes: the even lanes take the case that tells fused
// from unfused (2^-46 fused, 0 unfused), the odd lanes an ordinary 2 * 2 + 1.
TEST(HostMathVectorTests, aFloat32VectorFmaFusesEveryLane) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float32[] a = heap float32[16];\n"
        "        float32[] c = heap float32[16];\n"
        "        int32 i = 0;\n"
        "        while (i < 16) {\n"
        "            if (i % 2 == 0) { a[i] = 1.0f + 1.0f / 8388608.0f; c[i] = 0.0f - (1.0f + 2.0f / 8388608.0f); }\n"
        "            else { a[i] = 2.0f; c[i] = 1.0f; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        Vector<float32,16> va = a.vload<16>(0);\n"
        "        Vector<float32,16> vc = c.vload<16>(0);\n"
        "        float32[] out = heap float32[16];\n"
        "        out.vstore(0, Math.fma(va, va, vc));\n"
        "        float32 tiny = 1.0f / 70368744177664.0f;\n"   // 2^-46
        "        int32 bad = 0;\n"
        "        i = 0;\n"
        "        while (i < 16) {\n"
        "            float32 want = 5.0f;\n"
        "            if (i % 2 == 0) { want = tiny; }\n"
        "            if (out[i] != want) { bad = bad + 1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return bad;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(0, runI32(src));
}

// 2.1.1 — eight float64 lanes, the 2^-52 case giving 2^-104 in lane 0.
TEST(HostMathVectorTests, aFloat64VectorFmaFusesEveryLane) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float64[] a = heap float64[8];\n"
        "        float64[] c = heap float64[8];\n"
        "        int32 i = 0;\n"
        "        while (i < 8) { a[i] = 3.0; c[i] = 0.5; i = i + 1; }\n"
        "        a[0] = 1.0 + 1.0 / 4503599627370496.0;\n"
        "        c[0] = 0.0 - (1.0 + 2.0 / 4503599627370496.0);\n"
        "        float64[] out = heap float64[8];\n"
        "        out.vstore(0, Math.fma(a.vload<8>(0), a.vload<8>(0), c.vload<8>(0)));\n"
        "        float64 tiny = 1.0 / 4503599627370496.0 / 4503599627370496.0;\n"   // 2^-104
        "        int32 bad = 0;\n"
        "        if (out[0] != tiny) { bad = bad + 100; }\n"
        "        i = 1;\n"
        "        while (i < 8) { if (out[i] != 9.5) { bad = bad + 1; } i = i + 1; }\n"
        "        return bad;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(0, runI32(src));
}

// 2.1.2 — a scalar beside Vectors, or Vectors of two lengths, is refused.
TEST(HostMathVectorTests, mixedVectorOperandsAreRefused) {
    auto body = [](const std::string& call) {
        return std::string(
            "package test;\n"
            "public final class D {\n"
            "    public static int32 run() {\n"
            "        float32[] a = heap float32[16];\n"
            "        Vector<float32,16> v = a.vload<16>(0);\n"
            "        Vector<float32,8> w = a.vload<8>(0);\n"
            "        float32 s = 1.0f;\n"
            "        Vector<float32,16> r = ") + call + ";\n"
            "        return 0;\n"
            "    }\n"
            "}\n";
    };
    EXPECT_EQ("CAJETA_ERROR_MATH_FMA_OPERANDS", errorOf(body("Math.fma(v, v, s)")));
    EXPECT_EQ("CAJETA_ERROR_MATH_FMA_OPERANDS", errorOf(body("Math.fma(v, w, v)")));
    EXPECT_EQ("", errorOf(body("Math.fma(v, v, v)")));
}

// 2.1.3 — the native element-wise forms on a host Vector<float32,4>, lane by
// lane. round keeps Java's floor(x + 0.5): -2.5 rounds to -2.
TEST(HostMathVectorTests, theNativeFormsWorkLaneByLaneOnFloat32) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    static int32 same(Vector<float32,4> v, float32 x, float32 y, float32 z, float32 w) {\n"
        "        if (v.x == x && v.y == y && v.z == z && v.w == w) { return 0; }\n"
        "        return 1;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        float32[] sq = [4.0f, 9.0f, 16.0f, 25.0f];\n"
        "        float32[] m = [-2.5f, 2.5f, -1.4f, 1.6f];\n"
        "        Vector<float32,4> s = sq.vload<4>(0);\n"
        "        Vector<float32,4> v = m.vload<4>(0);\n"
        "        int32 bad = 0;\n"
        "        bad = bad + D.same(Math.sqrt(s), 2.0f, 3.0f, 4.0f, 5.0f);\n"
        "        bad = bad + 2 * D.same(Math.abs(v), 2.5f, 2.5f, 1.4f, 1.6f);\n"
        "        bad = bad + 4 * D.same(Math.floor(v), -3.0f, 2.0f, -2.0f, 1.0f);\n"
        "        bad = bad + 8 * D.same(Math.ceil(v), -2.0f, 3.0f, -1.0f, 2.0f);\n"
        "        bad = bad + 16 * D.same(Math.trunc(v), -2.0f, 2.0f, -1.0f, 1.0f);\n"
        "        bad = bad + 32 * D.same(Math.round(v), -2.0f, 3.0f, -1.0f, 2.0f);\n"
        "        return bad;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(0, runI32(src));
}

// 2.1.3 — the same on Vector<float64,4>.
TEST(HostMathVectorTests, theNativeFormsWorkLaneByLaneOnFloat64) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    static int32 same(Vector<float64,4> v, float64 x, float64 y, float64 z, float64 w) {\n"
        "        if (v.x == x && v.y == y && v.z == z && v.w == w) { return 0; }\n"
        "        return 1;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        float64[] sq = [4.0, 9.0, 16.0, 25.0];\n"
        "        float64[] m = [-2.5, 2.5, -1.4, 1.6];\n"
        "        Vector<float64,4> s = sq.vload<4>(0);\n"
        "        Vector<float64,4> v = m.vload<4>(0);\n"
        "        int32 bad = 0;\n"
        "        bad = bad + D.same(Math.sqrt(s), 2.0, 3.0, 4.0, 5.0);\n"
        "        bad = bad + 2 * D.same(Math.abs(v), 2.5, 2.5, 1.4, 1.6);\n"
        "        bad = bad + 4 * D.same(Math.floor(v), -3.0, 2.0, -2.0, 1.0);\n"
        "        bad = bad + 8 * D.same(Math.ceil(v), -2.0, 3.0, -1.0, 2.0);\n"
        "        bad = bad + 16 * D.same(Math.trunc(v), -2.0, 2.0, -1.0, 1.0);\n"
        "        bad = bad + 32 * D.same(Math.round(v), -2.0, 3.0, -1.0, 2.0);\n"
        "        return bad;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(0, runI32(src));
}
