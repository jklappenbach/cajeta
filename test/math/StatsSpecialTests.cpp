//
// StatsSpecialTests — cajeta-ml plan Unit 2: the special functions on
// cajeta.math.stats.Stats (erf/erfc, expit, betainc, gammaLn, normal + t
// CDFs). Reference oracle = scipy 1.17 (special/stats), pinned as constants.
// f64 absolute tolerance 1e-12 on O(1) values; far tails asserted RELATIVE
// (erfc(26) ~ 5.7e-296).
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
    "import cajeta.math.stats.Stats;\n";

const char* HELPERS =
    "public final class D {\n"
    "    public static boolean close(float64 a, float64 b) {\n"
    "        float64 d = a - b; if (d < 0.0) { d = -d; } return d < 0.000000000001;\n"
    "    }\n"
    "    public static boolean closeRel(float64 a, float64 b) {\n"
    "        float64 d = a - b; if (d < 0.0) { d = -d; }\n"
    "        float64 ab = b; if (ab < 0.0) { ab = -ab; }\n"
    "        return d <= ab * 0.0000000001;\n"
    "    }\n"
    // betainc's documented accuracy class is ~1e-10 absolute (the NR-grade
    // Lanczos loses ~5e-12 through exp(gammaLn(a+b)-...) at a=b=10) — far
    // beyond what p-values need; erf/erfc/normalCdf stay at 1e-12.
    "    public static boolean closeB(float64 a, float64 b) {\n"
    "        float64 d = a - b; if (d < 0.0) { d = -d; } return d < 0.0000000001;\n"
    "    }\n";

} // namespace

// 2.1.1 — erf/erfc vs scipy.special, including the far erfc tail where
// 1-erf would round to zero.
TEST(StatsSpecialTests, erfErfcMatchScipy) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        if (Stats.erf(0.0) != 0.0) { return -1; }\n"
        "        if (!D.close(Stats.erf(0.1), 0.1124629160182849)) { return -2; }\n"
        "        if (!D.close(Stats.erf(0.5), 0.5204998778130465)) { return -3; }\n"
        "        if (!D.close(Stats.erf(1.0), 0.8427007929497148)) { return -4; }\n"
        "        if (!D.close(Stats.erf(2.0), 0.9953222650189527)) { return -5; }\n"
        "        if (!D.close(Stats.erf(3.5), 0.9999992569016276)) { return -6; }\n"
        "        if (!D.close(Stats.erf(-1.5), -0.9661051464753108)) { return -7; }\n"
        "        if (!D.close(Stats.erfc(0.5), 0.4795001221869535)) { return -8; }\n"
        "        if (!D.close(Stats.erfc(2.0), 0.004677734981047266)) { return -9; }\n"
        "        if (!D.closeRel(Stats.erfc(5.0), 0.0000000000015374597944280347)) { return -10; }\n"
        // gammaLn sanity (the Lanczos base everything rides on)
        "        if (!D.close(Stats.gammaLn(0.5), 0.5723649429247)) { return -11; }\n"
        "        if (!D.close(Stats.gammaLn(1.0), 0.0)) { return -12; }\n"
        "        if (!D.close(Stats.gammaLn(10.0), 12.801827480081469)) { return -13; }\n"
        // erf/erfc consistency at a mid value
        "        float64 s = Stats.erf(1.7) + Stats.erfc(1.7);\n"
        "        if (!D.close(s, 1.0)) { return -14; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 2.1.1 (tail) — erfc at 10 and 26: relative agreement with scipy in the
// deep tail (values ~1e-45 and ~1e-296).
TEST(StatsSpecialTests, erfcDeepTailRelative) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        float64 t10 = 0.0000000000000000000000000000000000000000000020884875837625446;\n"
        "        if (!D.closeRel(Stats.erfc(10.0), t10)) { return -1; }\n"
        "        float64 e26 = Stats.erfc(26.0);\n"
        // 5.663192408856145e-296: build from parts (literal notation limits)
        "        float64 want = 5.663192408856145;\n"
        "        int64 k = 0;\n"
        "        while (k < 296) { want = want / 10.0; k = k + 1; }\n"
        "        float64 d = e26 - want; if (d < 0.0) { d = -d; }\n"
        // repeated /10 loses a few ulps itself — 1e-6 relative is ample proof
        // the value is right to ~6 digits at 1e-296 (cancellation-free path)
        "        if (d > want * 0.000001) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 2.1.2 — expit vs scipy, including saturation with NO overflow at |x|=750.
TEST(StatsSpecialTests, expitMatchesScipyAndSaturates) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        if (Stats.expit(0.0) != 0.5) { return -1; }\n"
        "        if (!D.close(Stats.expit(1.0), 0.7310585786300049)) { return -2; }\n"
        "        if (!D.close(Stats.expit(-3.0), 0.04742587317756678)) { return -3; }\n"
        "        if (Stats.expit(40.0) != 1.0) { return -4; }\n"
        "        if (!D.closeRel(Stats.expit(-40.0), 0.000000000000000004248354255291589)) { return -5; }\n"
        "        if (Stats.expit(750.0) != 1.0) { return -6; }\n"
        "        if (Stats.expit(-750.0) != 0.0) { return -7; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 2.1.3 / 2.1.4 — betainc vs scipy.special (incl. symmetric and skewed
// parameterizations) and the normal/t CDFs vs scipy.stats — the exact
// p-value backbone for cajeta-ml summary().
TEST(StatsSpecialTests, betaincAndCdfsMatchScipy) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        if (!D.closeB(Stats.betainc(2.0, 3.0, 0.4), 0.5247999999999999)) { return -1; }\n"
        "        if (!D.closeB(Stats.betainc(0.5, 0.5, 0.3), 0.36901011956554536)) { return -2; }\n"
        "        if (!D.closeB(Stats.betainc(5.0, 2.0, 0.8), 0.65536)) { return -3; }\n"
        "        if (!D.closeB(Stats.betainc(10.0, 10.0, 0.5), 0.5)) { return -4; }\n"
        "        if (!D.closeB(Stats.betainc(1.5, 0.7, 0.05), 0.0072046553384233995)) { return -5; }\n"
        "        if (Stats.betainc(2.0, 2.0, 0.0) != 0.0) { return -6; }\n"
        "        if (Stats.betainc(2.0, 2.0, 1.0) != 1.0) { return -7; }\n"
        // normal CDF
        "        if (Stats.normalCdf(0.0) != 0.5) { return -8; }\n"
        "        if (!D.close(Stats.normalCdf(1.0), 0.8413447460685429)) { return -9; }\n"
        "        if (!D.close(Stats.normalCdf(-1.959963984540054), 0.025)) { return -10; }\n"
        "        if (!D.close(Stats.normalCdf(2.5758293035489004), 0.995)) { return -11; }\n"
        // Student-t CDF
        "        if (Stats.tCdf(0.0, 5.0) != 0.5) { return -12; }\n"
        "        if (!D.closeB(Stats.tCdf(2.0, 10.0), 0.9633059826146299)) { return -13; }\n"
        "        if (!D.closeB(Stats.tCdf(-1.5, 3.0), 0.11529193262241147)) { return -14; }\n"
        "        if (!D.closeB(Stats.tCdf(2.086, 20.0), 0.9750018227712799)) { return -15; }\n"
        "        if (!D.closeB(Stats.tCdf(12.0, 2.0), 0.9965635331614208)) { return -16; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
