//
// nucleo-autograd Unit 3 — the agreement bar (spec 5.3, 10.2): the eager tape
// and the compiled Grad consume ONE rule set, so their gradients must agree on
// every function both can express. Each test computes BOTH gradients inside one
// program at a runtime-supplied point and returns their difference — the v1
// enforcement of "one rule-set, two drivers" (literal rule sharing is the
// recorded follow-on in the plan).
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Compile a program whose run(xv) returns eagerGrad(f, xv) - compiledGrad(f, xv).
// `lambda` is the compiled form; `tapeBody` records the same f on tape `t` over
// input `x`, leaving the output Var in `y`.
float diffAt(const std::string& lambda, const std::string& tapeBody, float at) {
    std::string src =
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "import cajeta.nucleo.autograd.Tape;\n"
        "import cajeta.nucleo.autograd.Var;\n"
        "public final class T {\n"
        "    public static float32 run(float32 xv) {\n"
        "        (float32) -> GradResult<float32,float32> g = Grad(" + lambda + ");\n"
        "        GradResult<float32,float32> r = g(xv);\n"
        "        Tape t = heap Tape();\n"
        "        Var x = t.var(xv);\n"
        "        " + tapeBody + "\n"
        "        t.backward(y);\n"
        "        return t.grad(x) - r.grads;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    return jit->lookup<float (*)(float)>("run")(at);
}

} // namespace

// 3.1.1 — polynomial: x*x*x - 2x + 1.

// 3.1.1 — rational: x / (x + 1).

// 3.1.1 — exp/log chain (softplus): log(exp(x) + 1).
TEST(Agreement, softplus) {
    const char* lam = "(float32 x) -> Math.log(Math.exp(x) + 1.0f)";
    const char* tape = "Var y = t.log(t.add(t.exp(x), t.var(1.0f)));";
    EXPECT_NEAR(diffAt(lam, tape, 0.0f), 0.0f, 1e-5f);
    EXPECT_NEAR(diffAt(lam, tape, 1.25f), 0.0f, 1e-5f);
}

// 3.1.1 — sqrt chain: sqrt(x*x + 1).
TEST(Agreement, sqrtChain) {
    const char* lam = "(float32 x) -> Math.sqrt(x * x + 1.0f)";
    const char* tape = "Var y = t.sqrt(t.add(t.mul(x, x), t.var(1.0f)));";
    EXPECT_NEAR(diffAt(lam, tape, 1.0f), 0.0f, 1e-5f);
    EXPECT_NEAR(diffAt(lam, tape, 3.0f), 0.0f, 1e-5f);
}

// 3.1.1 — fan-out: (x+1)*(x-1) + x*x (x feeds three ops).

// 3.1.1 — two args, gradient w.r.t. the SECOND (Grad<1> / the tape's second
// leaf): f(x,y) = x*y + y, df/dy = x + 1.

// 3.1.2 — each driver's exclusive territory, documented as tests: the tape
// differentiates a runtime-bounded loop (TapeTests.runtimeBoundedLoopDifferentiates);
// the compiled path batches via Vmap, which the tape has no axis for. Here: the
// compiled exclusive — Vmap(Grad(f)) per-example gradients (no tape analog).
