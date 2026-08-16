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
        "        Tensor<float64> l #= D.mkL();\n"
        "        Tensor<float64> u #= D.mkU();\n"
        "        Tensor<float64> b #= D.mkB();\n"
        "        Tensor<float64> x #= LinAlg.solveTriangular<float64>(l, b, true, false, false);\n"
        "        if (!D.close(x.get1(0), 1.0) || !D.close(x.get1(1), 2.0) || !D.close(x.get1(2), 1.5)) { return -1; }\n"
        "        Tensor<float64> xu #= LinAlg.solveTriangular<float64>(u, b, false, false, false);\n"
        "        if (!D.close(xu.get1(0), 1.7083333333333333)) { return -2; }\n"
        "        if (!D.close(xu.get1(1), 0.8333333333333334)) { return -3; }\n"
        "        if (!D.close(xu.get1(2), 2.25)) { return -4; }\n"
        "        Tensor<float64> xt #= LinAlg.solveTriangular<float64>(l, b, true, false, true);\n"
        "        int64 i = 0;\n"
        "        while (i < 3) {\n"
        "            if (xt.get1(i) != xu.get1(i)) { return -5; }\n"  // L^T x = b IS the U solve, bit-exact
        "            i = i + 1;\n"
        "        }\n"
        "        Tensor<float64> xd #= LinAlg.solveTriangular<float64>(l, b, true, true, false);\n"
        "        if (!D.close(xd.get1(0), 2.0) || !D.close(xd.get1(1), 5.0) || !D.close(xd.get1(2), 1.0)) { return -6; }\n"
        // f32 lower solve at the f32 tolerance
        "        float32[] dlf = [ 2.0f, 0.0f, 0.0f, 1.0f, 3.0f, 0.0f, -1.0f, 2.0f, 4.0f ];\n"
        "        int64[] s33 = heap int64[2]; s33[0] = 3; s33[1] = 3;\n"
        "        Tensor<float32> lf #= Tensor.of<float32>(dlf, s33);\n"
        "        float32[] dbf = [ 2.0f, 7.0f, 9.0f ];\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<float32> bf #= Tensor.of<float32>(dbf, s3);\n"
        "        Tensor<float32> xf #= LinAlg.solveTriangular<float32>(lf, bf, true, false, false);\n"
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
        "        Tensor<float64> l #= D.mkL();\n"
        "        Tensor<float64> bm #= D.mkB2();\n"
        "        Tensor<float64> xm #= LinAlg.solveTriangular<float64>(l, bm, true, false, false);\n"
        "        if (xm.ndim() != 2 || xm.shapeAt(0) != 3 || xm.shapeAt(1) != 2) { return -1; }\n"
        "        if (!D.close(xm.get2(0, 0), 1.0) || !D.close(xm.get2(0, 1), 0.5)) { return -2; }\n"
        "        if (!D.close(xm.get2(1, 0), 2.0) || !D.close(xm.get2(1, 1), -0.8333333333333333)) { return -3; }\n"
        "        if (!D.close(xm.get2(2, 0), 1.5) || !D.close(xm.get2(2, 1), 1.7916666666666665)) { return -4; }\n"
        // bit-exact vs per-column vector solves
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        int64 c = 0;\n"
        "        while (c < 2) {\n"
        "            Tensor<float64> bc #= Tensor.zeros<float64>(s3);\n"
        "            int64 i = 0;\n"
        "            while (i < 3) { bc.set1(i, bm.get2(i, c)); i = i + 1; }\n"
        "            Tensor<float64> xc #= LinAlg.solveTriangular<float64>(l, bc, true, false, false);\n"
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
        "        Tensor<float64> a #= Tensor.of<float64>(da, s33);\n"
        "        Tensor<float64> bm #= D.mkB2();\n"
        "        Tensor<float64> xm #= LinAlg.solve<float64>(a, bm);\n"
        "        if (xm.ndim() != 2 || xm.shapeAt(0) != 3 || xm.shapeAt(1) != 2) { return -1; }\n"
        "        if (!D.close(xm.get2(0, 0), 4.285714285714286) || !D.close(xm.get2(0, 1), 0.28571428571428564)) { return -2; }\n"
        "        if (!D.close(xm.get2(1, 0), 1.5714285714285716) || !D.close(xm.get2(1, 1), -0.4285714285714286)) { return -3; }\n"
        "        if (!D.close(xm.get2(2, 0), -1.142857142857143) || !D.close(xm.get2(2, 1), 1.8571428571428572)) { return -4; }\n"
        // bit-exact vs the existing vector path, per column
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        int64 c = 0;\n"
        "        while (c < 2) {\n"
        "            Tensor<float64> bc #= Tensor.zeros<float64>(s3);\n"
        "            int64 i = 0;\n"
        "            while (i < 3) { bc.set1(i, bm.get2(i, c)); i = i + 1; }\n"
        "            Tensor<float64> xc #= LinAlg.solve<float64>(a, bc);\n"
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
        "        Tensor<float64> rect #= Tensor.of<float64>(dr, s23);\n"
        "        boolean threw = false;\n"
        "        try {\n"
        "            Tensor<float64> bad #= LinAlg.solve<float64>(rect, bm);\n"
        "        } catch (LinAlgException ex) {\n"
        "            threw = true;\n"
        "        }\n"
        "        if (!threw) { return -6; }\n"
        // RHS row-count mismatch throws
        "        float64[] d2 = [ 1.0, 2.0 ];\n"
        "        int64[] s2 = heap int64[1]; s2[0] = 2;\n"
        "        Tensor<float64> bshort #= Tensor.of<float64>(d2, s2);\n"
        "        threw = false;\n"
        "        try {\n"
        "            Tensor<float64> bad2 #= LinAlg.solve<float64>(a, bshort);\n"
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
        "        Tensor<float64>[] f #= LinAlg.qr<float64>(a);\n"
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
        "        Tensor<float64> rec #= Tensor.matmul<float64>(q, r);\n"
        "        i = 0;\n"
        "        while (i < m) {\n"
        "            int64 j = 0;\n"
        "            while (j < n) {\n"
        "                if (!D.close(rec.get2(i, j), a.get2(i, j))) { return base - 4; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        Tensor<float64> qtq #= Tensor.matmul<float64>(q.transpose(), q);\n"
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
        "        Tensor<float64> tall #= Tensor.of<float64>(dt, s53);\n"
        "        int32 rcTall = D.checkQr(tall, -10);\n"
        "        if (rcTall != 0) { return rcTall; }\n"
        "        float64[] dw = [ 1.0, 2.0, 0.0, -1.0, 3.0, 2.0, -1.0, 1.0, 0.0, 1.0, 0.0, 3.0, -2.0, 1.0, 2.0 ];\n"
        "        int64[] s35 = heap int64[2]; s35[0] = 3; s35[1] = 5;\n"
        "        Tensor<float64> wide #= Tensor.of<float64>(dw, s35);\n"
        "        int32 rcWide = D.checkQr(wide, -20);\n"
        "        if (rcWide != 0) { return rcWide; }\n"
        "        float64[] dq = [ 4.0, 1.0, 0.0, 1.0, 3.0, 1.0, 0.0, 1.0, 2.0 ];\n"
        "        int64[] s33 = heap int64[2]; s33[0] = 3; s33[1] = 3;\n"
        "        Tensor<float64> sq #= Tensor.of<float64>(dq, s33);\n"
        "        int32 rcSq = D.checkQr(sq, -30);\n"
        "        if (rcSq != 0) { return rcSq; }\n"
        // f32 tall: reconstruction at the f32 tolerance
        "        float32[] dtf = [ 2.0f, -1.0f, 1.0f, 4.0f, -2.0f, 1.0f, 3.0f, 0.0f ];\n"
        "        int64[] s42 = heap int64[2]; s42[0] = 4; s42[1] = 2;\n"
        "        Tensor<float32> tf #= Tensor.of<float32>(dtf, s42);\n"
        "        Tensor<float32>[] ff #= LinAlg.qr<float32>(tf);\n"
        "        Tensor<float32> recf #= Tensor.matmul<float32>(ff[0], ff[1]);\n"
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
        "        Tensor<float64> a #= Tensor.of<float64>(dl, s43);\n"
        "        a.flatSet(3, e); a.flatSet(7, e); a.flatSet(11, e);\n"  // rows 1..3 diagonal of eps
        "        Tensor<float64>[] f #= LinAlg.qr<float64>(a);\n"
        "        Tensor<float64> q = f[0];\n"
        "        Tensor<float64> qtq #= Tensor.matmul<float64>(q.transpose(), q);\n"
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
        "        Tensor<float64> rec #= Tensor.matmul<float64>(q, f[1]);\n"
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

// 3.1.1 / 3.1.2 — factor application: choSolve from the cholesky factor and
// luSolve from the [P,L,U] bag each agree with the direct solve, vector and
// multi-RHS, f64 (1e-12) + one f32 choSolve case (1e-3). The LU fixture is
// the pivot-forcing A from the Unit-1 test (A[0][0] = 0).
TEST(LinAlgSolversTests, choSolveLuSolveMatchDirectSolve) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        // SPD fixture
        "        float64[] ds = [ 4.0, 1.0, 1.0, 1.0, 3.0, 0.0, 1.0, 0.0, 2.0 ];\n"
        "        int64[] s33 = heap int64[2]; s33[0] = 3; s33[1] = 3;\n"
        "        Tensor<float64> spd #= Tensor.of<float64>(ds, s33);\n"
        "        Tensor<float64> b #= D.mkB();\n"
        "        Tensor<float64> bm #= D.mkB2();\n"
        "        Tensor<float64> lch #= LinAlg.cholesky<float64>(spd);\n"
        "        Tensor<float64> xc #= LinAlg.choSolve<float64>(lch, b);\n"
        "        Tensor<float64> xd #= LinAlg.solve<float64>(spd, b);\n"
        "        int64 i = 0;\n"
        "        while (i < 3) {\n"
        "            if (!D.close(xc.get1(i), xd.get1(i))) { return -1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        Tensor<float64> xcm #= LinAlg.choSolve<float64>(lch, bm);\n"
        "        Tensor<float64> xdm #= LinAlg.solve<float64>(spd, bm);\n"
        "        i = 0;\n"
        "        while (i < 3) {\n"
        "            int64 c = 0;\n"
        "            while (c < 2) {\n"
        "                if (!D.close(xcm.get2(i, c), xdm.get2(i, c))) { return -2; }\n"
        "                c = c + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        // luSolve on the pivot-forcing fixture
        "        float64[] da = [ 0.0, 2.0, 1.0, 1.0, 1.0, -1.0, 3.0, -1.0, 2.0 ];\n"
        "        Tensor<float64> a #= Tensor.of<float64>(da, s33);\n"
        "        Tensor<float64>[] plu #= LinAlg.lu<float64>(a);\n"
        "        Tensor<float64> xl #= LinAlg.luSolve<float64>(plu[0], plu[1], plu[2], b);\n"
        "        Tensor<float64> xs #= LinAlg.solve<float64>(a, b);\n"
        "        i = 0;\n"
        "        while (i < 3) {\n"
        "            if (!D.close(xl.get1(i), xs.get1(i))) { return -3; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        Tensor<float64> xlm #= LinAlg.luSolve<float64>(plu[0], plu[1], plu[2], bm);\n"
        "        Tensor<float64> xsm #= LinAlg.solve<float64>(a, bm);\n"
        "        i = 0;\n"
        "        while (i < 3) {\n"
        "            int64 c = 0;\n"
        "            while (c < 2) {\n"
        "                if (!D.close(xlm.get2(i, c), xsm.get2(i, c))) { return -4; }\n"
        "                c = c + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        // f32 choSolve
        "        float32[] dsf = [ 4.0f, 1.0f, 1.0f, 1.0f, 3.0f, 0.0f, 1.0f, 0.0f, 2.0f ];\n"
        "        Tensor<float32> spdf #= Tensor.of<float32>(dsf, s33);\n"
        "        float32[] dbf = [ 2.0f, 7.0f, 9.0f ];\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<float32> bf #= Tensor.of<float32>(dbf, s3);\n"
        "        Tensor<float32> lchf #= LinAlg.cholesky<float32>(spdf);\n"
        "        Tensor<float32> xcf #= LinAlg.choSolve<float32>(lchf, bf);\n"
        "        Tensor<float32> xdf #= LinAlg.solve<float32>(spdf, bf);\n"
        "        i = 0;\n"
        "        while (i < 3) {\n"
        "            if (!D.closeF(xcf.get1(i), xdf.get1(i))) { return -5; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 3.1.3 / 3.1.4 — consumer-grade lstsq on a (20,3) quadratic-fit design
// matrix (rows [1, t, t^2], t = i/10, deterministic +-0.01 noise; the
// fixture is reconstructed in-language with the same f64 expressions numpy
// evaluated, so inputs are bit-identical to the pin computation).
// numpy pins: x = [2.0014285714285713, 0.4984962406015036,
// -1.4999999999999996]; second RHS column [-1.0014285714285716,
// 1.0015037593984961, 0.24999999999999978]. Square input equals solve;
// normal-equations path (choSolve of A^T A) agrees to 1e-8.
TEST(LinAlgSolversTests, lstsqTallMatchesNumpyAndNormalEquations) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        int64[] s203 = heap int64[2]; s203[0] = 20; s203[1] = 3;\n"
        "        Tensor<float64> a #= Tensor.zeros<float64>(s203);\n"
        "        int64[] s20 = heap int64[1]; s20[0] = 20;\n"
        "        Tensor<float64> y1 #= Tensor.zeros<float64>(s20);\n"
        "        int64[] s202 = heap int64[2]; s202[0] = 20; s202[1] = 2;\n"
        "        Tensor<float64> ym #= Tensor.zeros<float64>(s202);\n"
        "        int64 i = 0;\n"
        "        while (i < 20) {\n"
        "            float64 t = (float64) i / 10.0;\n"
        "            float64 noise = 0.01;\n"
        "            if (i % 2 == 1) { noise = -0.01; }\n"
        "            a.flatSet(i * 3, 1.0);\n"
        "            a.flatSet(i * 3 + 1, t);\n"
        "            a.flatSet(i * 3 + 2, t * t);\n"
        "            float64 v1 = 2.0 + 0.5 * t - 1.5 * (t * t) + noise;\n"
        "            float64 v2 = -1.0 + 1.0 * t + 0.25 * (t * t) - noise;\n"
        "            y1.set1(i, v1);\n"
        "            ym.flatSet(i * 2, v1);\n"
        "            ym.flatSet(i * 2 + 1, v2);\n"
        "            i = i + 1;\n"
        "        }\n"
        // vector lstsq vs numpy
        "        Tensor<float64> x #= LinAlg.lstsq<float64>(a, y1);\n"
        "        if (x.ndim() != 1 || x.shapeAt(0) != 3) { return -1; }\n"
        "        if (!D.close(x.get1(0), 2.0014285714285713)) { return -2; }\n"
        "        if (!D.close(x.get1(1), 0.4984962406015036)) { return -3; }\n"
        "        if (!D.close(x.get1(2), -1.4999999999999996)) { return -4; }\n"
        // multi-RHS: column 0 matches the vector solve, column 1 matches numpy
        "        Tensor<float64> xm #= LinAlg.lstsq<float64>(a, ym);\n"
        "        if (xm.ndim() != 2 || xm.shapeAt(0) != 3 || xm.shapeAt(1) != 2) { return -5; }\n"
        "        i = 0;\n"
        "        while (i < 3) {\n"
        "            if (xm.get2(i, 0) != x.get1(i)) { return -6; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (!D.close(xm.get2(0, 1), -1.0014285714285716)) { return -7; }\n"
        "        if (!D.close(xm.get2(1, 1), 1.0015037593984961)) { return -8; }\n"
        "        if (!D.close(xm.get2(2, 1), 0.24999999999999978)) { return -9; }\n"
        // square input equals solve (well-conditioned)
        "        float64[] dq = [ 4.0, 1.0, 0.0, 1.0, 3.0, 1.0, 0.0, 1.0, 2.0 ];\n"
        "        int64[] s33 = heap int64[2]; s33[0] = 3; s33[1] = 3;\n"
        "        Tensor<float64> sq #= Tensor.of<float64>(dq, s33);\n"
        "        Tensor<float64> b3 #= D.mkB();\n"
        "        Tensor<float64> xq #= LinAlg.lstsq<float64>(sq, b3);\n"
        "        Tensor<float64> xqs #= LinAlg.solve<float64>(sq, b3);\n"
        "        i = 0;\n"
        "        while (i < 3) {\n"
        "            if (!D.close(xq.get1(i), xqs.get1(i))) { return -10; }\n"
        "            i = i + 1;\n"
        "        }\n"
        // 3.1.4 normal equations: choSolve(cholesky(A^T A), A^T y) vs lstsq
        "        Tensor<float64> g #= Tensor.matmul<float64>(a.transpose(), a);\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<float64> aty #= Tensor.zeros<float64>(s3);\n"
        "        i = 0;\n"
        "        while (i < 3) {\n"
        "            float64 acc = 0.0;\n"
        "            int64 r = 0;\n"
        "            while (r < 20) {\n"
        "                acc = acc + a.get2(r, i) * y1.get1(r);\n"
        "                r = r + 1;\n"
        "            }\n"
        "            aty.set1(i, acc);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        Tensor<float64> lg #= LinAlg.cholesky<float64>(g);\n"
        "        Tensor<float64> xn #= LinAlg.choSolve<float64>(lg, aty);\n"
        "        i = 0;\n"
        "        while (i < 3) {\n"
        "            float64 dd = xn.get1(i) - x.get1(i);\n"
        "            if (dd < 0.0) { dd = -dd; }\n"
        "            if (dd > 0.00000001) { return -11; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 4.1.1 — bidiagonal svd on tall (5,3) and wide (3,5): numpy singular-value
// pins (1e-12), reduced shapes, descending order, U·diag(S)·Vt reconstruction,
// orthonormal U columns and Vt rows.
TEST(LinAlgSolversTests, svdRectangularMatchesNumpy) {
    std::string src = std::string(PRE) + HELPERS +
        // checks svd(a) for (m,n): shape/order/recon/orthogonality; sv pins passed in
        "    public static int32 checkSvd(Tensor<float64> a, Tensor<float64> pins, int32 base) {\n"
        "        int64 m = a.shapeAt(0);\n"
        "        int64 n = a.shapeAt(1);\n"
        "        int64 rk = m; if (n < m) { rk = n; }\n"
        "        Tensor<float64>[] f #= LinAlg.svd<float64>(a);\n"
        "        Tensor<float64> u = f[0];\n"
        "        Tensor<float64> s = f[1];\n"
        "        Tensor<float64> vt = f[2];\n"
        "        if (u.shapeAt(0) != m || u.shapeAt(1) != rk) { return base; }\n"
        "        if (s.shapeAt(0) != rk) { return base - 1; }\n"
        "        if (vt.shapeAt(0) != rk || vt.shapeAt(1) != n) { return base - 2; }\n"
        "        int64 i = 0;\n"
        "        while (i < rk) {\n"
        "            if (!D.close(s.get1(i), pins.get1(i))) { return base - 3; }\n"
        "            if (i > 0) { if (s.get1(i) > s.get1(i - 1)) { return base - 4; } }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        i = 0;\n"
        "        while (i < m) {\n"
        "            int64 j = 0;\n"
        "            while (j < n) {\n"
        "                float64 acc = 0.0;\n"
        "                int64 k = 0;\n"
        "                while (k < rk) {\n"
        "                    acc = acc + u.get2(i, k) * s.get1(k) * vt.get2(k, j);\n"
        "                    k = k + 1;\n"
        "                }\n"
        "                if (!D.close(acc, a.get2(i, j))) { return base - 5; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        Tensor<float64> utu #= Tensor.matmul<float64>(u.transpose(), u);\n"
        "        Tensor<float64> vvt #= Tensor.matmul<float64>(vt, vt.transpose());\n"
        "        i = 0;\n"
        "        while (i < rk) {\n"
        "            int64 j = 0;\n"
        "            while (j < rk) {\n"
        "                float64 want = 0.0; if (i == j) { want = 1.0; }\n"
        "                if (!D.close(utu.get2(i, j), want)) { return base - 6; }\n"
        "                if (!D.close(vvt.get2(i, j), want)) { return base - 7; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        float64[] dt = [ 2.0, -1.0, 3.0, 1.0, 4.0, 0.0, -2.0, 1.0, 2.0, 3.0, 0.0, -1.0, 1.0, 2.0, 1.0 ];\n"
        "        int64[] s53 = heap int64[2]; s53[0] = 5; s53[1] = 3;\n"
        "        Tensor<float64> tall #= Tensor.of<float64>(dt, s53);\n"
        "        float64[] pt = [ 4.806168449760469, 4.249945163697459, 3.85210474131891 ];\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<float64> pinsT #= Tensor.of<float64>(pt, s3);\n"
        "        int32 rcT = D.checkSvd(tall, pinsT, -10);\n"
        "        if (rcT != 0) { return rcT; }\n"
        "        float64[] dw = [ 1.0, 2.0, 0.0, -1.0, 3.0, 2.0, -1.0, 1.0, 0.0, 1.0, 0.0, 3.0, -2.0, 1.0, 2.0 ];\n"
        "        int64[] s35 = heap int64[2]; s35[0] = 3; s35[1] = 5;\n"
        "        Tensor<float64> wide #= Tensor.of<float64>(dw, s35);\n"
        "        float64[] pw = [ 5.254138363048124, 3.24102506057107, 1.3746951002663361 ];\n"
        "        Tensor<float64> pinsW #= Tensor.of<float64>(pw, s3);\n"
        "        int32 rcW = D.checkSvd(wide, pinsW, -20);\n"
        "        if (rcW != 0) { return rcW; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 4.1.2 — the accuracy case the Gram route provably fails: (4,3) f32 matrix
// with cond ~ 1e4 (built from orthogonal factors, singular values
// [10, 0.05, 0.001], rounded to f32). numpy f32 pins: [10.0, 4.999999e-2,
// 9.999771e-4]; tolerance 3e-5 (the m·eps class). The eigh(AᵀA) route
// computes sigma3 ~ 1.898e-3 — off by 9e-4, 30x outside this bound.
TEST(LinAlgSolversTests, svdF32IllConditionedBeatsGramRoute) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        float32[] da = [ 3.6491716f, 1.8303664f, -0.034683466f, -0.0009065672f, 0.0018131344f, -0.01903647f, 7.304767f, 3.6478841f, 0.025184933f, 3.6501963f, 1.8283168f, -0.0156864f ];\n"
        "        int64[] s43 = heap int64[2]; s43[0] = 4; s43[1] = 3;\n"
        "        Tensor<float32> a #= Tensor.of<float32>(da, s43);\n"
        "        Tensor<float32>[] f #= LinAlg.svd<float32>(a);\n"
        "        Tensor<float32> s = f[1];\n"
        "        float32 d0 = s.get1(0) - 10.0f; if (d0 < 0.0f) { d0 = -d0; }\n"
        "        float32 d1 = s.get1(1) - 0.04999999f; if (d1 < 0.0f) { d1 = -d1; }\n"
        "        float32 d2 = s.get1(2) - 0.0009999771f; if (d2 < 0.0f) { d2 = -d2; }\n"
        "        if (d0 > 0.0001f) { return -1; }\n"
        "        if (d1 > 0.00003f) { return -2; }\n"
        "        if (d2 > 0.00003f) { return -3; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 4.1.3 / 4.1.4 — rectangular pinv satisfies the four Moore-Penrose
// identities (1e-10); matrixRank under numpy tolerance semantics: diag(1,
// 1e-5) is rank 2 (the old sigma_max*1e-4 rule misclassified it as 1) and
// the exactly-singular [[1,2],[2,4]] stays rank 1.
TEST(LinAlgSolversTests, pinvMoorePenroseAndRankTolerance) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static boolean closeM(Tensor<float64> x, Tensor<float64> y) {\n"
        "        int64 m = x.shapeAt(0);\n"
        "        int64 n = x.shapeAt(1);\n"
        "        int64 i = 0;\n"
        "        while (i < m) {\n"
        "            int64 j = 0;\n"
        "            while (j < n) {\n"
        "                float64 d = x.get2(i, j) - y.get2(i, j);\n"
        "                if (d < 0.0) { d = -d; }\n"
        "                if (d > 0.0000000001) { return false; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return true;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        float64[] dt = [ 2.0, -1.0, 3.0, 1.0, 4.0, 0.0, -2.0, 1.0, 2.0, 3.0, 0.0, -1.0, 1.0, 2.0, 1.0 ];\n"
        "        int64[] s53 = heap int64[2]; s53[0] = 5; s53[1] = 3;\n"
        "        Tensor<float64> a #= Tensor.of<float64>(dt, s53);\n"
        "        Tensor<float64> p #= LinAlg.pinv<float64>(a);\n"
        "        if (p.shapeAt(0) != 3 || p.shapeAt(1) != 5) { return -1; }\n"
        "        Tensor<float64> ap #= Tensor.matmul<float64>(a, p);\n"       // (5,5)
        "        Tensor<float64> pa #= Tensor.matmul<float64>(p, a);\n"       // (3,3)
        "        if (!D.closeM(Tensor.matmul<float64>(ap, a), a)) { return -2; }\n"
        "        if (!D.closeM(Tensor.matmul<float64>(pa, p), p)) { return -3; }\n"
        "        if (!D.closeM(ap.transpose().copy(), ap)) { return -4; }\n"
        "        if (!D.closeM(pa.transpose().copy(), pa)) { return -5; }\n"
        // rank tolerance semantics
        "        float64[] dd = [ 1.0, 0.0, 0.0, 0.00001 ];\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<float64> nearDef #= Tensor.of<float64>(dd, s22);\n"
        "        if (LinAlg.matrixRank<float64>(nearDef) != 2) { return -6; }\n"
        "        float64[] ds = [ 1.0, 2.0, 2.0, 4.0 ];\n"
        "        Tensor<float64> sing #= Tensor.of<float64>(ds, s22);\n"
        "        if (LinAlg.matrixRank<float64>(sing) != 1) { return -7; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 4.1.5 — lstsq minimum-norm: rank-deficient tall (col2 = 2*col1) matches
// numpy's min-norm solution; the underdetermined m<n case now solves (exact
// on a consistent system, x = A^T(AA^T)^-1 b).
TEST(LinAlgSolversTests, lstsqRankDeficientAndUnderdeterminedMinNorm) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        float64[] da = [ 1.0, 2.0, 2.0, 4.0, 3.0, 6.0, 4.0, 8.0, 5.0, 10.0 ];\n"
        "        int64[] s52 = heap int64[2]; s52[0] = 5; s52[1] = 2;\n"
        "        Tensor<float64> a #= Tensor.of<float64>(da, s52);\n"
        "        float64[] db = [ 1.0, 3.0, 2.0, 5.0, 4.0 ];\n"
        "        int64[] s5 = heap int64[1]; s5[0] = 5;\n"
        "        Tensor<float64> b #= Tensor.of<float64>(db, s5);\n"
        "        Tensor<float64> x #= LinAlg.lstsq<float64>(a, b);\n"
        "        if (!D.close(x.get1(0), 0.19272727272727275)) { return -1; }\n"
        "        if (!D.close(x.get1(1), 0.3854545454545455)) { return -2; }\n"
        // m < n: A=[[1,0,1],[0,1,1]], b=[2,3] -> min-norm [1/3, 4/3, 5/3]
        "        float64[] du = [ 1.0, 0.0, 1.0, 0.0, 1.0, 1.0 ];\n"
        "        int64[] s23 = heap int64[2]; s23[0] = 2; s23[1] = 3;\n"
        "        Tensor<float64> au #= Tensor.of<float64>(du, s23);\n"
        "        float64[] db2 = [ 2.0, 3.0 ];\n"
        "        int64[] s2 = heap int64[1]; s2[0] = 2;\n"
        "        Tensor<float64> b2 #= Tensor.of<float64>(db2, s2);\n"
        "        Tensor<float64> xu #= LinAlg.lstsq<float64>(au, b2);\n"
        "        if (xu.shapeAt(0) != 3) { return -3; }\n"
        "        if (!D.close(xu.get1(0), 1.0 / 3.0)) { return -4; }\n"
        "        if (!D.close(xu.get1(1), 4.0 / 3.0)) { return -5; }\n"
        "        if (!D.close(xu.get1(2), 5.0 / 3.0)) { return -6; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 5.1.1 — slogdet vs numpy: [[1,2],[3,4]] -> (-1, log 2); singular
// [[1,2],[2,4]] -> (0, -inf); diag(1e6) at (60,60), where det overflows f64
// to inf, slogdet = 60*log(1e6) = 828.9306334778564 exactly to 1e-9.
TEST(LinAlgSolversTests, slogdetMatchesNumpyIncludingOverflow) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        float64[] da = [ 1.0, 2.0, 3.0, 4.0 ];\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<float64> a #= Tensor.of<float64>(da, s22);\n"
        "        Tensor<float64> sd #= LinAlg.slogdet<float64>(a);\n"
        "        if (!D.close(sd.get1(0), -1.0)) { return -1; }\n"
        "        if (!D.close(sd.get1(1), 0.6931471805599453)) { return -2; }\n"
        "        float64[] ds = [ 1.0, 2.0, 2.0, 4.0 ];\n"
        "        Tensor<float64> sing #= Tensor.of<float64>(ds, s22);\n"
        "        Tensor<float64> sds #= LinAlg.slogdet<float64>(sing);\n"
        "        if (sds.get1(0) != 0.0) { return -3; }\n"
        "        if (sds.get1(1) > -100000000000.0) { return -4; }\n"   // -inf
        "        int64[] s60 = heap int64[2]; s60[0] = 60; s60[1] = 60;\n"
        "        Tensor<float64> big #= Tensor.zeros<float64>(s60);\n"
        "        int64 i = 0;\n"
        "        while (i < 60) { big.flatSet(i * 60 + i, 1000000.0); i = i + 1; }\n"
        "        Tensor<float64> sdb #= LinAlg.slogdet<float64>(big);\n"
        "        if (!D.close(sdb.get1(0), 1.0)) { return -5; }\n"
        "        float64 dv = sdb.get1(1) - 828.9306334778564;\n"
        "        if (dv < 0.0) { dv = -dv; }\n"
        "        if (dv > 0.000000001) { return -6; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 5.1.2 — norms vs numpy: vector [3,-4,2,-1] -> 1-norm 10, 2-norm sqrt(30),
// inf-norm 4, 3-norm 100^(1/3); matrix (2,3) [[1,-2,3],[-4,5,-6]] ->
// fro sqrt(91), induced-1 9, induced-inf 15.
TEST(LinAlgSolversTests, normsMatchNumpy) {
    std::string src = std::string(PRE) + HELPERS +
        "    public static int32 run() {\n"
        "        float64[] dx = [ 3.0, -4.0, 2.0, -1.0 ];\n"
        "        int64[] s4 = heap int64[1]; s4[0] = 4;\n"
        "        Tensor<float64> x #= Tensor.of<float64>(dx, s4);\n"
        "        if (!D.close(LinAlg.normVec<float64>(x, 1.0), 10.0)) { return -1; }\n"
        "        if (!D.close(LinAlg.normVec<float64>(x, 2.0), 5.477225575051661)) { return -2; }\n"
        "        if (!D.close(LinAlg.normVec<float64>(x, 3.0), 4.641588833612779)) { return -3; }\n"
        "        if (!D.close(LinAlg.normInfVec<float64>(x), 4.0)) { return -4; }\n"
        "        float64[] dm = [ 1.0, -2.0, 3.0, -4.0, 5.0, -6.0 ];\n"
        "        int64[] s23 = heap int64[2]; s23[0] = 2; s23[1] = 3;\n"
        "        Tensor<float64> a #= Tensor.of<float64>(dm, s23);\n"
        "        if (!D.close(LinAlg.normFro<float64>(a), 9.539392014169456)) { return -5; }\n"
        "        if (!D.close(LinAlg.norm1<float64>(a), 9.0)) { return -6; }\n"
        "        if (!D.close(LinAlg.normInf<float64>(a), 15.0)) { return -7; }\n"
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
        "        Tensor<float64> lz #= Tensor.of<float64>(dz, s33);\n"
        "        Tensor<float64> b #= D.mkB();\n"
        "        boolean threw = false;\n"
        "        try {\n"
        "            Tensor<float64> bad #= LinAlg.solveTriangular<float64>(lz, b, true, false, false);\n"
        "        } catch (LinAlgException ex) {\n"
        "            threw = true;\n"
        "        }\n"
        "        if (!threw) { return -1; }\n"
        // unitDiag: the stored diagonal (including the zero) is ignored
        "        Tensor<float64> xd #= LinAlg.solveTriangular<float64>(lz, b, true, true, false);\n"
        "        if (!D.close(xd.get1(0), 2.0) || !D.close(xd.get1(1), 5.0) || !D.close(xd.get1(2), 1.0)) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
