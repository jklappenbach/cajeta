//
// InformationTests — stdlib-completion plan Unit 4: information theory on
// cajeta.math.stats (spec §5, §9.6). KL divergence (= scipy rel_entr
// summed, natural log), entropy and cross-entropy with the LOG BASE A
// REQUIRED ARGUMENT (ml-trees-ensembles needs base 2, ML losses base e —
// a default would silently serve one of them wrong).
//
// Reference oracle = scipy 1.18.0, pinned as constants; regenerate with
// tools/fixtures/gen_information.py. The base-2 entropy hand anchor is
// ml-trees-ensembles §3.5's H(0.625, 0.375) = 0.9544. Degenerate cases
// are the §9.6 contract: q=0 where p>0 -> +infinity (NEVER NaN — the case
// that silently corrupts a t-SNE run); p=0 contributes exactly zero.
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
    "import cajeta.math.stats.Information;\n"
    "import cajeta.math.stats.StatsException;\n";

const char* HELPERS =
    "public final class D {\n"
    "    public static boolean close(float64 a, float64 b) {\n"
    "        float64 d = a - b; if (d < 0.0) { d = -d; } return d < 0.000000000001;\n"
    "    }\n"
    "    public static #Tensor<float64> v3(float64 a, float64 b, float64 c) {\n"
    "        float64[] d = heap float64[3]; d[0] = a; d[1] = b; d[2] = c;\n"
    "        int64[] sh = heap int64[1]; sh[0] = 3;\n"
    "        return Tensor.of<float64>(d, sh);\n"
    "    }\n"
    "    public static #Tensor<float64> v2(float64 a, float64 b) {\n"
    "        float64[] d = heap float64[2]; d[0] = a; d[1] = b;\n"
    "        int64[] sh = heap int64[1]; sh[0] = 2;\n"
    "        return Tensor.of<float64>(d, sh);\n"
    "    }\n";

} // namespace

// 4.1.1 + 4.1.4 — KL against scipy rel_entr summed, and the asymmetry the
// docs claim, asserted: KL(p||q) != KL(q||p).
TEST(InformationTests, klMatchesScipyAndIsAsymmetric) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        Tensor<float64> p = D.v3(0.5, 0.3, 0.2);\n"
        "        Tensor<float64> q = D.v3(0.2, 0.5, 0.3);\n"
        "        float64 pq = Information.klDivergence(p, q);\n"
        "        float64 qp = Information.klDivergence(q, p);\n"
        "        if (!D.close(pq, 0.22380465718564752)) { return -1; }\n"
        "        if (!D.close(qp, 0.19379419794061364)) { return -2; }\n"
        "        if (D.close(pq, qp)) { return -3; }\n"
        "        if (!D.close(Information.klDivergence(p, p), 0.0)) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 4.1.2 + 4.1.3 + acceptance 4.3.1 — the degenerate cases: q=0 with p>0 is
// +INFINITY (never NaN); p=0 contributes exactly zero (0·log 0 = 0).
TEST(InformationTests, klDegenerateCases) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        // q has a zero where p > 0 -> +inf, not NaN
        "        Tensor<float64> p = D.v3(0.5, 0.3, 0.2);\n"
        "        Tensor<float64> qz = D.v3(0.0, 0.5, 0.5);\n"
        "        float64 inf = Information.klDivergence(p, qz);\n"
        "        if (inf != inf) { return -1; }\n"              // NaN guard
        "        if (inf < 1.0e300) { return -2; }\n"           // infinite
        // p = 0 contributes zero: finite scipy value on the pz fixture
        "        Tensor<float64> pz = D.v3(0.0, 0.6, 0.4);\n"
        "        Tensor<float64> q2 = D.v3(0.5, 0.25, 0.25);\n"
        "        float64 f = Information.klDivergence(pz, q2);\n"
        "        if (!D.close(f, 0.713282694110634)) { return -3; }\n"
        // BOTH zero at the same index: the p=0 rule wins, term is 0
        "        Tensor<float64> pb = D.v3(0.0, 0.6, 0.4);\n"
        "        Tensor<float64> qb = D.v3(0.0, 0.5, 0.5);\n"
        "        float64 g = Information.klDivergence(pb, qb);\n"
        "        if (g != g) { return -4; }\n"
        "        if (g > 1.0e300) { return -5; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 4.1.5 + 4.1.6 + acceptance 4.3.2 — entropy: the base-2 hand value from
// ml-trees-ensembles §3.5 (H(5/8, 3/8) = 0.9544), an exact power-of-two
// case, and base e against scipy.
TEST(InformationTests, entropyBothBases) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        Tensor<float64> h = D.v2(0.625, 0.375);\n"
        "        if (!D.close(Information.entropy(h, 2.0), 0.954434002924965)) { return -1; }\n"
        "        if (!D.close(Information.entropy(h, 2.718281828459045), 0.6615632381579821)) { return -2; }\n"
        // uniform over 4 -> exactly 2 bits
        "        float64[] u4 = heap float64[4];\n"
        "        int64 i = 0;\n"
        "        while (i < 4) { u4[i] = 0.25; i = i + 1; }\n"
        "        int64[] sh = heap int64[1]; sh[0] = 4;\n"
        "        Tensor<float64> u = Tensor.of<float64>(u4, sh);\n"
        "        if (Information.entropy(u, 2.0) != 2.0) { return -3; }\n"
        "        Tensor<float64> p = D.v3(0.5, 0.3, 0.2);\n"
        "        if (!D.close(Information.entropy(p, 2.718281828459045), 1.0296530140645737)) { return -4; }\n"
        "        if (!D.close(Information.entropy(p, 2.0), 1.4854752972273346)) { return -5; }\n"
        // a zero probability contributes zero, not NaN
        "        Tensor<float64> pz = D.v3(0.0, 0.5, 0.5);\n"
        "        if (Information.entropy(pz, 2.0) != 1.0) { return -6; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 4.1.7 — cross-entropy in both bases, plus the two identities that pin
// its definition: CE(p, p) = H(p), and CE(p, q) = H(p) + KL(p||q) in nats.
TEST(InformationTests, crossEntropyBothBases) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        Tensor<float64> p = D.v3(0.5, 0.3, 0.2);\n"
        "        Tensor<float64> q = D.v3(0.2, 0.5, 0.3);\n"
        "        float64 e = 2.718281828459045;\n"
        "        if (!D.close(Information.crossEntropy(p, q, e), 1.2534576712502208)) { return -1; }\n"
        "        if (!D.close(Information.crossEntropy(p, q, 2.0), 1.8083571662769222)) { return -2; }\n"
        "        if (!D.close(Information.crossEntropy(p, p, e), Information.entropy(p, e))) { return -3; }\n"
        "        float64 lhs = Information.crossEntropy(p, q, e);\n"
        "        float64 rhs = Information.entropy(p, e) + Information.klDivergence(p, q);\n"
        "        if (!D.close(lhs, rhs)) { return -4; }\n"
        // q=0 where p>0 -> +infinity, never NaN
        "        Tensor<float64> qz = D.v3(0.0, 0.5, 0.5);\n"
        "        float64 inf = Information.crossEntropy(p, qz, e);\n"
        "        if (inf != inf) { return -5; }\n"
        "        if (inf < 1.0e300) { return -6; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// Validation: negative probabilities and mismatched lengths are rejected.
TEST(InformationTests, invalidInputRejected) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        int32 caught = 0;\n"
        "        Tensor<float64> p = D.v3(0.5, 0.3, 0.2);\n"
        "        try {\n"
        "            float64 x = Information.klDivergence(p, D.v3(-0.1, 0.6, 0.5));\n"
        "            if (x > 1.0e308) { return 99; }\n"
        "        } catch (StatsException ex) { caught = caught + 1; }\n"
        "        try {\n"
        "            float64 x = Information.entropy(D.v3(0.5, 0.5, -0.1), 2.0);\n"
        "            if (x > 1.0e308) { return 99; }\n"
        "        } catch (StatsException ex) { caught = caught + 1; }\n"
        "        try {\n"
        "            float64 x = Information.crossEntropy(p, D.v2(0.5, 0.5), 2.0);\n"
        "            if (x > 1.0e308) { return 99; }\n"
        "        } catch (StatsException ex) { caught = caught + 1; }\n"
        "        try {\n"
        "            float64 x = Information.entropy(p, 1.0);\n"
        "            if (x > 1.0e308) { return 99; }\n"
        "        } catch (StatsException ex) { caught = caught + 1; }\n"
        "        if (caught != 4) { return -caught; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
