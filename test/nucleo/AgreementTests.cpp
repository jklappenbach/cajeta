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
TEST(Agreement, polynomial) {
    const char* lam = "(float32 x) -> x * x * x - 2.0f * x + 1.0f";
    const char* tape =
        "Var y = t.add(t.sub(t.mul(t.mul(x, x), x),"
        " t.mul(t.var(2.0f), x)), t.var(1.0f));";
    EXPECT_NEAR(diffAt(lam, tape, 2.0f), 0.0f, 1e-4f);
    EXPECT_NEAR(diffAt(lam, tape, -1.5f), 0.0f, 1e-4f);
}

// 3.1.1 — rational: x / (x + 1).
TEST(Agreement, rational) {
    const char* lam = "(float32 x) -> x / (x + 1.0f)";
    const char* tape = "Var y = t.div(x, t.add(x, t.var(1.0f)));";
    EXPECT_NEAR(diffAt(lam, tape, 2.0f), 0.0f, 1e-5f);
    EXPECT_NEAR(diffAt(lam, tape, 0.5f), 0.0f, 1e-5f);
}

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
TEST(Agreement, fanOut) {
    const char* lam = "(float32 x) -> (x + 1.0f) * (x - 1.0f) + x * x";
    const char* tape =
        "Var y = t.add(t.mul(t.add(x, t.var(1.0f)),"
        " t.sub(x, t.var(1.0f))), t.mul(x, x));";
    EXPECT_NEAR(diffAt(lam, tape, 3.0f), 0.0f, 1e-4f);
}

// 3.1.1 — two args, gradient w.r.t. the SECOND (Grad<1> / the tape's second
// leaf): f(x,y) = x*y + y, df/dy = x + 1.
TEST(Agreement, twoArgArgnums) {
    std::string src =
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "import cajeta.nucleo.autograd.Tape;\n"
        "import cajeta.nucleo.autograd.Var;\n"
        "public final class T {\n"
        "    public static float32 run(float32 xv, float32 yv) {\n"
        "        (float32, float32) -> GradResult<float32,float32> g =\n"
        "            Grad<1>((float32 a, float32 b) -> a * b + b);\n"
        "        GradResult<float32,float32> r = g(xv, yv);\n"
        "        Tape t = heap Tape();\n"
        "        Var a = t.var(xv);\n"
        "        Var b = t.var(yv);\n"
        "        Var y = t.add(t.mul(a, b), b);\n"
        "        t.backward(y);\n"
        "        return t.grad(b) - r.grads;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    auto fn = jit->lookup<float (*)(float, float)>("run");
    EXPECT_NEAR(fn(3.0f, 5.0f), 0.0f, 1e-5f);
    EXPECT_NEAR(fn(-2.0f, 0.5f), 0.0f, 1e-5f);
}

// 3.1.2 — each driver's exclusive territory, documented as tests: the tape
// differentiates a runtime-bounded loop (TapeTests.runtimeBoundedLoopDifferentiates);
// the compiled path batches via Vmap, which the tape has no axis for. Here: the
// compiled exclusive — Vmap(Grad(f)) per-example gradients (no tape analog).
TEST(Agreement, compiledExclusiveVmapBatching) {
    std::string src =
        "package test;\n"
        "import cajeta.nucleo.transform.GradResult;\n"
        "public final class T {\n"
        "    public static float32 run() {\n"
        "        float32[] xs = [1.0f, 2.0f, 3.0f];\n"
        "        (float32[]) -> #GradResult<float32,float32>[] g =\n"
        "            Vmap(Grad((float32 x) -> x * x));\n"
        "        GradResult<float32,float32>[] rs = g(xs);\n"
        "        return rs[0].grads + rs[1].grads * 10.0f + rs[2].grads * 100.0f;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    EXPECT_FLOAT_EQ(jit->lookup<float (*)()>("run")(), 642.0f);
}
