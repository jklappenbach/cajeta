//
// nucleo-autograd Unit 4 — stop-gradient on the tape (spec 8.1/8.2): the value
// flows, the cotangent does not — the eager analog of `@NoGrad`, agreement-
// tested against the compiled form on the same function.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <string>

using cajeta_test::CajetaJit;

namespace {
float runF32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.T");
    return jit->lookup<float (*)()>("run")();
}
} // namespace

// 4.1.1 — f = x * stop(x*x): the stopped factor contributes its VALUE (9) but
// no gradient path, so df/dx = x*x = 9 at x=3; value 27. Compiled @NoGrad on
// the same shape (U4 of transform-intrinsics) gives 27/9 too — asserted here
// side by side: same function, both drivers, same {value, grad}.
TEST(TapeStopGrad, mirrorsCompiledNoGrad) {
    EXPECT_FLOAT_EQ(runF32(
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "import cajeta.nucleo.autograd.Tape;\n"
        "import cajeta.nucleo.autograd.Var;\n"
        "public final class T {\n"
        "    @NoGrad\n"
        "    public static float32 sq(float32 y) { return y * y; }\n"
        "    public static float32 run() {\n"
        "        (float32) -> GradResult<float32,float32> g = Grad((float32 x) -> x * sq(x));\n"
        "        GradResult<float32,float32> r = g(3.0f);\n"
        "        Tape t = heap Tape();\n"
        "        Var x = t.var(3.0f);\n"
        "        Var y = t.mul(x, t.stopGrad(t.mul(x, x)));\n"
        "        t.backward(y);\n"
        "        float32 dv = t.valueOf(y) - r.value;\n"
        "        float32 dg = t.grad(x) - r.grads;\n"
        "        return dv * 1000.0f + dg;\n"
        "    }\n"
        "}\n"), 0.0f);
}

// 4.1.2 — a stop-only path is identically zero: f = stop(x*x), df/dx = 0
// (computed zero on the tape; statically zero on the compiled side).
TEST(TapeStopGrad, stopOnlyPathIsZero) {
    EXPECT_FLOAT_EQ(runF32(
        "package test;\n"
        "import cajeta.nucleo.autograd.Tape;\n"
        "import cajeta.nucleo.autograd.Var;\n"
        "public final class T {\n"
        "    public static float32 run() {\n"
        "        Tape t = heap Tape();\n"
        "        Var x = t.var(3.0f);\n"
        "        Var y = t.stopGrad(t.mul(x, x));\n"
        "        t.backward(y);\n"
        "        return t.valueOf(y) * 100.0f + t.grad(x);\n"
        "    }\n"
        "}\n"), 900.0f);
}
