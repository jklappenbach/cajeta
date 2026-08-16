//
// OptimTests — stdlib-completion plan Unit 5: cajeta.math.optim (spec
// §8.6). L-BFGS (strong-Wolfe line search, numerical differentiation when
// no gradient is supplied) and Nelder-Mead, with a result that reports WHY
// it stopped — and never reports success on an iteration cap.
//
// Reference oracle = scipy 1.18.0 minimize (L-BFGS-B / Nelder-Mead) on a
// shared fixture set; parity is to the shared MINIMIZER, not the
// trajectory (line-search internals legitimately differ). Fixtures:
// tools/fixtures/gen_optim.py. Analytic-vs-numerical gradient dispatch is
// by STATIC OVERLOAD (Objective vs GradObjective) — instanceof on an
// interface-typed value is a live compiler defect
// (specs/instanceof-interface-lhs).
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* PRE =
    "package test;\n"
    "import cajeta.math.Tensor;\n"
    "import cajeta.math.optim.Objective;\n"
    "import cajeta.math.optim.GradObjective;\n"
    "import cajeta.math.optim.Lbfgs;\n"
    "import cajeta.math.optim.NelderMead;\n"
    "import cajeta.math.optim.OptimResult;\n"
    "import cajeta.math.optim.TerminationReason;\n";

// Rosenbrock f = (1-x)^2 + 100(y-x^2)^2, its analytic gradient, the
// quadratic (x-3)^2 + 2(y+1)^2, and an always-NaN objective.
const char* FNS =
    "public final class Quad implements Objective {\n"
    "    public float64 value(Tensor<float64> x) {\n"
    "        float64 a = x.get1(0) - 3.0;\n"
    "        float64 b = x.get1(1) + 1.0;\n"
    "        return a * a + 2.0 * b * b;\n"
    "    }\n"
    "}\n"
    "public final class Rosen implements Objective {\n"
    "    public float64 value(Tensor<float64> x) {\n"
    "        float64 a = 1.0 - x.get1(0);\n"
    "        float64 b = x.get1(1) - x.get1(0) * x.get1(0);\n"
    "        return a * a + 100.0 * b * b;\n"
    "    }\n"
    "}\n"
    "public final class RosenG implements GradObjective {\n"
    "    public float64 value(Tensor<float64> x) {\n"
    "        float64 a = 1.0 - x.get1(0);\n"
    "        float64 b = x.get1(1) - x.get1(0) * x.get1(0);\n"
    "        return a * a + 100.0 * b * b;\n"
    "    }\n"
    "    public #Tensor<float64> gradient(Tensor<float64> x) {\n"
    "        float64 x0 = x.get1(0);\n"
    "        float64 x1 = x.get1(1);\n"
    "        float64[] g = heap float64[2];\n"
    "        g[0] = -2.0 * (1.0 - x0) - 400.0 * x0 * (x1 - x0 * x0);\n"
    "        g[1] = 200.0 * (x1 - x0 * x0);\n"
    "        int64[] sh = heap int64[1]; sh[0] = 2;\n"
    "        return Tensor.of<float64>(g, sh);\n"
    "    }\n"
    "}\n"
    "public final class NanFn implements Objective {\n"
    "    public float64 value(Tensor<float64> x) {\n"
    "        return 0.0 / 0.0;\n"
    "    }\n"
    "}\n";

const char* HELPERS =
    "public final class D {\n"
    "    public static #Tensor<float64> v2(float64 a, float64 b) {\n"
    "        float64[] d = heap float64[2]; d[0] = a; d[1] = b;\n"
    "        int64[] sh = heap int64[1]; sh[0] = 2;\n"
    "        return Tensor.of<float64>(d, sh);\n"
    "    }\n"
    "    public static boolean near(float64 a, float64 b, float64 tol) {\n"
    "        float64 d = a - b; if (d < 0.0) { d = -d; } return d < tol;\n"
    "    }\n";

} // namespace

// 5.1.2 — both optimizers on the quadratic: failure here means the line
// search (or simplex update) is wrong.
TEST(OptimTests, quadraticFoundExactly) {
    std::string src = std::string(PRE) + FNS + HELPERS +
        "    public static int32 run() {\n"
        "        OptimResult lb #= Lbfgs.minimize(heap Quad(), D.v2(0.0, 0.0), 500);\n"
        "        if (!lb.converged) { return -1; }\n"
        "        if (!D.near(lb.x.get1(0), 3.0, 0.000001)) { return -2; }\n"
        "        if (!D.near(lb.x.get1(1), -1.0, 0.000001)) { return -3; }\n"
        "        if (lb.value > 0.0000000001) { return -4; }\n"
        "        OptimResult nm #= NelderMead.minimize(heap Quad(), D.v2(0.0, 0.0), 2000);\n"
        "        if (!nm.converged) { return -5; }\n"
        "        if (!D.near(nm.x.get1(0), 3.0, 0.000001)) { return -6; }\n"
        "        if (!D.near(nm.x.get1(1), -1.0, 0.000001)) { return -7; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 5.1.1 + 5.1.7 — Rosenbrock from the standard start (-1.2, 1): both
// methods reach (1, 1) within tolerance and budget, like scipy does
// (L-BFGS-B: 36 iters; NM: 117).
TEST(OptimTests, rosenbrockConverges) {
    std::string src = std::string(PRE) + FNS + HELPERS +
        "    public static int32 run() {\n"
        "        OptimResult lb #= Lbfgs.minimize(heap Rosen(), D.v2(-1.2, 1.0), 500);\n"
        "        if (!lb.converged) { return -1; }\n"
        "        if (!D.near(lb.x.get1(0), 1.0, 0.00001)) { return -2; }\n"
        "        if (!D.near(lb.x.get1(1), 1.0, 0.00001)) { return -3; }\n"
        "        if (lb.value > 0.000000001) { return -4; }\n"
        "        if (lb.iterations > 500) { return -5; }\n"
        "        OptimResult nm #= NelderMead.minimize(heap Rosen(), D.v2(-1.2, 1.0), 2000);\n"
        "        if (!nm.converged) { return -6; }\n"
        "        if (!D.near(nm.x.get1(0), 1.0, 0.000001)) { return -7; }\n"
        "        if (!D.near(nm.x.get1(1), 1.0, 0.000001)) { return -8; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 5.1.3 — L-BFGS with the ANALYTIC Rosenbrock gradient and with the
// NUMERICAL one land on the same minimizer.
TEST(OptimTests, analyticMatchesNumericalGradient) {
    std::string src = std::string(PRE) + FNS + HELPERS +
        "    public static int32 run() {\n"
        "        OptimResult an #= Lbfgs.minimize(heap RosenG(), D.v2(-1.2, 1.0), 500);\n"
        "        OptimResult nu #= Lbfgs.minimize(heap Rosen(), D.v2(-1.2, 1.0), 500);\n"
        "        if (!an.converged) { return -1; }\n"
        "        if (!nu.converged) { return -2; }\n"
        "        if (!D.near(an.x.get1(0), nu.x.get1(0), 0.000001)) { return -3; }\n"
        "        if (!D.near(an.x.get1(1), nu.x.get1(1), 0.000001)) { return -4; }\n"
        "        if (!D.near(an.x.get1(0), 1.0, 0.00001)) { return -5; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 5.1.4 + 5.3.2 — the termination reason is honest: a capped run reports
// MAX_ITERATIONS with converged == false, NEVER success; a successful run
// names the criterion that stopped it.
TEST(OptimTests, terminationReasonHonest) {
    std::string src = std::string(PRE) + FNS + HELPERS +
        "    public static int32 run() {\n"
        "        OptimResult capped #= Lbfgs.minimize(heap Rosen(), D.v2(-1.2, 1.0), 3);\n"
        "        if (capped.converged) { return -1; }\n"
        "        if (capped.reason != TerminationReason.MAX_ITERATIONS) { return -2; }\n"
        "        OptimResult nmCapped #= NelderMead.minimize(heap Rosen(), D.v2(-1.2, 1.0), 5);\n"
        "        if (nmCapped.converged) { return -3; }\n"
        "        if (nmCapped.reason != TerminationReason.MAX_ITERATIONS) { return -4; }\n"
        "        OptimResult ok #= Lbfgs.minimize(heap Quad(), D.v2(0.0, 0.0), 500);\n"
        "        if (ok.reason != TerminationReason.GRADIENT_NORM) { return -5; }\n"
        "        OptimResult nmOk #= NelderMead.minimize(heap Quad(), D.v2(0.0, 0.0), 2000);\n"
        "        if (nmOk.reason != TerminationReason.SIMPLEX_SIZE) { return -6; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 5.1.5 — a fixed start gives an IDENTICAL trajectory across runs: same
// minimizer bits, same iteration count. (Both methods are deterministic;
// nothing draws randomness.)
TEST(OptimTests, deterministicAcrossRuns) {
    std::string src = std::string(PRE) + FNS + HELPERS +
        "    public static int32 run() {\n"
        "        OptimResult a #= Lbfgs.minimize(heap Rosen(), D.v2(-1.2, 1.0), 500);\n"
        "        OptimResult b #= Lbfgs.minimize(heap Rosen(), D.v2(-1.2, 1.0), 500);\n"
        "        if (a.x.get1(0) != b.x.get1(0)) { return -1; }\n"
        "        if (a.x.get1(1) != b.x.get1(1)) { return -2; }\n"
        "        if (a.iterations != b.iterations) { return -3; }\n"
        "        if (a.value != b.value) { return -4; }\n"
        "        OptimResult c #= NelderMead.minimize(heap Rosen(), D.v2(-1.2, 1.0), 2000);\n"
        "        OptimResult d #= NelderMead.minimize(heap Rosen(), D.v2(-1.2, 1.0), 2000);\n"
        "        if (c.x.get1(0) != d.x.get1(0)) { return -5; }\n"
        "        if (c.iterations != d.iterations) { return -6; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 5.1.6 — a NaN objective is a DIAGNOSTIC (NONFINITE, converged false),
// not a silent wrong minimum and not a crash.
TEST(OptimTests, nonfiniteObjectiveDiagnosed) {
    std::string src = std::string(PRE) + FNS + HELPERS +
        "    public static int32 run() {\n"
        "        OptimResult lb #= Lbfgs.minimize(heap NanFn(), D.v2(0.5, 0.5), 100);\n"
        "        if (lb.converged) { return -1; }\n"
        "        if (lb.reason != TerminationReason.NONFINITE) { return -2; }\n"
        "        OptimResult nm #= NelderMead.minimize(heap NanFn(), D.v2(0.5, 0.5), 100);\n"
        "        if (nm.converged) { return -3; }\n"
        "        if (nm.reason != TerminationReason.NONFINITE) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 5.3.3 — the ml-timeseries §8 shape is reachable: an AR(1) conditional
// sum of squares over a fixed series, minimized by Nelder-Mead, recovers
// the closed-form OLS phi (scipy fixture: 0.4701450843381199).
TEST(OptimTests, ar1CssSmokeFit) {
    std::string src = std::string(PRE) +
        "public final class Ar1Css implements Objective {\n"
        "    public Tensor<float64> series;\n"
        "    public Ar1Css(Tensor<float64> series) {\n"
        "        this.series #= series;\n"
        "    }\n"
        "    public float64 value(Tensor<float64> params) {\n"
        "        float64 phi = params.get1(0);\n"
        "        int64 n = this.series.shapeAt(0);\n"
        "        float64 s = 0.0;\n"
        "        int64 t = 1;\n"
        "        while (t < n) {\n"
        "            float64 e = this.series.get1(t) - phi * this.series.get1(t - 1);\n"
        "            s = s + e * e;\n"
        "            t = t + 1;\n"
        "        }\n"
        "        return s;\n"
        "    }\n"
        "}\n" + std::string(HELPERS) +
        "    public static #Tensor<float64> series() {\n"
        "        float64[] d = heap float64[30];\n"
        "        d[0]=0.0; d[1]=-0.519992; d[2]=0.06323; d[3]=0.508221; d[4]=-0.670585;\n"
        "        d[5]=-1.053441; d[6]=-0.568144; d[7]=-0.499008; d[8]=-0.307805; d[9]=-0.611205;\n"
        "        d[10]=0.072976; d[11]=0.432682; d[12]=0.292624; d[13]=0.739195; d[14]=0.677272;\n"
        "        d[15]=-0.023283; d[16]=0.170405; d[17]=-0.377198; d[18]=0.212906; d[19]=0.102781;\n"
        "        d[20]=-0.030763; d[21]=-0.358922; d[22]=0.395917; d[23]=0.160286; d[24]=-0.117993;\n"
        "        d[25]=-0.246862; d[26]=0.118037; d[27]=0.253544; d[28]=0.358493; d[29]=0.430506;\n"
        "        int64[] sh = heap int64[1]; sh[0] = 30;\n"
        "        return Tensor.of<float64>(d, sh);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        float64[] p0 = heap float64[1]; p0[0] = 0.0;\n"
        "        int64[] psh = heap int64[1]; psh[0] = 1;\n"
        "        Tensor<float64> start #= Tensor.of<float64>(p0, psh);\n"
        "        OptimResult r #= NelderMead.minimize(heap Ar1Css(D.series()), start, 1000);\n"
        "        if (!r.converged) { return -1; }\n"
        "        if (!D.near(r.x.get1(0), 0.4701450843381199, 0.000001)) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
