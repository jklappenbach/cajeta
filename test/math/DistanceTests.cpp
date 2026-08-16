//
// DistanceTests — stdlib-completion plan Unit 1: cajeta.math.distance
// (spec §2, §9.1, §9.2). Euclidean, manhattan, chebyshev, minkowski(p),
// cosine, and Pearson behind one Metric interface; pdist/cdist; the
// expanded-form euclidean matmul path with its documented precision caveat.
//
// Reference oracle = scipy 1.18.0 (spatial.distance), pinned as constants;
// regenerate with tools/fixtures/gen_distance.py. Hand-computable cases are
// asserted first so the unit has checks that need no oracle. f64 absolute
// tolerance 1e-12 on O(1) values.
//
// Zero-norm cosine (§2.5) deliberately DIVERGES from scipy (which yields
// NaN): one zero-norm operand → similarity 0 / distance 1; both zero-norm →
// distance 0 (identity of indiscernibles preserved). Documented on Cosine.
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
    "import cajeta.math.distance.Distance;\n"
    "import cajeta.math.distance.Metric;\n"
    "import cajeta.math.distance.Euclidean;\n"
    "import cajeta.math.distance.Manhattan;\n"
    "import cajeta.math.distance.Chebyshev;\n"
    "import cajeta.math.distance.Minkowski;\n"
    "import cajeta.math.distance.Cosine;\n"
    "import cajeta.math.distance.Pearson;\n"
    "import cajeta.math.distance.DistanceException;\n";

// close(): 1e-12 absolute — the suite tolerance for O(1) values.
// v2/v3: vector builders. x43(): the fixed 4x3 fixture matrix from the
// generator. y23(): its first two rows + 0.5, the cdist counterpart.
const char* HELPERS =
    "public final class D {\n"
    "    public static boolean close(float64 a, float64 b) {\n"
    "        float64 d = a - b; if (d < 0.0) { d = -d; } return d < 0.000000000001;\n"
    "    }\n"
    "    public static #Tensor<float64> v2(float64 a, float64 b) {\n"
    "        float64[] d = heap float64[2]; d[0] = a; d[1] = b;\n"
    "        int64[] sh = heap int64[1]; sh[0] = 2;\n"
    "        return Tensor.of<float64>(d, sh);\n"
    "    }\n"
    "    public static #Tensor<float64> v3(float64 a, float64 b, float64 c) {\n"
    "        float64[] d = heap float64[3]; d[0] = a; d[1] = b; d[2] = c;\n"
    "        int64[] sh = heap int64[1]; sh[0] = 3;\n"
    "        return Tensor.of<float64>(d, sh);\n"
    "    }\n"
    "    public static #Tensor<float64> x43() {\n"
    "        float64[] d = heap float64[12];\n"
    "        d[0] = 0.2;  d[1] = -1.3; d[2] = 2.7;\n"
    "        d[3] = 1.9;  d[4] = 0.4;  d[5] = -0.6;\n"
    "        d[6] = -2.1; d[7] = 3.3;  d[8] = 1.1;\n"
    "        d[9] = 0.7;  d[10] = 0.9; d[11] = -1.8;\n"
    "        int64[] sh = heap int64[2]; sh[0] = 4; sh[1] = 3;\n"
    "        return Tensor.of<float64>(d, sh);\n"
    "    }\n"
    "    public static #Tensor<float64> y23() {\n"
    "        float64[] d = heap float64[6];\n"
    "        d[0] = 0.7;  d[1] = -0.8; d[2] = 3.2;\n"
    "        d[3] = 2.4;  d[4] = 0.9;  d[5] = -0.1;\n"
    "        int64[] sh = heap int64[2]; sh[0] = 2; sh[1] = 3;\n"
    "        return Tensor.of<float64>(d, sh);\n"
    "    }\n";

} // namespace

// 1.1.1 — every metric against hand-computed 2-D and 3-D cases, checkable
// on paper: (0,0)->(3,4) and (1,2,3)->(4,6,3) both have diffs (3,4[,0]).
TEST(DistanceTests, handComputedCases) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        Tensor<float64> a2 #= D.v2(0.0, 0.0);\n"
        "        Tensor<float64> b2 #= D.v2(3.0, 4.0);\n"
        "        Tensor<float64> a3 #= D.v3(1.0, 2.0, 3.0);\n"
        "        Tensor<float64> b3 #= D.v3(4.0, 6.0, 3.0);\n"
        "        if (!D.close(Distance.euclidean(a2, b2), 5.0)) { return -1; }\n"
        "        if (!D.close(Distance.manhattan(a2, b2), 7.0)) { return -2; }\n"
        "        if (!D.close(Distance.chebyshev(a2, b2), 4.0)) { return -3; }\n"
        // 91^(1/3): scipy 4.497941445275415
        "        if (!D.close(Distance.minkowski(a2, b2, 3.0), 4.497941445275415)) { return -4; }\n"
        "        if (!D.close(Distance.euclidean(a3, b3), 5.0)) { return -5; }\n"
        "        if (!D.close(Distance.manhattan(a3, b3), 7.0)) { return -6; }\n"
        "        if (!D.close(Distance.chebyshev(a3, b3), 4.0)) { return -7; }\n"
        "        if (!D.close(Distance.minkowski(a3, b3, 3.0), 4.497941445275415)) { return -8; }\n"
        // cosine: orthogonal -> distance 1; parallel -> 0; general vs scipy
        "        if (!D.close(Distance.cosine(D.v2(1.0, 0.0), D.v2(0.0, 1.0)), 1.0)) { return -9; }\n"
        "        if (!D.close(Distance.cosine(D.v3(1.0, 2.0, 2.0), D.v3(2.0, 4.0, 4.0)), 0.0)) { return -10; }\n"
        "        if (!D.close(Distance.cosine(a3, b3), 0.14451761146355635)) { return -11; }\n"
        // the Metric objects agree with the statics
        "        Metric me = heap Euclidean();\n"
        "        Metric mm = heap Manhattan();\n"
        "        Metric mc = heap Chebyshev();\n"
        "        Metric mk = heap Minkowski(3.0);\n"
        "        Metric mo = heap Cosine();\n"
        "        if (!D.close(me.distance(a3, b3), 5.0)) { return -12; }\n"
        "        if (!D.close(mm.distance(a3, b3), 7.0)) { return -13; }\n"
        "        if (!D.close(mc.distance(a3, b3), 4.0)) { return -14; }\n"
        "        if (!D.close(mk.distance(a3, b3), 4.497941445275415)) { return -15; }\n"
        "        if (!D.close(mo.distance(a3, b3), 0.14451761146355635)) { return -16; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 1.1.2 — minkowski(p=1) == manhattan and minkowski(p=2) == euclidean:
// the identity that catches an off-by-one in the exponent.
TEST(DistanceTests, minkowskiIdentities) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        Tensor<float64> a #= D.v3(0.3, -2.1, 1.4);\n"
        "        Tensor<float64> b #= D.v3(-1.6, 0.9, 2.2);\n"
        "        if (!D.close(Distance.minkowski(a, b, 1.0), Distance.manhattan(a, b))) { return -1; }\n"
        "        if (!D.close(Distance.minkowski(a, b, 2.0), Distance.euclidean(a, b))) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 1.1.3 — minkowski with p <= 0 is rejected (§2.2), on the static and on
// the Metric constructor alike.
TEST(DistanceTests, minkowskiRejectsNonPositiveP) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        Tensor<float64> a #= D.v2(1.0, 2.0);\n"
        "        Tensor<float64> b #= D.v2(3.0, 4.0);\n"
        "        int32 caught = 0;\n"
        "        try {\n"
        "            float64 x = Distance.minkowski(a, b, 0.0);\n"
        "            if (x > 0.0) { caught = 99; }\n"
        "        } catch (DistanceException ex) { caught = caught + 1; }\n"
        "        try {\n"
        "            float64 y = Distance.minkowski(a, b, -1.5);\n"
        "            if (y > 0.0) { caught = 99; }\n"
        "        } catch (DistanceException ex) { caught = caught + 1; }\n"
        "        try {\n"
        "            Metric m = heap Minkowski(0.0);\n"
        "            float64 z = m.distance(a, b);\n"
        "            if (z > 0.0) { caught = 99; }\n"
        "        } catch (DistanceException ex) { caught = caught + 1; }\n"
        "        if (caught != 3) { return -caught; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 1.1.4 — pdist over n points is (n, n), EXACTLY symmetric, with an EXACT
// zero diagonal — equality, not tolerance (§9.2). Asserted for both the
// generic path (manhattan) and the expanded euclidean path.
TEST(DistanceTests, pdistShapeSymmetryZeroDiagonalExact) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static boolean checkExact(Tensor<float64> m, int64 n) {\n"
        "        if (m.ndim() != 2) { return false; }\n"
        "        if (m.shapeAt(0) != n || m.shapeAt(1) != n) { return false; }\n"
        "        int64 i = 0;\n"
        "        while (i < n) {\n"
        "            if (m.get2(i, i) != 0.0) { return false; }\n"
        "            int64 j = 0;\n"
        "            while (j < n) {\n"
        "                if (m.get2(i, j) != m.get2(j, i)) { return false; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return true;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Tensor<float64> x #= D.x43();\n"
        "        Tensor<float64> me #= Distance.pdist(x, heap Euclidean());\n"
        "        if (!D.checkExact(me, 4)) { return -1; }\n"
        "        Tensor<float64> mm #= Distance.pdist(x, heap Manhattan());\n"
        "        if (!D.checkExact(mm, 4)) { return -2; }\n"
        "        Tensor<float64> mo #= Distance.pdist(x, heap Cosine());\n"
        "        if (!D.checkExact(mo, 4)) { return -3; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 1.1.5 — cdist cross-distances match pdist on the overlapping block:
// cdist(X, X)[i][j] == pdist(X)[i][j] at tolerance.
TEST(DistanceTests, cdistMatchesPdistOnOverlap) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        Tensor<float64> x #= D.x43();\n"
        "        Metric m = heap Euclidean();\n"
        "        Tensor<float64> p #= Distance.pdist(x, m);\n"
        "        Tensor<float64> c #= Distance.cdist(x, x, m);\n"
        "        if (c.shapeAt(0) != 4 || c.shapeAt(1) != 4) { return -1; }\n"
        "        int64 i = 0;\n"
        "        while (i < 4) {\n"
        "            int64 j = 0;\n"
        "            while (j < 4) {\n"
        "                if (!D.close(c.get2(i, j), p.get2(i, j))) { return -2; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        Metric mb = heap Manhattan();\n"
        "        Tensor<float64> pb #= Distance.pdist(x, mb);\n"
        "        Tensor<float64> cb #= Distance.cdist(x, x, mb);\n"
        "        i = 0;\n"
        "        while (i < 4) {\n"
        "            int64 j = 0;\n"
        "            while (j < 4) {\n"
        "                if (!D.close(cb.get2(i, j), pb.get2(i, j))) { return -3; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 1.1.6 — cosine of a zero-norm vector is DEFINED (§2.5), never NaN or a
// trap. Doctrine: one zero-norm operand -> similarity 0 / distance 1;
// both zero-norm -> distance 0. (scipy 1.18 yields NaN here; divergence
// is deliberate and documented.)
TEST(DistanceTests, cosineZeroNormDefined) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        Tensor<float64> z #= D.v3(0.0, 0.0, 0.0);\n"
        "        Tensor<float64> b #= D.v3(4.0, 6.0, 3.0);\n"
        "        float64 dz = Distance.cosine(z, b);\n"
        "        if (dz != dz) { return -1; }\n"                // NaN guard
        "        if (!D.close(dz, 1.0)) { return -2; }\n"
        "        float64 sz = Distance.cosineSimilarity(z, b);\n"
        "        if (sz != sz) { return -3; }\n"
        "        if (!D.close(sz, 0.0)) { return -4; }\n"
        "        float64 dzz = Distance.cosine(z, z);\n"
        "        if (dzz != dzz) { return -5; }\n"
        "        if (!D.close(dzz, 0.0)) { return -6; }\n"
        "        float64 dbz = Distance.cosine(b, z);\n"        // symmetric
        "        if (!D.close(dbz, 1.0)) { return -7; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 1.1.7 — Pearson correlation against scipy on fixtures with known
// correlation; correlation DISTANCE (1 - r) matches scipy.correlation.
TEST(DistanceTests, pearsonMatchesScipy) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static #Tensor<float64> v5(float64 a, float64 b, float64 c, float64 d, float64 e) {\n"
        "        float64[] x = heap float64[5]; x[0]=a; x[1]=b; x[2]=c; x[3]=d; x[4]=e;\n"
        "        int64[] sh = heap int64[1]; sh[0] = 5;\n"
        "        return Tensor.of<float64>(x, sh);\n"
        "    }\n"
        "    public static #Tensor<float64> v6(float64 a, float64 b, float64 c, float64 d, float64 e, float64 f) {\n"
        "        float64[] x = heap float64[6]; x[0]=a; x[1]=b; x[2]=c; x[3]=d; x[4]=e; x[5]=f;\n"
        "        int64[] sh = heap int64[1]; sh[0] = 6;\n"
        "        return Tensor.of<float64>(x, sh);\n"
        "    }\n"
        "    public static int32 run() {\n"
        // v = 0.1 + 0.01*u -> r == 1 by construction (scipy: 0.9999999999999999)
        "        Tensor<float64> u #= D.v5(1.0, 2.0, 3.0, 5.0, 8.0);\n"
        "        Tensor<float64> v #= D.v5(0.11, 0.12, 0.13, 0.15, 0.18);\n"
        "        if (!D.close(Distance.pearson(u, v), 0.9999999999999999)) { return -1; }\n"
        "        Tensor<float64> u2 #= D.v6(2.0, 1.0, 4.0, 3.0, 7.0, 5.0);\n"
        "        Tensor<float64> v2 #= D.v6(1.9, 2.2, 3.5, 4.1, 5.6, 7.3);\n"
        "        if (!D.close(Distance.pearson(u2, v2), 0.7984391211489366)) { return -2; }\n"
        "        if (!D.close(Distance.pearsonDistance(u2, v2), 0.20156087885106333)) { return -3; }\n"
        "        Metric mp = heap Pearson();\n"
        "        if (!D.close(mp.distance(u2, v2), 0.20156087885106333)) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 1.1.8 — the expanded-form euclidean path (§2.8) agrees with the direct
// form on well-separated points, and its precision loss on near-zero
// distances is OBSERVABLE, not hidden: large-norm points a tiny distance
// apart lose the distance to cancellation in ||a||^2 + ||b||^2 - 2ab.
// The expanded result must still be finite and non-negative (clamped),
// never NaN from a negative sqrt argument.
TEST(DistanceTests, expandedEuclideanAgreesAndLosesPrecisionObservably) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        // well-separated: expanded (via pdist/cdist) ~ direct, within 1e-9
        "        Tensor<float64> x #= D.x43();\n"
        "        Tensor<float64> p #= Distance.pdist(x, heap Euclidean());\n"
        "        int64 i = 0;\n"
        "        while (i < 4) {\n"
        "            int64 j = 0;\n"
        "            while (j < 4) {\n"
        "                float64 direct = Distance.euclidean(x.index(0, i), x.index(0, j));\n"
        "                float64 diff = p.get2(i, j) - direct;\n"
        "                if (diff < 0.0) { diff = -diff; }\n"
        "                if (diff > 0.000000001) { return -1; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        // adversarial: coords ~1e8, true distance ~1.7e-4
        "        float64[] d = heap float64[6];\n"
        "        d[0] = 100000000.0; d[1] = 100000000.0; d[2] = 100000000.0;\n"
        "        d[3] = 100000000.0001; d[4] = 100000000.0001; d[5] = 100000000.0001;\n"
        "        int64[] sh = heap int64[2]; sh[0] = 2; sh[1] = 3;\n"
        "        Tensor<float64> w #= Tensor.of<float64>(d, sh);\n"
        "        float64 direct = Distance.euclidean(w.index(0, 0), w.index(0, 1));\n"
        "        Tensor<float64> pw #= Distance.pdist(w, heap Euclidean());\n"
        "        float64 expanded = pw.get2(0, 1);\n"
        "        if (expanded != expanded) { return -2; }\n"     // never NaN
        "        if (expanded < 0.0) { return -3; }\n"           // never negative
        // direct form is good here: |direct - true| small relative to true
        "        float64 truth = 0.00017320508075688772;\n"      // sqrt(3)*1e-4
        "        float64 de = direct - truth; if (de < 0.0) { de = -de; }\n"
        "        if (de > 0.00000001) { return -4; }\n"
        // the loss is observable: expanded deviates from direct by far more
        // than the well-separated agreement bound
        "        float64 le = expanded - direct; if (le < 0.0) { le = -le; }\n"
        "        if (le <= 0.000000001) { return -5; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 1.1.9 — the seam is open (§2.7): a metric defined by a CONSUMER (here, in
// this test source) drives pdist/cdist with no changes to the library —
// nothing hardcodes a distance.
TEST(DistanceTests, metricSeamAcceptsUserMetric) {
    std::string src = std::string(PRE) +
        "public final class Discrete implements Metric {\n"
        "    public float64 distance(Tensor<float64> a, Tensor<float64> b) {\n"
        "        int64 n = a.size();\n"
        "        int64 i = 0;\n"
        "        while (i < n) {\n"
        "            if (a.get1(i) != b.get1(i)) { return 1.0; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 0.0;\n"
        "    }\n"
        "}\n" + std::string(HELPERS) +
        "    public static int32 run() {\n"
        "        Tensor<float64> x #= D.x43();\n"
        "        Tensor<float64> m #= Distance.pdist(x, heap Discrete());\n"
        "        int64 i = 0;\n"
        "        while (i < 4) {\n"
        "            int64 j = 0;\n"
        "            while (j < 4) {\n"
        "                float64 want = 1.0;\n"
        "                if (i == j) { want = 0.0; }\n"
        "                if (m.get2(i, j) != want) { return -1; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        Tensor<float64> c #= Distance.cdist(x, D.y23(), heap Discrete());\n"
        "        if (c.shapeAt(0) != 4 || c.shapeAt(1) != 2) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 1.3.3 — every metric matches scipy 1.18.0 on the pdist/cdist fixture
// (condensed order (0,1),(0,2),(0,3),(1,2),(1,3),(2,3) mapped onto the
// square matrix).
TEST(DistanceTests, scipyParityPdistCdist) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static boolean checkP(Tensor<float64> m, float64 a, float64 b, float64 c,\n"
        "                                 float64 d, float64 e, float64 f) {\n"
        "        if (!D.close(m.get2(0, 1), a)) { return false; }\n"
        "        if (!D.close(m.get2(0, 2), b)) { return false; }\n"
        "        if (!D.close(m.get2(0, 3), c)) { return false; }\n"
        "        if (!D.close(m.get2(1, 2), d)) { return false; }\n"
        "        if (!D.close(m.get2(1, 3), e)) { return false; }\n"
        "        if (!D.close(m.get2(2, 3), f)) { return false; }\n"
        "        return true;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Tensor<float64> x #= D.x43();\n"
        "        Tensor<float64> y #= D.y23();\n"
        "        if (!D.checkP(Distance.pdist(x, heap Euclidean()),\n"
        "                4.0828911325187205, 5.386093203798092, 5.033885179461287,\n"
        "                5.224940191045253, 1.7691806012954134, 4.69148164229596)) { return -1; }\n"
        "        if (!D.checkP(Distance.pdist(x, heap Manhattan()),\n"
        "                6.700000000000001, 8.5, 7.2,\n"
        "                8.600000000000001, 2.9000000000000004, 8.1)) { return -2; }\n"
        "        if (!D.checkP(Distance.pdist(x, heap Chebyshev()),\n"
        "                3.3000000000000003, 4.6, 4.5,\n"
        "                4.0, 1.2000000000000002, 2.9000000000000004)) { return -3; }\n"
        "        if (!D.checkP(Distance.pdist(x, heap Cosine()),\n"
        "                1.2883595726427466, 1.142584541318865, 1.9204158728219194,\n"
        "                1.4032697732503743, 0.3602993134657916, 1.0554421130819613)) { return -4; }\n"
        "        if (!D.checkP(Distance.pdist(x, heap Pearson()),\n"
        "                1.5243788202755697, 1.2703332124647717, 1.95118841460467,\n"
        "                1.678024580161325, 0.23843948330579035, 1.0399823789975216)) { return -5; }\n"
        "        if (!D.checkP(Distance.pdist(x, heap Minkowski(3.0)),\n"
        "                3.5768837774467954, 4.843115608904262, 4.670770766009563,\n"
        "                4.535553746949212, 1.529917833456064, 3.9184529852801995)) { return -6; }\n"
        // cdist(X, Y) euclidean, full 4x2 against scipy
        "        Tensor<float64> c #= Distance.cdist(x, y, heap Euclidean());\n"
        "        if (!D.close(c.get2(0, 0), 0.8660254037844386)) { return -7; }\n"
        "        if (!D.close(c.get2(0, 1), 4.1856899072912706)) { return -8; }\n"
        "        if (!D.close(c.get2(1, 0), 4.161730409336962)) { return -9; }\n"
        "        if (!D.close(c.get2(1, 1), 0.8660254037844386)) { return -10; }\n"
        "        if (!D.close(c.get2(2, 0), 5.390732788777422)) { return -11; }\n"
        "        if (!D.close(c.get2(2, 1), 5.239274758971894)) { return -12; }\n"
        "        if (!D.close(c.get2(3, 0), 5.281098370604358)) { return -13; }\n"
        "        if (!D.close(c.get2(3, 1), 2.4041630560342617)) { return -14; }\n"
        // cdist minkowski p=3, spot rows 0 and 3
        "        Tensor<float64> ck #= Distance.cdist(x, y, heap Minkowski(3.0));\n"
        "        if (!D.close(ck.get2(0, 0), 0.7211247851537042)) { return -15; }\n"
        "        if (!D.close(ck.get2(3, 1), 2.1418657848212845)) { return -16; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
