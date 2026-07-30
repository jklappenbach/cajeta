//
// LinAlgSolversTests — linalg-solvers plan (specs/linalg-solvers-spec.md).
// Unit 1: triangular solves (scipy solve_triangular semantics) + multi-RHS
// square solve. Reference oracle = scipy 1.17 / numpy 2.3 values, pinned as
// constants (the NumpyOpsTests §11b convention). f64 tolerance 1e-12 on
// well-conditioned fixtures; f32 1e-3.
//
// Multi-RHS results are additionally asserted BIT-EXACT against repeated
// vector solves: our implementation does identical per-column arithmetic,
// so equality is exact (numpy's own vec-vs-multi pins differ in the last
// ulp — LAPACK takes different paths; both sit inside 1e-12).
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
    "import cajeta.math.linalg.LinAlg;\n"
    "import cajeta.math.linalg.LinAlgException;\n";

// Shared cajeta helpers: close() at the f64/f32 pins, and the Unit-1 fixtures.
// L = [[2,0,0],[1,3,0],[-1,2,4]], U = L^T, b = [2,7,9].
const char* HELPERS =
    "public final class D {\n"
    "    public static boolean close(float64 a, float64 b) {\n"
    "        float64 d = a - b; if (d < 0.0) { d = -d; } return d < 0.000000000001;\n"
    "    }\n"
    "    public static boolean closeF(float32 a, float32 b) {\n"
    "        float32 d = a - b; if (d < 0.0f) { d = -d; } return d < 0.001f;\n"
    "    }\n"
    "    public static #Tensor<float64> mkL() {\n"
    "        float64[] dl = [ 2.0, 0.0, 0.0, 1.0, 3.0, 0.0, -1.0, 2.0, 4.0 ];\n"
    "        int64[] s = heap int64[2]; s[0] = 3; s[1] = 3;\n"
    "        return Tensor.of<float64>(dl, s);\n"
    "    }\n"
    "    public static #Tensor<float64> mkU() {\n"
    "        float64[] du = [ 2.0, 1.0, -1.0, 0.0, 3.0, 2.0, 0.0, 0.0, 4.0 ];\n"
    "        int64[] s = heap int64[2]; s[0] = 3; s[1] = 3;\n"
    "        return Tensor.of<float64>(du, s);\n"
    "    }\n"
    "    public static #Tensor<float64> mkB() {\n"
    "        float64[] db = [ 2.0, 7.0, 9.0 ];\n"
    "        int64[] s = heap int64[1]; s[0] = 3;\n"
    "        return Tensor.of<float64>(db, s);\n"
    "    }\n"
    "    public static #Tensor<float64> mkB2() {\n"
    "        float64[] db = [ 2.0, 1.0, 7.0, -2.0, 9.0, 5.0 ];\n"
    "        int64[] s = heap int64[2]; s[0] = 3; s[1] = 2;\n"
    "        return Tensor.of<float64>(db, s);\n"
    "    }\n";

} // namespace

// 1.1.1 — lower/upper x plain/transposed x unit-diag, vector RHS.
// scipy pins: lower -> [1, 2, 1.5]; upper -> [1.7083333333333333,
// 0.8333333333333334, 2.25]; lower-transposed == the upper solve (U = L^T);
// unit-diag lower -> [2, 5, 1].
TEST(LinAlgSolversTests, solveTriangularVariantsMatchScipy) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        Tensor<float64> l = D.mkL();\n"
        "        Tensor<float64> u = D.mkU();\n"
        "        Tensor<float64> b = D.mkB();\n"
        "        Tensor<float64> x = LinAlg.solveTriangular<float64>(l, b, true, false, false);\n"
        "        if (!D.close(x.get1(0), 1.0) || !D.close(x.get1(1), 2.0) || !D.close(x.get1(2), 1.5)) { return -1; }\n"
        "        Tensor<float64> xu = LinAlg.solveTriangular<float64>(u, b, false, false, false);\n"
        "        if (!D.close(xu.get1(0), 1.7083333333333333)) { return -2; }\n"
        "        if (!D.close(xu.get1(1), 0.8333333333333334)) { return -3; }\n"
        "        if (!D.close(xu.get1(2), 2.25)) { return -4; }\n"
        "        Tensor<float64> xt = LinAlg.solveTriangular<float64>(l, b, true, false, true);\n"
        "        int64 i = 0;\n"
        "        while (i < 3) {\n"
        "            if (xt.get1(i) != xu.get1(i)) { return -5; }\n"  // L^T x = b IS the U solve, bit-exact
        "            i = i + 1;\n"
        "        }\n"
        "        Tensor<float64> xd = LinAlg.solveTriangular<float64>(l, b, true, true, false);\n"
        "        if (!D.close(xd.get1(0), 2.0) || !D.close(xd.get1(1), 5.0) || !D.close(xd.get1(2), 1.0)) { return -6; }\n"
        // f32 lower solve at the f32 tolerance
        "        float32[] dlf = [ 2.0f, 0.0f, 0.0f, 1.0f, 3.0f, 0.0f, -1.0f, 2.0f, 4.0f ];\n"
        "        int64[] s33 = heap int64[2]; s33[0] = 3; s33[1] = 3;\n"
        "        Tensor<float32> lf = Tensor.of<float32>(dlf, s33);\n"
        "        float32[] dbf = [ 2.0f, 7.0f, 9.0f ];\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<float32> bf = Tensor.of<float32>(dbf, s3);\n"
        "        Tensor<float32> xf = LinAlg.solveTriangular<float32>(lf, bf, true, false, false);\n"
        "        if (!D.closeF(xf.get1(0), 1.0f) || !D.closeF(xf.get1(1), 2.0f) || !D.closeF(xf.get1(2), 1.5f)) { return -7; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 1.1.2 — multi-RHS (n,k): scipy pins for the lower solve of B=[[2,1],[7,-2],
// [9,5]], plus bit-exact agreement with column-by-column vector solves.
TEST(LinAlgSolversTests, solveTriangularMultiRhsMatchesColumnwise) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        Tensor<float64> l = D.mkL();\n"
        "        Tensor<float64> bm = D.mkB2();\n"
        "        Tensor<float64> xm = LinAlg.solveTriangular<float64>(l, bm, true, false, false);\n"
        "        if (xm.ndim() != 2 || xm.shapeAt(0) != 3 || xm.shapeAt(1) != 2) { return -1; }\n"
        "        if (!D.close(xm.get2(0, 0), 1.0) || !D.close(xm.get2(0, 1), 0.5)) { return -2; }\n"
        "        if (!D.close(xm.get2(1, 0), 2.0) || !D.close(xm.get2(1, 1), -0.8333333333333333)) { return -3; }\n"
        "        if (!D.close(xm.get2(2, 0), 1.5) || !D.close(xm.get2(2, 1), 1.7916666666666665)) { return -4; }\n"
        // bit-exact vs per-column vector solves
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        int64 c = 0;\n"
        "        while (c < 2) {\n"
        "            Tensor<float64> bc = Tensor.zeros<float64>(s3);\n"
        "            int64 i = 0;\n"
        "            while (i < 3) { bc.set1(i, bm.get2(i, c)); i = i + 1; }\n"
        "            Tensor<float64> xc = LinAlg.solveTriangular<float64>(l, bc, true, false, false);\n"
        "            i = 0;\n"
        "            while (i < 3) {\n"
        "                if (xc.get1(i) != xm.get2(i, c)) { return -5; }\n"
        "                i = i + 1;\n"
        "            }\n"
        "            c = c + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 1.1.3 — solve(A, B) multi-RHS on a pivot-forcing A (A[0][0]=0), numpy pins;
// bit-exact vs the vector solve per column; shape errors throw.
TEST(LinAlgSolversTests, solveMultiRhsMatchesNumpyAndThrowsOnShape) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        float64[] da = [ 0.0, 2.0, 1.0, 1.0, 1.0, -1.0, 3.0, -1.0, 2.0 ];\n"
        "        int64[] s33 = heap int64[2]; s33[0] = 3; s33[1] = 3;\n"
        "        Tensor<float64> a = Tensor.of<float64>(da, s33);\n"
        "        Tensor<float64> bm = D.mkB2();\n"
        "        Tensor<float64> xm = LinAlg.solve<float64>(a, bm);\n"
        "        if (xm.ndim() != 2 || xm.shapeAt(0) != 3 || xm.shapeAt(1) != 2) { return -1; }\n"
        "        if (!D.close(xm.get2(0, 0), 4.285714285714286) || !D.close(xm.get2(0, 1), 0.28571428571428564)) { return -2; }\n"
        "        if (!D.close(xm.get2(1, 0), 1.5714285714285716) || !D.close(xm.get2(1, 1), -0.4285714285714286)) { return -3; }\n"
        "        if (!D.close(xm.get2(2, 0), -1.142857142857143) || !D.close(xm.get2(2, 1), 1.8571428571428572)) { return -4; }\n"
        // bit-exact vs the existing vector path, per column
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        int64 c = 0;\n"
        "        while (c < 2) {\n"
        "            Tensor<float64> bc = Tensor.zeros<float64>(s3);\n"
        "            int64 i = 0;\n"
        "            while (i < 3) { bc.set1(i, bm.get2(i, c)); i = i + 1; }\n"
        "            Tensor<float64> xc = LinAlg.solve<float64>(a, bc);\n"
        "            i = 0;\n"
        "            while (i < 3) {\n"
        "                if (xc.get1(i) != xm.get2(i, c)) { return -5; }\n"
        "                i = i + 1;\n"
        "            }\n"
        "            c = c + 1;\n"
        "        }\n"
        // non-square A throws
        "        float64[] dr = [ 1.0, 2.0, 3.0, 4.0, 5.0, 6.0 ];\n"
        "        int64[] s23 = heap int64[2]; s23[0] = 2; s23[1] = 3;\n"
        "        Tensor<float64> rect = Tensor.of<float64>(dr, s23);\n"
        "        boolean threw = false;\n"
        "        try {\n"
        "            Tensor<float64> bad = LinAlg.solve<float64>(rect, bm);\n"
        "        } catch (LinAlgException ex) {\n"
        "            threw = true;\n"
        "        }\n"
        "        if (!threw) { return -6; }\n"
        // RHS row-count mismatch throws
        "        float64[] d2 = [ 1.0, 2.0 ];\n"
        "        int64[] s2 = heap int64[1]; s2[0] = 2;\n"
        "        Tensor<float64> bshort = Tensor.of<float64>(d2, s2);\n"
        "        threw = false;\n"
        "        try {\n"
        "            Tensor<float64> bad2 = LinAlg.solve<float64>(a, bshort);\n"
        "        } catch (LinAlgException ex) {\n"
        "            threw = true;\n"
        "        }\n"
        "        if (!threw) { return -7; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 2.1.1 — Householder QR on tall (5,3), wide (3,5), square (3,3): reduced
// shapes (Q (m,r), R (r,n), r=min(m,n)), R upper-triangular with diag >= 0,
// Q·R reconstructs A, QᵀQ = I. Invariant-based: for full-rank A these
// properties determine the factorization uniquely, so agreement with
// (sign-normalized) numpy is implied. f64 at 1e-12 + one f32 case at 1e-3.
TEST(LinAlgSolversTests, qrRectangularInvariantsHold) {
    std::string src = std::string(PRE) + HELPERS +
        // checkQr: returns 0 on success, else a negative code offset by `base`.
        "    public static int32 checkQr(Tensor<float64> a, int32 base) {\n"
        "        int64 m = a.shapeAt(0);\n"
        "        int64 n = a.shapeAt(1);\n"
        "        int64 rk = m; if (n < m) { rk = n; }\n"
        "        Tensor<float64>[] f = LinAlg.qr<float64>(a);\n"
        "        Tensor<float64> q = f[0];\n"
        "        Tensor<float64> r = f[1];\n"
        "        if (q.shapeAt(0) != m || q.shapeAt(1) != rk) { return base; }\n"
        "        if (r.shapeAt(0) != rk || r.shapeAt(1) != n) { return base - 1; }\n"
        "        int64 i = 0;\n"
        "        while (i < rk) {\n"
        "            if (r.get2(i, i) < 0.0) { return base - 2; }\n"
        "            int64 j = 0;\n"
        "            while (j < i) {\n"
        "                if (r.get2(i, j) != 0.0) { return base - 3; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        Tensor<float64> rec = Tensor.matmul<float64>(q, r);\n"
        "        i = 0;\n"
        "        while (i < m) {\n"
        "            int64 j = 0;\n"
        "            while (j < n) {\n"
        "                if (!D.close(rec.get2(i, j), a.get2(i, j))) { return base - 4; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        Tensor<float64> qtq = Tensor.matmul<float64>(q.transpose(), q);\n"
        "        i = 0;\n"
        "        while (i < rk) {\n"
        "            int64 j = 0;\n"
        "            while (j < rk) {\n"
        "                float64 want = 0.0; if (i == j) { want = 1.0; }\n"
        "                if (!D.close(qtq.get2(i, j), want)) { return base - 5; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        float64[] dt = [ 2.0, -1.0, 3.0, 1.0, 4.0, 0.0, -2.0, 1.0, 2.0, 3.0, 0.0, -1.0, 1.0, 2.0, 1.0 ];\n"
        "        int64[] s53 = heap int64[2]; s53[0] = 5; s53[1] = 3;\n"
        "        Tensor<float64> tall = Tensor.of<float64>(dt, s53);\n"
        "        int32 rcTall = D.checkQr(tall, -10);\n"
        "        if (rcTall != 0) { return rcTall; }\n"
        "        float64[] dw = [ 1.0, 2.0, 0.0, -1.0, 3.0, 2.0, -1.0, 1.0, 0.0, 1.0, 0.0, 3.0, -2.0, 1.0, 2.0 ];\n"
        "        int64[] s35 = heap int64[2]; s35[0] = 3; s35[1] = 5;\n"
        "        Tensor<float64> wide = Tensor.of<float64>(dw, s35);\n"
        "        int32 rcWide = D.checkQr(wide, -20);\n"
        "        if (rcWide != 0) { return rcWide; }\n"
        "        float64[] dq = [ 4.0, 1.0, 0.0, 1.0, 3.0, 1.0, 0.0, 1.0, 2.0 ];\n"
        "        int64[] s33 = heap int64[2]; s33[0] = 3; s33[1] = 3;\n"
        "        Tensor<float64> sq = Tensor.of<float64>(dq, s33);\n"
        "        int32 rcSq = D.checkQr(sq, -30);\n"
        "        if (rcSq != 0) { return rcSq; }\n"
        // f32 tall: reconstruction at the f32 tolerance
        "        float32[] dtf = [ 2.0f, -1.0f, 1.0f, 4.0f, -2.0f, 1.0f, 3.0f, 0.0f ];\n"
        "        int64[] s42 = heap int64[2]; s42[0] = 4; s42[1] = 2;\n"
        "        Tensor<float32> tf = Tensor.of<float32>(dtf, s42);\n"
        "        Tensor<float32>[] ff = LinAlg.qr<float32>(tf);\n"
        "        Tensor<float32> recf = Tensor.matmul<float32>(ff[0], ff[1]);\n"
        "        int64 i = 0;\n"
        "        while (i < 4) {\n"
        "            int64 j = 0;\n"
        "            while (j < 2) {\n"
        "                if (!D.closeF(recf.get2(i, j), tf.get2(i, j))) { return -40; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 2.1.2 — Läuchli matrix [[1,1,1],[e,0,0],[0,e,0],[0,0,e]] with e = 1e-8 at
// f64 (cond ~ 1.7e8): modified Gram-Schmidt loses orthogonality at the
// cond(A)·eps ~ 1e-8 level here; Householder must hold the m·eps-class bound
// ‖QᵀQ − I‖max < 10·eps·m ≈ 8.9e-15.
TEST(LinAlgSolversTests, qrLauchliOrthogonalityHouseholderBound) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        float64 e = 0.00000001;\n"
        "        float64[] dl = [ 1.0, 1.0, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 ];\n"
        "        int64[] s43 = heap int64[2]; s43[0] = 4; s43[1] = 3;\n"
        "        Tensor<float64> a = Tensor.of<float64>(dl, s43);\n"
        "        a.flatSet(3, e); a.flatSet(7, e); a.flatSet(11, e);\n"  // rows 1..3 diagonal of eps
        "        Tensor<float64>[] f = LinAlg.qr<float64>(a);\n"
        "        Tensor<float64> q = f[0];\n"
        "        Tensor<float64> qtq = Tensor.matmul<float64>(q.transpose(), q);\n"
        "        float64 worst = 0.0;\n"
        "        int64 i = 0;\n"
        "        while (i < 3) {\n"
        "            int64 j = 0;\n"
        "            while (j < 3) {\n"
        "                float64 want = 0.0; if (i == j) { want = 1.0; }\n"
        "                float64 d = qtq.get2(i, j) - want;\n"
        "                if (d < 0.0) { d = -d; }\n"
        "                if (d > worst) { worst = d; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (worst > 0.0000000000000089) { return -1; }\n"  // 10*eps*m, m=4
        // and the factorization still reconstructs the Lauchli matrix
        "        Tensor<float64> rec = Tensor.matmul<float64>(q, f[1]);\n"
        "        i = 0;\n"
        "        while (i < 4) {\n"
        "            int64 j = 0;\n"
        "            while (j < 3) {\n"
        "                if (!D.close(rec.get2(i, j), a.get2(i, j))) { return -2; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 1.1.4 — a zero diagonal in a non-unit triangular solve throws (no NaN
// propagation); the SAME matrix under unitDiag is legal (diagonal ignored).
TEST(LinAlgSolversTests, solveTriangularSingularThrowsUnitDiagIgnores) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        float64[] dz = [ 2.0, 0.0, 0.0, 1.0, 0.0, 0.0, -1.0, 2.0, 4.0 ];\n"
        "        int64[] s33 = heap int64[2]; s33[0] = 3; s33[1] = 3;\n"
        "        Tensor<float64> lz = Tensor.of<float64>(dz, s33);\n"
        "        Tensor<float64> b = D.mkB();\n"
        "        boolean threw = false;\n"
        "        try {\n"
        "            Tensor<float64> bad = LinAlg.solveTriangular<float64>(lz, b, true, false, false);\n"
        "        } catch (LinAlgException ex) {\n"
        "            threw = true;\n"
        "        }\n"
        "        if (!threw) { return -1; }\n"
        // unitDiag: the stored diagonal (including the zero) is ignored
        "        Tensor<float64> xd = LinAlg.solveTriangular<float64>(lz, b, true, true, false);\n"
        "        if (!D.close(xd.get1(0), 2.0) || !D.close(xd.get1(1), 5.0) || !D.close(xd.get1(2), 1.0)) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
