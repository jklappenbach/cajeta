//
// transform-intrinsics Unit 4 — @NoGrad stop-gradient + differentiate-through-a-
// call (spec §9.1/§9.2, F5). The baseline: a call to a same-class static single-
// return helper is INLINED into the forward DAG, so gradients flow through it
// (`Grad` can differentiate through helper functions, not just inline lambdas). A
// `@NoGrad` helper is the disciplined stop-gradient — the call contributes its
// forward value but a ZERO cotangent, and the backward simply has no term for it
// (statically visible; no runtime version-counter / detach machinery).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {
// Runs `run()` from a full source that may declare helper statics on class G.
float runSrc(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.G");
    auto fn = jit->lookup<float (*)()>("run");
    return fn();
}

// A source with one helper `sq` (optionally @NoGrad) and a differentiated body.
std::string prog(const std::string& sqAnnotation, const std::string& body) {
    return
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class G {\n"
        "    " + sqAnnotation + "\n"
        "    public static float32 sq(float32 y) { return y * y; }\n"
        "    public static float32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
}
} // namespace

// 4.1.1 — a @NoGrad call is a constant w.r.t. gradient. f = x * sq(x); with sq
// @NoGrad, d/dx = sq(x) only (the x INSIDE sq contributes nothing). At x=3:
// value 3*9=27, grad sq(3)=9 -> 27*100 + 9 = 2709.
TEST(GradNoGrad, noGradCallIsConstant) {
    EXPECT_FLOAT_EQ(runSrc(prog("@NoGrad",
        "(float32) -> GradResult<float32,float32> g = Grad((float32 x) -> x * sq(x));\n"
        "        GradResult<float32,float32> r = g(3.0f);\n"
        "        return r.value * 100.0f + r.grads;")), 2709.0f);
}

// 4.1.2 — gradient does not flow IN: when x reaches the output only through a
// @NoGrad region, the grad is identically ZERO (statically — no term at all). f =
// sq(x), @NoGrad. value sq(3)=9, grad 0 -> 9*100 + 0 = 900.
TEST(GradNoGrad, noGradParamOnlyInStopIsZero) {
    EXPECT_FLOAT_EQ(runSrc(prog("@NoGrad",
        "(float32) -> GradResult<float32,float32> g = Grad((float32 x) -> sq(x));\n"
        "        GradResult<float32,float32> r = g(3.0f);\n"
        "        return r.value * 100.0f + r.grads;")), 900.0f);
}

// 4.1.3 — the annotation is the ONLY difference: with sq NOT @NoGrad, the call is
// inlined and gradient flows through. f = x * sq(x) = x^3, d/dx = 3x^2. At x=3:
// value 27, grad 27 -> 27*100 + 27 = 2727 (vs 2709 with @NoGrad).
TEST(GradNoGrad, removingNoGradRestoresFlow) {
    EXPECT_FLOAT_EQ(runSrc(prog("",
        "(float32) -> GradResult<float32,float32> g = Grad((float32 x) -> x * sq(x));\n"
        "        GradResult<float32,float32> r = g(3.0f);\n"
        "        return r.value * 100.0f + r.grads;")), 2727.0f);
}

// The inlining baseline on its own: f = sq(x) with a plain helper differentiates
// THROUGH sq (body y*y, y bound to x) -> d/dx = 2x. At x=3: value 9, grad 6 -> 906.
TEST(GradNoGrad, differentiateThroughHelper) {
    EXPECT_FLOAT_EQ(runSrc(prog("",
        "(float32) -> GradResult<float32,float32> g = Grad((float32 x) -> sq(x));\n"
        "        GradResult<float32,float32> r = g(3.0f);\n"
        "        return r.value * 100.0f + r.grads;")), 906.0f);
}
