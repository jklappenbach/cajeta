//
// transform-intrinsics Unit 3 — Grad end-to-end (spec §5). `Grad(f)` walks f's
// lambda body, reverse-composes the VJP rules into a Tier-A backward, and returns
// a callable `(P) -> GradResult<V,G>`. Validated on the JIT path.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {
float runF32(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class G {\n"
        "    public static float32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.G");
    auto fn = jit->lookup<float (*)()>("run");
    return fn();
}
std::string compileErrorId(const std::string& body) {
    std::string src =
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class G {\n"
        "    public static float32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    try { CajetaJit::compile(src, "test.G"); }
    catch (cajeta::Exception& e) { return e.getErrorId(); }
    return "";
}
} // namespace

// 3.1.1 — Grad((float32 x) -> x*x) yields {value: x*x, grads: 2x}. Encode both in
// one float: value*100 + grads = 9*100 + 6 = 906.
TEST(GradEndToEnd, squareValueAndGrad) {
    EXPECT_FLOAT_EQ(runF32(
        "(float32) -> GradResult<float32,float32> g = Grad((float32 x) -> x * x);\n"
        "        GradResult<float32,float32> r = g(3.0f);\n"
        "        return r.value * 100.0f + r.grads;"), 906.0f);
}

// 3.1.2 — grads are explicit return values: calling the differentiated function
// twice gives identical results (no global accumulation).
TEST(GradEndToEnd, gradIsPureNoAccumulation) {
    EXPECT_FLOAT_EQ(runF32(
        "(float32) -> GradResult<float32,float32> g = Grad((float32 x) -> x * x);\n"
        "        GradResult<float32,float32> a = g(3.0f);\n"
        "        GradResult<float32,float32> b = g(3.0f);\n"
        "        return a.grads + b.grads;"), 12.0f);
}

// A composite body: d/dx (x + x) via the add rule (identity cotangents) = 2.
TEST(GradEndToEnd, addBodyGrad) {
    EXPECT_FLOAT_EQ(runF32(
        "(float32) -> GradResult<float32,float32> g = Grad((float32 x) -> x + x);\n"
        "        GradResult<float32,float32> r = g(4.0f);\n"
        "        return r.value * 100.0f + r.grads;"), 802.0f);  // value 8, grads 2
}

// Nested ops: d/dx ((x*x) - x) = 2x - 1 = 5 at x=3; value = 9 - 3 = 6.
TEST(GradEndToEnd, nestedMulSubGrad) {
    EXPECT_FLOAT_EQ(runF32(
        "(float32) -> GradResult<float32,float32> g = Grad((float32 x) -> x * x - x);\n"
        "        GradResult<float32,float32> r = g(3.0f);\n"
        "        return r.value * 100.0f + r.grads;"), 605.0f);  // value 6, grads 5
}

// A misspelled/unsupported primitive fails with a named compile error, never a
// silent wrong gradient (spec §5.3). Division has no VJP rule in v1.
TEST(GradEndToEnd, unsupportedOpNamedError) {
    EXPECT_EQ(compileErrorId(
        "(float32) -> GradResult<float32,float32> g = Grad((float32 x) -> x / x);\n"
        "        return g(2.0f).value;"), "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
}
