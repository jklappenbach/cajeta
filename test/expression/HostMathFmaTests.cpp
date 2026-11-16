//
// host-simd Unit 1 (specs/math-fma-host-spec.md): `Math.fma(a, b, c)` in host
// code is one fused multiply-add at its operands' own precision. On v0.53.7 a
// host call failed with CAJETA_ERROR_MEMBER_NOT_FOUND, though the Math class
// lists fma among the intrinsics native everywhere.
//
// The case that tells fused from unfused: a = 1 + 2^-23, c = -(1 + 2^-22).
// a * a = 1 + 2^-22 + 2^-46 exactly, which rounds to 1 + 2^-22 at float32, so
// a * a + c is 0 unfused and 2^-46 fused. The float64 analogue uses 2^-52 and
// gives 2^-104.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cmath>
#include <string>

using cajeta_test::CajetaJit;

namespace {

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

const char* f32Case =
    "package test;\n"
    "public final class D {\n"
    "    static float32 one(float32 x) { return x; }\n"
    "    public static float32 fused() {\n"
    "        float32 a = D.one(1.0f) + D.one(1.0f) / 8388608.0f;\n"
    "        float32 c = 0.0f - (D.one(1.0f) + D.one(2.0f) / 8388608.0f);\n"
    "        return Math.fma(a, a, c);\n"
    "    }\n"
    "    public static float32 unfused() {\n"
    "        float32 a = D.one(1.0f) + D.one(1.0f) / 8388608.0f;\n"
    "        float32 c = 0.0f - (D.one(1.0f) + D.one(2.0f) / 8388608.0f);\n"
    "        return a * a + c;\n"
    "    }\n"
    "}\n";

} // namespace

// 1.1.1 — float32: fused gives 2^-46 where the separate multiply and add give 0.
TEST(HostMathFmaTests, aFloat32FmaRoundsOnce) {
    auto jit = CajetaJit::compile(f32Case, "test.D");
    ASSERT_NE(jit, nullptr);
    auto fused = jit->lookup<float (*)()>("fused");
    auto unfused = jit->lookup<float (*)()>("unfused");
    ASSERT_NE(fused, nullptr);
    ASSERT_NE(unfused, nullptr);
    EXPECT_EQ(fused(), std::ldexp(1.0f, -46));
    EXPECT_EQ(unfused(), 0.0f);
}

// 1.1.1 — float64: a = 1 + 2^-52, c = -(1 + 2^-51); fused gives 2^-104.
TEST(HostMathFmaTests, aFloat64FmaRoundsOnce) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    static float64 one(float64 x) { return x; }\n"
        "    public static float64 fused() {\n"
        "        float64 a = D.one(1.0) + D.one(1.0) / 4503599627370496.0;\n"
        "        float64 c = 0.0 - (D.one(1.0) + D.one(2.0) / 4503599627370496.0);\n"
        "        return Math.fma(a, a, c);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    ASSERT_NE(jit, nullptr);
    auto fused = jit->lookup<double (*)()>("fused");
    ASSERT_NE(fused, nullptr);
    EXPECT_EQ(fused(), std::ldexp(1.0, -104));
}

// 1.1.3 — float32 operands lower to llvm.fma.f32, not an f64 fma around casts
// (that would round twice and is not a fused multiply-add).
TEST(HostMathFmaTests, aFloat32FmaIsTheF32Intrinsic) {
    CajetaJit::Options opts;
    opts.captureIr = true;
    auto jit = CajetaJit::compile(f32Case, "test.D", opts);
    ASSERT_NE(jit, nullptr);
    const std::string& ir = jit->getModuleIr();
    EXPECT_NE(ir.find("@llvm.fma.f32("), std::string::npos);
    EXPECT_EQ(ir.find("@llvm.fma.f64("), std::string::npos);
}

// 1.1.2 — integer operands, the wrong count, and mixed float32 and float64
// operands are compile errors that name the call.
TEST(HostMathFmaTests, badOperandsAreRefused) {
    auto body = [](const std::string& call) {
        return std::string(
            "package test;\n"
            "public final class D {\n"
            "    public static float64 run() {\n"
            "        float32 f = 1.0f;\n"
            "        float64 g = 2.0;\n"
            "        int32 i = 3;\n"
            "        return (float64) ") + call + ";\n"
            "    }\n"
            "}\n";
    };
    EXPECT_EQ("CAJETA_ERROR_MATH_FMA_OPERANDS", errorOf(body("Math.fma(i, i, i)")));
    EXPECT_EQ("CAJETA_ERROR_MATH_FMA_OPERANDS", errorOf(body("Math.fma(f, f)")));
    EXPECT_EQ("CAJETA_ERROR_MATH_FMA_OPERANDS", errorOf(body("Math.fma(f, g, f)")));
    EXPECT_EQ("", errorOf(body("Math.fma(f, f, f)")));
    EXPECT_EQ("", errorOf(body("Math.fma(g, g, g)")));
}
