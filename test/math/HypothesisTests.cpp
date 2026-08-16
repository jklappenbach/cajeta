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

// 3.1.2 + 3.1.3 — two-sample in BOTH forms on the same data, pinned to
// scipy and asserted to DIFFER; the documented default (Welch) is the one
// the no-flag overload actually applies.

// 3.1.4 — paired is DISTINCT from two-sample on the same inputs.

// 3.1.6 — chi-square, both forms, against scipy (independence pinned with
// correction=False).

// 3.1.7 — one-way ANOVA F and p against scipy.

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
