//
// nucleo-autograd Unit 2 — the eager tape, scalar core (spec 5.1/5.2/5.3; F3
// resolved as an explicit owned handle over parallel primitive arrays). Ops
// record as they execute; backward replays the registry's cotangent algebra in
// reverse. The tape's territory is DYNAMIC control flow — the loop whose trip
// count arrives at runtime, which compiled Grad rejects as non-specializable.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {
float runF32(const std::string& body, const std::string& extra = "") {
    std::string src =
        "package test;\n"
        "import cajeta.nucleo.autograd.Tape;\n"
        "import cajeta.nucleo.autograd.Var;\n"
        "import cajeta.error.Exception;\n"
        "public final class T {\n"
        + extra +
        "    public static float32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    return jit->lookup<float (*)()>("run")();
}
} // namespace

// 2.1.1 — record x*x, backward, read the gradient and the forward value.
TEST(TapeTests, squareValueAndGrad) {
    EXPECT_FLOAT_EQ(runF32(
        "Tape t = heap Tape();\n"
        "        Var x = t.var(3.0f);\n"
        "        Var y = t.mul(x, x);\n"
        "        t.backward(y);\n"
        "        return t.valueOf(y) * 100.0f + t.grad(x);"), 906.0f);
}

// 2.1.2 — the full scalar op set composes; analytic check:
// f = exp(log(x) + x/2) - sqrt(x)*x  at x=4:
//   exp(ln 4 + 2) = 4e^2; sqrt(4)*4 = 8; f = 4e^2 - 8
//   f' = 4e^2*? ... checked piecewise instead: g = log(x*x) -> 2/x (chain+fanout)
TEST(TapeTests, opSetComposesWithFanOut) {
    // d/dx [log(x*x)] = 2/x at x=3 -> 2/3 (x used twice through one mul).
    EXPECT_NEAR(runF32(
        "Tape t = heap Tape();\n"
        "        Var x = t.var(3.0f);\n"
        "        Var y = t.log(t.mul(x, x));\n"
        "        t.backward(y);\n"
        "        return t.grad(x);"), 2.0f / 3.0f, 1e-5f);
}

// 2.1.2 — div/exp/sqrt/neg: d/dx [exp(x)/sqrt(x) - (-x)] at x=1:
//   d(e^x/sqrt x) = e^x*(1/sqrt x - 1/(2 x^1.5)) = e*(1 - 0.5) = e/2; +1 -> e/2+1.
TEST(TapeTests, divExpSqrtNegAnalytic) {
    EXPECT_NEAR(runF32(
        "Tape t = heap Tape();\n"
        "        Var x = t.var(1.0f);\n"
        "        Var y = t.sub(t.div(t.exp(x), t.sqrt(x)), t.neg(x));\n"
        "        t.backward(y);\n"
        "        return t.grad(x);"), 2.7182818f / 2.0f + 1.0f, 1e-4f);
}

// 2.1.3 — DATA-DEPENDENT control flow: y = x^n with n arriving at runtime.
// d/dx x^n = n*x^(n-1); at x=2, n=5 -> 5*16 = 80. This is the tape's territory:
// compiled Grad cannot specialize a runtime-bounded loop.
TEST(TapeTests, runtimeBoundedLoopDifferentiates) {
    std::string src =
        "package test;\n"
        "import cajeta.nucleo.autograd.Tape;\n"
        "import cajeta.nucleo.autograd.Var;\n"
        "public final class T {\n"
        "    public static float32 run(int32 n) {\n"
        "        Tape t = heap Tape();\n"
        "        Var x = t.var(2.0f);\n"
        "        Var y = x;\n"
        "        int32 i = 1;\n"
        "        while (i < n) {\n"
        "            y = t.mul(y, x);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        t.backward(y);\n"
        "        return t.grad(x);\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.T");
    auto fn = jit->lookup<float (*)(int32_t)>("run");
    // n arrives at RUNTIME — the compiled Grad path cannot specialize this.
    EXPECT_NEAR(fn(5), 80.0f, 1e-3f);
    EXPECT_NEAR(fn(3), 12.0f, 1e-3f);
}

// 2.1.4 — two tapes are independent; grads are per-backward, not accumulated
// across tapes.
TEST(TapeTests, tapesAreIndependent) {
    EXPECT_FLOAT_EQ(runF32(
        "Tape a = heap Tape();\n"
        "        Tape b = heap Tape();\n"
        "        Var xa = a.var(3.0f);\n"
        "        Var xb = b.var(10.0f);\n"
        "        Var ya = a.mul(xa, xa);\n"
        "        Var yb = b.mul(xb, xb);\n"
        "        a.backward(ya);\n"
        "        b.backward(yb);\n"
        "        return a.grad(xa) + b.grad(xb);"), 26.0f);
}

// 2.1.4 — backward twice on one tape is a NAMED error, never a silent
// re-accumulate.
TEST(TapeTests, doubleBackwardIsNamedError) {
    // The throw is caught IN-SOURCE (a cajeta exception does not cross the
    // JIT boundary as a std::exception); the sentinel proves it fired.
    EXPECT_FLOAT_EQ(runF32(
        "Tape t = heap Tape();\n"
        "        Var x = t.var(2.0f);\n"
        "        Var y = t.mul(x, x);\n"
        "        t.backward(y);\n"
        "        try {\n"
        "            t.backward(y);\n"
        "        } catch (Exception e) {\n"
        "            return 111.0f;\n"
        "        }\n"
        "        return -1.0f;"), 111.0f);
}
