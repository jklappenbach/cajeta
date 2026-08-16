//
// DistributionTests — stdlib-completion plan Unit 2: discrete
// distributions on cajeta.math.stats (spec §3, §9.3, §9.4). Binomial PMF
// in log space via gammaLn, CDF via betainc; Bernoulli as the named n=1
// case; Poisson PMF/CDF; sampling through cajeta.math.random.Generator.
//
// Reference oracle = scipy 1.18.0 (stats), pinned as constants; regenerate
// with tools/fixtures/gen_distributions.py. The two §9.3 values are
// hand-computable (15/64 and C(10,7)·0.8^7·0.2^3) so the unit has checks
// that need no oracle. PMF tolerance 1e-12; CDF via betainc 1e-10 (its
// documented accuracy class, see StatsSpecialTests).
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
    "import cajeta.math.random.Generator;\n"
    "import cajeta.math.stats.Binomial;\n"
    "import cajeta.math.stats.Bernoulli;\n"
    "import cajeta.math.stats.Poisson;\n"
    "import cajeta.math.stats.StatsException;\n";

const char* HELPERS =
    "public final class D {\n"
    "    public static boolean close(float64 a, float64 b) {\n"
    "        float64 d = a - b; if (d < 0.0) { d = -d; } return d < 0.000000000001;\n"
    "    }\n"
    "    public static boolean closeB(float64 a, float64 b) {\n"
    "        float64 d = a - b; if (d < 0.0) { d = -d; } return d < 0.0000000001;\n"
    "    }\n";

} // namespace

// 2.1.1 — the two hand-computable PMF values (§9.3): P(X=2|6,0.5) = 15/64,
// P(X=7|10,0.8) = 120·0.8^7·0.2^3. No oracle needed to check these.
TEST(DistributionTests, binomialHandComputedPmf) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        if (!D.close(Binomial.pmf(2, 6, 0.5), 0.234375)) { return -1; }\n"
        "        if (!D.close(Binomial.pmf(7, 10, 0.8), 0.201326592)) { return -2; }\n"
        // outside the support
        "        if (Binomial.pmf(-1, 6, 0.5) != 0.0) { return -3; }\n"
        "        if (Binomial.pmf(7, 6, 0.5) != 0.0) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 2.1.2 — large n where a naive factorial overflows (1000! ~ 4e2567):
// log-space evaluation must return the finite scipy values (§9.4).
// Tolerance is the ACCURACY CLASS, not the suite default: gammaLn carries
// ~1e-14 relative error, which on ln(1000!) ~ 5900 is ~1e-10 absolute in
// the log and hence ~1e-10 RELATIVE on the exponentiated pmf.
TEST(DistributionTests, binomialLargeNFiniteAndCorrect) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        float64 a = Binomial.pmf(300, 1000, 0.3);\n"
        "        if (a != a) { return -1; }\n"
        "        if (!D.closeB(a, 0.027521003821268382)) { return -2; }\n"
        "        float64 b = Binomial.pmf(950, 1000, 0.95);\n"
        "        if (!D.closeB(b, 0.0577879837141071)) { return -3; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 2.1.3 — binomial CDF against scipy across the parameter grid, via the
// regularized incomplete beta (§3.2).
TEST(DistributionTests, binomialCdfMatchesScipyGrid) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        if (!D.closeB(Binomial.cdf(2, 6, 0.5), 0.34375)) { return -1; }\n"
        "        if (!D.closeB(Binomial.cdf(7, 10, 0.8), 0.32220047359999987)) { return -2; }\n"
        "        if (!D.closeB(Binomial.cdf(0, 6, 0.1), 0.531441)) { return -3; }\n"
        "        if (!D.closeB(Binomial.cdf(5, 6, 0.9), 0.46855899999999995)) { return -4; }\n"
        "        if (!D.closeB(Binomial.cdf(25, 50, 0.5), 0.5561375863296085)) { return -5; }\n"
        "        if (!D.closeB(Binomial.cdf(300, 1000, 0.3), 0.5155935198141203)) { return -6; }\n"
        "        if (!D.closeB(Binomial.cdf(1, 10, 0.1), 0.7360989291)) { return -7; }\n"
        // edges: full support -> 1, below support -> 0
        "        if (Binomial.cdf(6, 6, 0.5) != 1.0) { return -8; }\n"
        "        if (Binomial.cdf(-1, 6, 0.5) != 0.0) { return -9; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 2.1.4 — n = 1 gives the exact Bernoulli values, and Bernoulli is the
// named case (§3.3): Bernoulli.pmf(k, p) == Binomial.pmf(k, 1, p).
TEST(DistributionTests, bernoulliExactAndNamed) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        if (!D.close(Bernoulli.pmf(1, 0.3), 0.3)) { return -1; }\n"
        "        if (!D.close(Bernoulli.pmf(0, 0.3), 0.7000000000000002)) { return -2; }\n"
        "        if (!D.closeB(Bernoulli.cdf(0, 0.3), 0.7)) { return -3; }\n"
        "        if (Bernoulli.cdf(1, 0.3) != 1.0) { return -4; }\n"
        "        if (!D.close(Bernoulli.pmf(1, 0.3), Binomial.pmf(1, 1, 0.3))) { return -5; }\n"
        "        if (!D.close(Bernoulli.pmf(0, 0.3), Binomial.pmf(0, 1, 0.3))) { return -6; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 2.1.5 — Poisson PMF and CDF against scipy.
TEST(DistributionTests, poissonMatchesScipy) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        if (!D.close(Poisson.pmf(0, 0.5), 0.6065306597126334)) { return -1; }\n"
        "        if (!D.close(Poisson.pmf(2, 0.5), 0.07581633246407919)) { return -2; }\n"
        "        if (!D.close(Poisson.pmf(2, 3.0), 0.22404180765538775)) { return -3; }\n"
        "        if (!D.close(Poisson.pmf(5, 3.0), 0.10081881344492458)) { return -4; }\n"
        "        if (!D.close(Poisson.pmf(10, 10.0), 0.12511003572113372)) { return -5; }\n"
        "        if (!D.close(Poisson.pmf(20, 10.0), 0.0018660813139987742)) { return -6; }\n"
        "        if (!D.closeB(Poisson.cdf(0, 0.5), 0.6065306597126334)) { return -7; }\n"
        "        if (!D.closeB(Poisson.cdf(2, 0.5), 0.9856123220330293)) { return -8; }\n"
        "        if (!D.closeB(Poisson.cdf(2, 3.0), 0.42319008112684364)) { return -9; }\n"
        "        if (!D.closeB(Poisson.cdf(5, 3.0), 0.9160820579686966)) { return -10; }\n"
        "        if (!D.closeB(Poisson.cdf(10, 10.0), 0.5830397501929852)) { return -11; }\n"
        "        if (!D.closeB(Poisson.cdf(20, 10.0), 0.998411739338142)) { return -12; }\n"
        "        if (Poisson.pmf(-1, 3.0) != 0.0) { return -13; }\n"
        "        if (Poisson.cdf(-1, 3.0) != 0.0) { return -14; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 2.1.6 — sampling with a fixed seed is reproducible run to run (§3.5):
// two Generators with the same seed produce identical sample vectors;
// a different seed produces a different vector.
TEST(DistributionTests, samplingSeededReproducible) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static boolean sameI(Tensor<int64> a, Tensor<int64> b) {\n"
        "        int64 n = a.size();\n"
        "        if (b.size() != n) { return false; }\n"
        "        int64 i = 0;\n"
        "        while (i < n) {\n"
        "            if (a.get1(i) != b.get1(i)) { return false; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return true;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Generator g1 = heap Generator(1234);\n"
        "        Generator g2 = heap Generator(1234);\n"
        "        Generator g3 = heap Generator(9999);\n"
        "        Tensor<int64> a #= Binomial.sample(6, 0.5, 200, g1);\n"
        "        Tensor<int64> b #= Binomial.sample(6, 0.5, 200, g2);\n"
        "        Tensor<int64> c #= Binomial.sample(6, 0.5, 200, g3);\n"
        "        if (!D.sameI(a, b)) { return -1; }\n"
        "        if (D.sameI(a, c)) { return -2; }\n"
        "        Generator h1 = heap Generator(42);\n"
        "        Generator h2 = heap Generator(42);\n"
        "        Tensor<int64> pa #= Poisson.sample(3.0, 200, h1);\n"
        "        Tensor<int64> pb #= Poisson.sample(3.0, 200, h2);\n"
        "        if (!D.sameI(pa, pb)) { return -3; }\n"
        "        Generator k1 = heap Generator(7);\n"
        "        Generator k2 = heap Generator(7);\n"
        "        Tensor<int64> ba #= Bernoulli.sample(0.3, 200, k1);\n"
        "        Tensor<int64> bb #= Bernoulli.sample(0.3, 200, k2);\n"
        "        if (!D.sameI(ba, bb)) { return -4; }\n"
        // samples land in the support
        "        int64 i = 0;\n"
        "        while (i < 200) {\n"
        "            if (a.get1(i) < 0 || a.get1(i) > 6) { return -5; }\n"
        "            if (ba.get1(i) < 0 || ba.get1(i) > 1) { return -6; }\n"
        "            if (pa.get1(i) < 0) { return -7; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 2.1.7 — sampled empirical frequencies converge to the PMF: 100000
// samples, |freq(k) - pmf(k)| < 0.01 at the modal values.
TEST(DistributionTests, samplingConvergesToPmf) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static float64 freqOf(Tensor<int64> s, int64 k) {\n"
        "        int64 n = s.size();\n"
        "        int64 c = 0;\n"
        "        int64 i = 0;\n"
        "        while (i < n) {\n"
        "            if (s.get1(i) == k) { c = c + 1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return ((float64) c) / ((float64) n);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Generator g = heap Generator(20260804);\n"
        "        Tensor<int64> bs #= Binomial.sample(6, 0.5, 100000, g);\n"
        "        float64 d2 = D.freqOf(bs, 2) - 0.234375;\n"
        "        if (d2 < 0.0) { d2 = -d2; }\n"
        "        if (d2 > 0.01) { return -1; }\n"
        "        float64 d3 = D.freqOf(bs, 3) - 0.3125;\n"
        "        if (d3 < 0.0) { d3 = -d3; }\n"
        "        if (d3 > 0.01) { return -2; }\n"
        "        Tensor<int64> ps #= Poisson.sample(3.0, 100000, g);\n"
        "        float64 p2 = D.freqOf(ps, 2) - 0.22404180765538775;\n"
        "        if (p2 < 0.0) { p2 = -p2; }\n"
        "        if (p2 > 0.01) { return -3; }\n"
        "        Tensor<int64> es #= Bernoulli.sample(0.3, 100000, g);\n"
        "        float64 e1 = D.freqOf(es, 1) - 0.3;\n"
        "        if (e1 < 0.0) { e1 = -e1; }\n"
        "        if (e1 > 0.01) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 2.1.8 — invalid parameters are REJECTED, never NaN (§3.6): p outside
// [0,1], negative n, negative lambda, each on pmf, cdf, and sample.
TEST(DistributionTests, invalidParametersRejected) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        Generator g = heap Generator(1);\n"
        "        int32 caught = 0;\n"
        "        try { float64 x = Binomial.pmf(2, 6, -0.1); if (x > 2.0) { return 99; } }\n"
        "        catch (StatsException ex) { caught = caught + 1; }\n"
        "        try { float64 x = Binomial.pmf(2, 6, 1.5); if (x > 2.0) { return 99; } }\n"
        "        catch (StatsException ex) { caught = caught + 1; }\n"
        "        try { float64 x = Binomial.cdf(2, -6, 0.5); if (x > 2.0) { return 99; } }\n"
        "        catch (StatsException ex) { caught = caught + 1; }\n"
        "        try { Tensor<int64> t = Binomial.sample(-6, 0.5, 10, g); if (t.size() > 99) { return 99; } }\n"
        "        catch (StatsException ex) { caught = caught + 1; }\n"
        "        try { float64 x = Bernoulli.pmf(1, -0.5); if (x > 2.0) { return 99; } }\n"
        "        catch (StatsException ex) { caught = caught + 1; }\n"
        "        try { float64 x = Poisson.pmf(2, -3.0); if (x > 2.0) { return 99; } }\n"
        "        catch (StatsException ex) { caught = caught + 1; }\n"
        "        try { float64 x = Poisson.cdf(2, -3.0); if (x > 2.0) { return 99; } }\n"
        "        catch (StatsException ex) { caught = caught + 1; }\n"
        "        try { Tensor<int64> t = Poisson.sample(-3.0, 10, g); if (t.size() > 99) { return 99; } }\n"
        "        catch (StatsException ex) { caught = caught + 1; }\n"
        "        if (caught != 8) { return -caught; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
