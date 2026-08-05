//
// HypothesisTests — stdlib-completion plan Unit 3: hypothesis testing on
// cajeta.math.stats (spec §4, §9.5). One-sample / two-sample (equal-var
// AND Welch) / paired t-tests, explicit Alternative, chi-square
// goodness-of-fit + independence, one-way ANOVA, assumption warnings.
//
// Reference oracle = scipy 1.18.0 (stats), pinned as constants; regenerate
// with tools/fixtures/gen_hypothesis.py. chi-square independence is pinned
// with correction=False (no Yates correction; documented). Statistics at
// 1e-12; p-values at 1e-10 (betainc / incomplete-gamma accuracy class).
//
// The DEFAULT two-sample variance assumption is WELCH (R's default; scipy
// defaults to equal-var — the difference is exactly why §4.2 requires the
// default to be stated). 3.1.3 pins that the no-flag overload equals the
// explicit Welch call.
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
    "import cajeta.math.stats.Hypothesis;\n"
    "import cajeta.math.stats.TestResult;\n"
    "import cajeta.math.stats.Alternative;\n";

// x (n=8), y (n=6), d2 (n=8) — the fixture vectors from the generator.
const char* HELPERS =
    "public final class D {\n"
    "    public static boolean close(float64 a, float64 b) {\n"
    "        float64 d = a - b; if (d < 0.0) { d = -d; } return d < 0.000000000001;\n"
    "    }\n"
    "    public static boolean closeB(float64 a, float64 b) {\n"
    "        float64 d = a - b; if (d < 0.0) { d = -d; } return d < 0.0000000001;\n"
    "    }\n"
    "    public static #Tensor<float64> vec(float64[] d, int64 n) {\n"
    "        int64[] sh = heap int64[1]; sh[0] = n;\n"
    "        return Tensor.of<float64>(d, sh);\n"
    "    }\n"
    "    public static #Tensor<float64> x() {\n"
    "        float64[] d = heap float64[8];\n"
    "        d[0]=2.1; d[1]=2.5; d[2]=1.9; d[3]=2.8; d[4]=2.2; d[5]=2.6; d[6]=2.4; d[7]=2.0;\n"
    "        return D.vec(d, 8);\n"
    "    }\n"
    "    public static #Tensor<float64> y() {\n"
    "        float64[] d = heap float64[6];\n"
    "        d[0]=1.6; d[1]=1.8; d[2]=2.3; d[3]=1.5; d[4]=1.9; d[5]=2.1;\n"
    "        return D.vec(d, 6);\n"
    "    }\n"
    "    public static #Tensor<float64> d2() {\n"
    "        float64[] d = heap float64[8];\n"
    "        d[0]=2.3; d[1]=2.1; d[2]=2.2; d[3]=2.0; d[4]=2.5; d[5]=2.4; d[6]=1.8; d[7]=2.6;\n"
    "        return D.vec(d, 8);\n"
    "    }\n";

} // namespace

// 3.1.1 + 3.1.5 + 3.1.9 — one-sample t-test against scipy: statistic, df,
// p-value, all three alternatives; the result carries the alternative
// tested and is reportable without recomputation.
TEST(HypothesisTests, oneSampleTTestAllAlternatives) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        Tensor<float64> x = D.x();\n"
        "        TestResult r2 = Hypothesis.tTest1(x, 2.0, Alternative.TWO_SIDED);\n"
        "        if (!D.close(r2.statistic, 2.817819882980806)) { return -1; }\n"
        "        if (!D.closeB(r2.pValue, 0.025854033918168668)) { return -2; }\n"
        "        if (r2.df != 7.0) { return -3; }\n"
        "        if (r2.alternative != Alternative.TWO_SIDED) { return -4; }\n"
        "        TestResult rl = Hypothesis.tTest1(x, 2.0, Alternative.LESS);\n"
        "        if (!D.closeB(rl.pValue, 0.9870729830409156)) { return -5; }\n"
        "        if (rl.alternative != Alternative.LESS) { return -6; }\n"
        "        TestResult rg = Hypothesis.tTest1(x, 2.0, Alternative.GREATER);\n"
        "        if (!D.closeB(rg.pValue, 0.012927016959084334)) { return -7; }\n"
        // one/two-tailed relationship on this data (t > 0):
        // p_greater = p_two/2, p_less = 1 - p_greater
        "        if (!D.closeB(rg.pValue * 2.0, r2.pValue)) { return -8; }\n"
        "        if (!D.closeB(rl.pValue + rg.pValue, 1.0)) { return -9; }\n"
        "        if (r2.warnings != 0) { return -10; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 3.1.2 + 3.1.3 — two-sample in BOTH forms on the same data, pinned to
// scipy and asserted to DIFFER; the documented default (Welch) is the one
// the no-flag overload actually applies.
TEST(HypothesisTests, twoSampleBothVarianceForms) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        Tensor<float64> x = D.x();\n"
        "        Tensor<float64> y = D.y();\n"
        "        TestResult eq = Hypothesis.tTest2(x, y, true, Alternative.TWO_SIDED);\n"
        "        if (!D.close(eq.statistic, 2.675906059092438)) { return -1; }\n"
        "        if (!D.closeB(eq.pValue, 0.020189154546710813)) { return -2; }\n"
        "        if (eq.df != 12.0) { return -3; }\n"
        "        TestResult w = Hypothesis.tTest2(x, y, false, Alternative.TWO_SIDED);\n"
        "        if (!D.close(w.statistic, 2.6928755872337247)) { return -4; }\n"
        "        if (!D.closeB(w.pValue, 0.020675442041109077)) { return -5; }\n"
        "        if (!D.close(w.df, 11.167253980973832)) { return -6; }\n"
        // the two forms DIFFER on the same data (§9.5)
        "        if (D.close(eq.statistic, w.statistic)) { return -7; }\n"
        "        if (D.close(eq.df, w.df)) { return -8; }\n"
        // the default IS Welch
        "        TestResult def = Hypothesis.tTest2(x, y, Alternative.TWO_SIDED);\n"
        "        if (!D.close(def.statistic, w.statistic)) { return -9; }\n"
        "        if (!D.close(def.df, w.df)) { return -10; }\n"
        // one-sided welch
        "        TestResult wg = Hypothesis.tTest2(x, y, false, Alternative.GREATER);\n"
        "        if (!D.closeB(wg.pValue, 0.010337721020554538)) { return -11; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 3.1.4 — paired is DISTINCT from two-sample on the same inputs.
TEST(HypothesisTests, pairedDistinctFromTwoSample) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        Tensor<float64> x = D.x();\n"
        "        Tensor<float64> d2 = D.d2();\n"
        "        TestResult p = Hypothesis.tTestPaired(x, d2, Alternative.TWO_SIDED);\n"
        "        if (!D.close(p.statistic, 0.42609411632339844)) { return -1; }\n"
        "        if (!D.closeB(p.pValue, 0.6828350875988098)) { return -2; }\n"
        "        if (p.df != 7.0) { return -3; }\n"
        "        TestResult u = Hypothesis.tTest2(x, d2, Alternative.TWO_SIDED);\n"
        "        if (D.close(p.statistic, u.statistic)) { return -4; }\n"
        "        if (D.close(p.pValue, u.pValue)) { return -5; }\n"
        "        if (!D.close(u.statistic, 0.5150370451673695)) { return -6; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 3.1.6 — chi-square, both forms, against scipy (independence pinned with
// correction=False).
TEST(HypothesisTests, chiSquareBothForms) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        float64[] od = heap float64[5];\n"
        "        od[0]=18.0; od[1]=22.0; od[2]=27.0; od[3]=13.0; od[4]=20.0;\n"
        "        float64[] ed = heap float64[5];\n"
        "        ed[0]=20.0; ed[1]=20.0; ed[2]=20.0; ed[3]=20.0; ed[4]=20.0;\n"
        "        TestResult g = Hypothesis.chiSquareGof(D.vec(od, 5), D.vec(ed, 5));\n"
        "        if (!D.close(g.statistic, 5.300000000000001)) { return -1; }\n"
        "        if (!D.closeB(g.pValue, 0.25787692767056786)) { return -2; }\n"
        "        if (g.df != 4.0) { return -3; }\n"
        "        float64[] td = heap float64[6];\n"
        "        td[0]=12.0; td[1]=5.0; td[2]=9.0;\n"
        "        td[3]=8.0; td[4]=14.0; td[5]=6.0;\n"
        "        int64[] tsh = heap int64[2]; tsh[0] = 2; tsh[1] = 3;\n"
        "        Tensor<float64> table = Tensor.of<float64>(td, tsh);\n"
        "        TestResult i = Hypothesis.chiSquareIndependence(table);\n"
        "        if (!D.close(i.statistic, 5.59676113360324)) { return -4; }\n"
        "        if (!D.closeB(i.pValue, 0.06090862024162731)) { return -5; }\n"
        "        if (i.df != 2.0) { return -6; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 3.1.7 — one-way ANOVA F and p against scipy.
TEST(HypothesisTests, oneWayAnova) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        float64[] a = heap float64[5];\n"
        "        a[0]=4.1; a[1]=3.9; a[2]=4.3; a[3]=4.0; a[4]=4.2;\n"
        "        float64[] b = heap float64[5];\n"
        "        b[0]=3.5; b[1]=3.8; b[2]=3.6; b[3]=3.9; b[4]=3.4;\n"
        "        float64[] c = heap float64[5];\n"
        "        c[0]=4.4; c[1]=4.6; c[2]=4.2; c[3]=4.7; c[4]=4.5;\n"
        "        Tensor<float64>[] groups = heap Tensor<float64>[3];\n"
        "        groups[0] = D.vec(a, 5);\n"
        "        groups[1] = D.vec(b, 5);\n"
        "        groups[2] = D.vec(c, 5);\n"
        "        TestResult r = Hypothesis.anova1(groups);\n"
        "        if (!D.close(r.statistic, 25.276190476190518)) { return -1; }\n"
        "        if (!D.closeB(r.pValue, 0.0000498453220181408)) { return -2; }\n"
        "        if (r.df != 2.0) { return -3; }\n"
        "        if (r.dfWithin != 12.0) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 3.1.8 — assumption violations WARN, never silently report (§4.7): tiny
// samples, zero variance, expected cell counts below 5. A clean test has
// warnings == 0.
TEST(HypothesisTests, assumptionViolationsWarn) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        // n = 3 < 5 -> WARN_SMALL_N
        "        float64[] sd = heap float64[3];\n"
        "        sd[0]=1.0; sd[1]=2.0; sd[2]=3.0;\n"
        "        TestResult s = Hypothesis.tTest1(D.vec(sd, 3), 1.5, Alternative.TWO_SIDED);\n"
        "        if ((s.warnings & Hypothesis.WARN_SMALL_N) == 0) { return -1; }\n"
        // constant vector -> WARN_ZERO_VARIANCE
        "        float64[] zd = heap float64[8];\n"
        "        int64 zi = 0;\n"
        "        while (zi < 8) { zd[zi] = 5.0; zi = zi + 1; }\n"
        "        TestResult z = Hypothesis.tTest1(D.vec(zd, 8), 4.0, Alternative.TWO_SIDED);\n"
        "        if ((z.warnings & Hypothesis.WARN_ZERO_VARIANCE) == 0) { return -2; }\n"
        // expected cell below 5 -> WARN_LOW_EXPECTED
        "        float64[] od = heap float64[3];\n"
        "        od[0]=3.0; od[1]=4.0; od[2]=5.0;\n"
        "        float64[] ed = heap float64[3];\n"
        "        ed[0]=4.0; ed[1]=4.0; ed[2]=4.0;\n"
        "        TestResult g = Hypothesis.chiSquareGof(D.vec(od, 3), D.vec(ed, 3));\n"
        "        if ((g.warnings & Hypothesis.WARN_LOW_EXPECTED) == 0) { return -3; }\n"
        // clean inputs -> no warnings
        "        TestResult ok = Hypothesis.tTest1(D.x(), 2.0, Alternative.TWO_SIDED);\n"
        "        if (ok.warnings != 0) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
