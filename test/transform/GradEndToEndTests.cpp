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

// Numeric literals in the body are constants (zero cotangent): d/dx (2x + 1) = 2.
// value at x=3 is 7, grad 2 -> 7*100 + 2 = 702.
TEST(GradEndToEnd, literalsInBody) {
    EXPECT_FLOAT_EQ(runF32(
        "(float32) -> GradResult<float32,float32> g = Grad((float32 x) -> 2.0f * x + 1.0f);\n"
        "        GradResult<float32,float32> r = g(3.0f);\n"
        "        return r.value * 100.0f + r.grads;"), 702.0f);
}

// A parameter-independent body has an identically ZERO gradient — never a spurious
// nonzero from a defaulted node index. value 3, grad 0 -> 3*100 + 0 = 300.
TEST(GradEndToEnd, paramUnusedZeroGrad) {
    EXPECT_FLOAT_EQ(runF32(
        "(float32) -> GradResult<float32,float32> g = Grad((float32 x) -> 3.0f + 4.0f);\n"
        "        GradResult<float32,float32> r = g(5.0f);\n"
        "        return r.value * 100.0f + r.grads;"), 700.0f);  // value 7, grad 0
}

// 3.1.4 — multi-arg f, default argnum 0: Grad((float32 x, float32 y) -> x*y)
// differentiates w.r.t. arg 0, so value = x*y, grads = d/dx = y. The backward
// lambda takes BOTH params. At (3,4): value 12, grad 4 -> 12*100 + 4 = 1204.
TEST(GradEndToEnd, multiArgDefaultArgnumZero) {
    EXPECT_FLOAT_EQ(runF32(
        "(float32,float32) -> GradResult<float32,float32> g =\n"
        "            Grad((float32 x, float32 y) -> x * y);\n"
        "        GradResult<float32,float32> r = g(3.0f, 4.0f);\n"
        "        return r.value * 100.0f + r.grads;"), 1204.0f);
}

// 3.1.4 — argnum selection: Grad<1>(f) differentiates w.r.t. arg 1, so
// grads = d/dy (x*y) = x = 3. value 12, grad 3 -> 12*100 + 3 = 1203.
TEST(GradEndToEnd, multiArgArgnumOne) {
    EXPECT_FLOAT_EQ(runF32(
        "(float32,float32) -> GradResult<float32,float32> g =\n"
        "            Grad<1>((float32 x, float32 y) -> x * y);\n"
        "        GradResult<float32,float32> r = g(3.0f, 4.0f);\n"
        "        return r.value * 100.0f + r.grads;"), 1203.0f);
}

// 3.1.4 — the SELECTED arg drives the zero-grad path: Grad<1>((x,y) -> x*x) is
// independent of y, so grads = 0 even though arg 0 has a nonzero gradient.
// value 9, grad 0 -> 9*100 + 0 = 900.
TEST(GradEndToEnd, multiArgUnusedSelectedArgZeroGrad) {
    EXPECT_FLOAT_EQ(runF32(
        "(float32,float32) -> GradResult<float32,float32> g =\n"
        "            Grad<1>((float32 x, float32 y) -> x * x);\n"
        "        GradResult<float32,float32> r = g(3.0f, 4.0f);\n"
        "        return r.value * 100.0f + r.grads;"), 900.0f);
}

// 3.1.4 — an out-of-range argnum is a named, located compile error, not a crash.
TEST(GradEndToEnd, argnumOutOfRangeNamedError) {
    EXPECT_EQ(compileErrorId(
        "(float32,float32) -> GradResult<float32,float32> g =\n"
        "            Grad<5>((float32 x, float32 y) -> x * y);\n"
        "        return g(1.0f, 2.0f).value;"),
        "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
}

// 3.1.6 — second-order: Grad(Grad(f)) differentiates the gradient. For f = x*x,
// f' = 2x and f'' = 2, so the result carries {value: f'(x)=2x, grads: f''=2}.
// At x=3: value 6, grads 2 -> 6*100 + 2 = 602.
TEST(GradEndToEnd, secondOrderSquare) {
    EXPECT_FLOAT_EQ(runF32(
        "(float32) -> GradResult<float32,float32> g2 =\n"
        "            Grad(Grad((float32 x) -> x * x));\n"
        "        GradResult<float32,float32> r = g2(3.0f);\n"
        "        return r.value * 100.0f + r.grads;"), 602.0f);
}

// 3.1.6 — second-order with a non-constant f'': for f = x*x*x, f' = 3x^2 and
// f'' = 6x, so {value: f'(x)=3x^2, grads: f''(x)=6x}. At x=2: value 12, grads 12
// -> 12*100 + 12 = 1212. Confirms the re-parsed gradient is itself differentiated.
TEST(GradEndToEnd, secondOrderCube) {
    EXPECT_FLOAT_EQ(runF32(
        "(float32) -> GradResult<float32,float32> g2 =\n"
        "            Grad(Grad((float32 x) -> x * x * x));\n"
        "        GradResult<float32,float32> r = g2(2.0f);\n"
        "        return r.value * 100.0f + r.grads;"), 1212.0f);
}

// A misspelled/unsupported primitive fails with a named compile error, never a
// silent wrong gradient (spec §5.3). Division has no VJP rule in v1.
TEST(GradEndToEnd, unsupportedOpNamedError) {
    EXPECT_EQ(compileErrorId(
        "(float32) -> GradResult<float32,float32> g = Grad((float32 x) -> x / x);\n"
        "        return g(2.0f).value;"), "CAJETA_ERROR_TRANSFORM_UNSUPPORTED_BODY");
}
