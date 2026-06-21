//
// NumpyOpsTests — numpy-porting-plan Phase 3: creation factories + elementwise
// ufuncs over the Tensor<T> keystone. Reference oracle = numpy semantics.
//
// Phase 2 already provides zeros/ones/full/arange/empty + _like (see TensorTests).
// Phase 3 adds the remaining creators (linspace/eye/meshgrid/arange-range) and the
// ufunc surface. cajeta.math is lazily parsed; importing cajeta.math.Tensor triggers it.
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

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
    "import cajeta.math.Tensor;\n";

} // namespace

// 3a — linspace<E>(start, stop, num): `num` evenly spaced samples over [start, stop]
// INCLUSIVE (numpy default endpoint=true). step = (stop-start)/(num-1); value[i] =
// start + i*step; value[num-1] == stop exactly. num==1 → [start].
TEST(NumpyOpsTests, linspaceMatchesNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tensor<float32> t = Tensor.linspace<float32>(0.0f, 10.0f, 5);\n"  // [0, 2.5, 5, 7.5, 10]
        "        if (t.ndim() != 1) { return -1; }\n"
        "        if (t.size() != 5) { return -2; }\n"
        "        if (t.get1(0) != 0.0f) { return -3; }\n"
        "        if (t.get1(1) != 2.5f) { return -4; }\n"
        "        if (t.get1(2) != 5.0f) { return -5; }\n"
        "        if (t.get1(3) != 7.5f) { return -6; }\n"
        "        if (t.get1(4) != 10.0f) { return -7; }\n"                      // endpoint exact
        "        Tensor<float32> one = Tensor.linspace<float32>(3.0f, 9.0f, 1);\n"
        "        if (one.size() != 1) { return -8; }\n"
        "        if (one.get1(0) != 3.0f) { return -9; }\n"                     // num==1 → [start]
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 3a — eye<E>(n): n x n identity — 1 on the main diagonal, 0 elsewhere.
TEST(NumpyOpsTests, eyeMatchesNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tensor<int32> e = Tensor.eye<int32>(3);\n"
        "        if (e.ndim() != 2) { return -1; }\n"
        "        if (e.shapeAt(0) != 3 || e.shapeAt(1) != 3) { return -2; }\n"
        "        if (e.get2(0, 0) != 1 || e.get2(1, 1) != 1 || e.get2(2, 2) != 1) { return -3; }\n"
        "        if (e.get2(0, 1) != 0 || e.get2(1, 0) != 0 || e.get2(2, 0) != 0) { return -4; }\n"
        "        if (e.get2(0, 2) != 0 || e.get2(2, 1) != 0) { return -5; }\n"
        "        Tensor<float32> ef = Tensor.eye<float32>(2);\n"
        "        if (ef.get2(0, 0) != 1.0f || ef.get2(1, 1) != 1.0f) { return -6; }\n"
        "        if (ef.get2(0, 1) != 0.0f || ef.get2(1, 0) != 0.0f) { return -7; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 3b/3e — elementwise binary arithmetic (add/sub/mul) over the Tensor, same-dtype,
// with right-aligned broadcasting (matches numpy). CPU floor; div/comparison/etc.
// follow in later units (they carry dtype-promotion / bool-result subtleties).
TEST(NumpyOpsTests, elementwiseArithmeticMatchesNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] da = { 1, 2, 3, 4, 5, 6 };\n"
        "        int64[] s23 = heap int64[2];\n"
        "        s23[0] = 2; s23[1] = 3;\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s23);\n"          // [[1,2,3],[4,5,6]]
        "        int32[] db = { 10, 20, 30, 40, 50, 60 };\n"
        "        Tensor<int32> b = Tensor.of<int32>(db, s23);\n"
        "        Tensor<int32> sum = Tensor.add<int32>(a, b);\n"
        "        if (sum.get2(0,0) != 11 || sum.get2(1,2) != 66) { return -1; }\n"
        "        Tensor<int32> diff = Tensor.sub<int32>(b, a);\n"
        "        if (diff.get2(0,0) != 9 || diff.get2(1,2) != 54) { return -2; }\n"
        "        Tensor<int32> prod = Tensor.mul<int32>(a, b);\n"
        "        if (prod.get2(0,1) != 40 || prod.get2(1,0) != 160) { return -3; }\n"  // 2*20, 4*40
        // broadcast a (2,3) + row (3,)
        "        int32[] dr = { 100, 200, 300 };\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<int32> row = Tensor.of<int32>(dr, s3);\n"
        "        Tensor<int32> br = Tensor.add<int32>(a, row);\n"
        "        if (br.ndim() != 2 || br.shapeAt(0) != 2 || br.shapeAt(1) != 3) { return -4; }\n"
        "        if (br.get2(0,0) != 101 || br.get2(1,2) != 306) { return -5; }\n"
        // broadcast a (2,3) + col (2,1)
        "        int32[] dc = { 1000, 2000 };\n"
        "        int64[] s21 = heap int64[2]; s21[0] = 2; s21[1] = 1;\n"
        "        Tensor<int32> col = Tensor.of<int32>(dc, s21);\n"
        "        Tensor<int32> bc = Tensor.add<int32>(a, col);\n"
        "        if (bc.get2(0,0) != 1001 || bc.get2(1,0) != 2004) { return -6; }\n"
        "        if (bc.get2(0,2) != 1003 || bc.get2(1,2) != 2006) { return -7; }\n"
        // float path — the dtype-dispatch check (must emit fadd, not integer add)
        "        float32[] fa = { 1.5f, 2.5f, 3.5f, 4.5f };\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<float32> fA = Tensor.of<float32>(fa, s22);\n"
        "        Tensor<float32> fS = Tensor.add<float32>(fA, fA);\n"
        "        if (fS.get2(0,0) != 3.0f || fS.get2(1,1) != 9.0f) { return -8; }\n"
        "        Tensor<float32> fP = Tensor.mul<float32>(fA, fA);\n"
        "        if (fP.get2(0,0) != 2.25f || fP.get2(1,1) != 20.25f) { return -10; }\n"
        // original operands unchanged (no aliasing/early-free)
        "        if (a.get2(0,0) != 1 || b.get2(1,2) != 60) { return -9; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 1.1.1 — bounded cross-cast kernel: add<A,B,R> on a MIXED dtype pair. Inputs are
// marker-bounded (A,B extends Numeric), the result is an explicit width R; each operand
// is cross-cast (R) and the op performed in R, producing a statically-typed Tensor<R>.
// int32 ⊕ float32 with R=float64 → exact (float64)ai + (float64)bf, no runtime dispatch.
TEST(NumpyOpsTests, boundedCrossCastKernel) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] da = { 1, 2, 3, 4 };\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<int32> ai = Tensor.of<int32>(da, s22);\n"          // [[1,2],[3,4]]
        "        float32[] fb = { 0.5f, 0.5f, 0.5f, 0.5f };\n"
        "        Tensor<float32> bf = Tensor.of<float32>(fb, s22);\n"      // [[.5,.5],[.5,.5]]
        "        Tensor<float64> r = Tensor.add<int32,float32,float64>(ai, bf);\n"
        "        if (r.ndim() != 2 || r.shapeAt(0) != 2 || r.shapeAt(1) != 2) { return -1; }\n"
        "        if (r.get2(0,0) != 1.5) { return -2; }\n"                 // (float64)1 + (float64).5
        "        if (r.get2(0,1) != 2.5) { return -3; }\n"
        "        if (r.get2(1,0) != 3.5) { return -4; }\n"
        "        if (r.get2(1,1) != 4.5) { return -5; }\n"
        // operands unchanged (read-only; no early free)
        "        if (ai.get2(0,0) != 1 || ai.get2(1,1) != 4) { return -6; }\n"
        "        if (bf.get2(0,0) != 0.5f || bf.get2(1,1) != 0.5f) { return -7; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 1.1.3 — sub/mul in the explicit <A,B,R> form on a mixed pair with R = promote(A,B)
// match numpy values + dtype. int32 ⊕ float32 → float64 (NEP-50 §2.2.5).
TEST(NumpyOpsTests, arithmeticPromotedExplicit) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] da = { 1, 2, 3, 4 };\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<int32> ai = Tensor.of<int32>(da, s22);\n"          // [[1,2],[3,4]]
        "        float32[] fb = { 0.5f, 0.5f, 0.5f, 0.5f };\n"
        "        Tensor<float32> bf = Tensor.of<float32>(fb, s22);\n"
        "        Tensor<float64> d = Tensor.sub<float32,int32,float64>(bf, ai);\n"  // .5 - {1,2,3,4}
        "        if (d.get2(0,0) != -0.5) { return -1; }\n"
        "        if (d.get2(1,1) != -3.5) { return -2; }\n"
        "        Tensor<float64> p = Tensor.mul<int32,float32,float64>(ai, bf);\n"  // {1,2,3,4} * .5
        "        if (p.get2(0,0) != 0.5) { return -3; }\n"
        "        if (p.get2(1,1) != 2.0) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 2.1.1 — comparison family eq/ne/lt/le/gt/ge over same-dtype tensors yields a
// Tensor<boolean> matching numpy, with right-aligned broadcasting (spec §6, 6.2.3).
TEST(NumpyOpsTests, comparisonsMatchNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] da = { 1, 2, 3, 4, 5, 6 };\n"
        "        int64[] s23 = heap int64[2]; s23[0] = 2; s23[1] = 3;\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s23);\n"          // [[1,2,3],[4,5,6]]
        "        int32[] db = { 1, 9, 3, 4, 0, 6 };\n"
        "        Tensor<int32> b = Tensor.of<int32>(db, s23);\n"          // [[1,9,3],[4,0,6]]
        "        Tensor<boolean> e = Tensor.eq<int32>(a, b);\n"           // [[T,F,T],[T,F,T]]
        "        if (e.ndim() != 2 || e.shapeAt(0) != 2 || e.shapeAt(1) != 3) { return -1; }\n"
        "        if (!e.get2(0,0) || e.get2(0,1) || !e.get2(0,2)) { return -2; }\n"
        "        if (!e.get2(1,0) || e.get2(1,1) || !e.get2(1,2)) { return -3; }\n"
        "        Tensor<boolean> n = Tensor.ne<int32>(a, b);\n"
        "        if (n.get2(0,0) || !n.get2(0,1)) { return -4; }\n"
        "        Tensor<boolean> lt = Tensor.lt<int32>(a, b);\n"          // [[F,T,F],[F,F,F]]
        "        if (lt.get2(0,0) || !lt.get2(0,1) || lt.get2(1,1)) { return -5; }\n"
        "        Tensor<boolean> le = Tensor.le<int32>(a, b);\n"          // [[T,T,T],[T,F,T]]
        "        if (!le.get2(0,0) || !le.get2(0,1) || le.get2(1,1)) { return -6; }\n"
        "        Tensor<boolean> gt = Tensor.gt<int32>(a, b);\n"          // [[F,F,F],[F,T,F]]
        "        if (gt.get2(0,0) || !gt.get2(1,1)) { return -7; }\n"
        "        Tensor<boolean> ge = Tensor.ge<int32>(a, b);\n"          // [[T,F,T],[T,T,T]]
        "        if (!ge.get2(0,0) || ge.get2(0,1) || !ge.get2(1,1)) { return -8; }\n"
        // broadcast (2,3) < row (3,): row=[1,5,6] → [[F,T,T],[F,F,F]]
        "        int32[] dr = { 1, 5, 6 };\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<int32> row = Tensor.of<int32>(dr, s3);\n"
        "        Tensor<boolean> br = Tensor.lt<int32>(a, row);\n"
        "        if (br.ndim() != 2 || br.shapeAt(0) != 2 || br.shapeAt(1) != 3) { return -9; }\n"
        "        if (br.get2(0,0) || !br.get2(0,1) || !br.get2(0,2)) { return -10; }\n"
        "        if (br.get2(1,1)) { return -11; }\n"
        // operands unchanged
        "        if (a.get2(0,0) != 1 || b.get2(1,2) != 6) { return -12; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 2.1.2 — mixed-dtype comparison compares at the promoted value (explicit compare
// width C) and returns boolean: int32 vs float32 at float64 (spec 6.2.3, 2.2.5).
TEST(NumpyOpsTests, comparisonMixedDtype) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] da = { 3, 4 };\n"
        "        int64[] s2 = heap int64[1]; s2[0] = 2;\n"
        "        Tensor<int32> ai = Tensor.of<int32>(da, s2);\n"         // [3, 4]
        "        float32[] fb = { 3.0f, 3.5f };\n"
        "        Tensor<float32> bf = Tensor.of<float32>(fb, s2);\n"     // [3.0, 3.5]
        "        Tensor<boolean> e = Tensor.eq<int32,float32,float64>(ai, bf);\n"  // [T, F]
        "        if (!e.get1(0) || e.get1(1)) { return -1; }\n"
        "        Tensor<boolean> g = Tensor.gt<int32,float32,float64>(ai, bf);\n"  // [F, T]
        "        if (g.get1(0) || !g.get1(1)) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 3.1.1 — true division: int/int → float64 (NEP-50 true div, spec 6.2.1); float
// same-dtype div<E extends Floating> stays E (spec 2.2.5).
TEST(NumpyOpsTests, trueDivMatchesNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] da = { 6, 7, 8, 9 };\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s22);\n"
        "        int32[] db = { 4, 2, 8, 3 };\n"
        "        Tensor<int32> b = Tensor.of<int32>(db, s22);\n"
        "        Tensor<float64> q = Tensor.div<int32,int32,float64>(a, b);\n"   // [1.5,3.5,1.0,3.0]
        "        if (q.get2(0,0) != 1.5) { return -1; }\n"
        "        if (q.get2(0,1) != 3.5) { return -2; }\n"
        "        if (q.get2(1,0) != 1.0) { return -3; }\n"
        "        if (q.get2(1,1) != 3.0) { return -4; }\n"
        "        float32[] fa = { 1.0f, 3.0f, 7.0f, 9.0f };\n"
        "        Tensor<float32> fA = Tensor.of<float32>(fa, s22);\n"
        "        float32[] fb = { 2.0f, 2.0f, 2.0f, 2.0f };\n"
        "        Tensor<float32> fB = Tensor.of<float32>(fb, s22);\n"
        "        Tensor<float32> fq = Tensor.div<float32,float32,float32>(fA, fB);\n"  // [0.5,1.5,3.5,4.5]
        "        if (fq.get2(0,0) != 0.5f) { return -5; }\n"
        "        if (fq.get2(1,1) != 4.5f) { return -6; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 3.1.2 — floor-division (toward −∞) and mod (sign of divisor): numpy //, % rules
// incl. negative operands; ints stay int, floats match np.floor_divide/np.mod (6.2.2).
TEST(NumpyOpsTests, floorDivAndModMatchNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] da = { 7, -7, 7, -7 };\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s22);\n"
        "        int32[] db = { 2, 2, -2, -2 };\n"
        "        Tensor<int32> b = Tensor.of<int32>(db, s22);\n"
        // numpy: 7//2=3, -7//2=-4, 7//-2=-4, -7//-2=3
        "        Tensor<int32> fd = Tensor.floorDiv<int32>(a, b);\n"
        "        if (fd.get2(0,0) != 3 || fd.get2(0,1) != -4) { return -1; }\n"
        "        if (fd.get2(1,0) != -4 || fd.get2(1,1) != 3) { return -2; }\n"
        // numpy mod (sign of divisor): 7%2=1, -7%2=1, 7%-2=-1, -7%-2=-1
        "        Tensor<int32> m = Tensor.mod<int32>(a, b);\n"
        "        if (m.get2(0,0) != 1 || m.get2(0,1) != 1) { return -3; }\n"
        "        if (m.get2(1,0) != -1 || m.get2(1,1) != -1) { return -4; }\n"
        // float floor_divide/mod
        "        float32[] fa = { -7.0f, 7.5f, -7.5f, 8.0f };\n"
        "        Tensor<float32> fA = Tensor.of<float32>(fa, s22);\n"
        "        float32[] fbv = { 2.0f, 2.0f, 2.0f, 3.0f };\n"
        "        Tensor<float32> fB = Tensor.of<float32>(fbv, s22);\n"
        // floor_divide: -7/2→-4, 7.5/2→3, -7.5/2→-4, 8/3→2
        "        Tensor<float32> ffd = Tensor.floorDiv<float32>(fA, fB);\n"
        "        if (ffd.get2(0,0) != -4.0f || ffd.get2(0,1) != 3.0f) { return -5; }\n"
        "        if (ffd.get2(1,0) != -4.0f || ffd.get2(1,1) != 2.0f) { return -6; }\n"
        // mod: np.mod(-7,2)=1, np.mod(7.5,2)=1.5, np.mod(-7.5,2)=0.5, np.mod(8,3)=2
        "        Tensor<float32> fm = Tensor.mod<float32>(fA, fB);\n"
        "        if (fm.get2(0,0) != 1.0f || fm.get2(0,1) != 1.5f) { return -7; }\n"
        "        if (fm.get2(1,0) != 0.5f || fm.get2(1,1) != 2.0f) { return -8; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 4.1.1 — bitwise and/or/xor/shift on integer tensors match numpy, with promotion to
// the wider result type for mixed-width operands (spec §6 bitwise, Integral domain).
TEST(NumpyOpsTests, bitwiseMatchesNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] da = { 12, 10, 6, 5 };\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s22);\n"
        "        int32[] db = { 10, 6, 3, 1 };\n"
        "        Tensor<int32> b = Tensor.of<int32>(db, s22);\n"
        "        Tensor<int32> an = Tensor.bitAnd<int32>(a, b);\n"          // 8,2,2,1
        "        if (an.get2(0,0)!=8 || an.get2(0,1)!=2 || an.get2(1,0)!=2 || an.get2(1,1)!=1) { return -1; }\n"
        "        Tensor<int32> orr = Tensor.bitOr<int32>(a, b);\n"          // 14,14,7,5
        "        if (orr.get2(0,0)!=14 || orr.get2(0,1)!=14 || orr.get2(1,0)!=7 || orr.get2(1,1)!=5) { return -2; }\n"
        "        Tensor<int32> xr = Tensor.bitXor<int32>(a, b);\n"          // 6,12,5,4
        "        if (xr.get2(0,0)!=6 || xr.get2(0,1)!=12 || xr.get2(1,0)!=5 || xr.get2(1,1)!=4) { return -3; }\n"
        "        int32[] sh = { 1, 2, 0, 3 };\n"
        "        Tensor<int32> sht = Tensor.of<int32>(sh, s22);\n"
        "        Tensor<int32> sl = Tensor.shiftL<int32>(a, sht);\n"        // 24,40,6,40
        "        if (sl.get2(0,0)!=24 || sl.get2(0,1)!=40 || sl.get2(1,0)!=6 || sl.get2(1,1)!=40) { return -4; }\n"
        "        Tensor<int32> sr = Tensor.shiftR<int32>(a, sht);\n"        // 6,2,6,0
        "        if (sr.get2(0,0)!=6 || sr.get2(0,1)!=2 || sr.get2(1,0)!=6 || sr.get2(1,1)!=0) { return -5; }\n"
        // promotion: int16 ⊕ int32 → int32 (mixed-width operands)
        "        int16[] d16 = heap int16[4];\n"
        "        d16[0] = (int16) 12; d16[1] = (int16) 10; d16[2] = (int16) 6; d16[3] = (int16) 5;\n"
        "        Tensor<int16> a16 = Tensor.of<int16>(d16, s22);\n"
        "        Tensor<int32> mp = Tensor.bitAnd<int16,int32,int32>(a16, b);\n"  // 8,2,2,1
        "        if (mp.get2(0,0)!=8 || mp.get2(1,1)!=1) { return -6; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 4.1.2 — logical and/or/xor/not on boolean tensors match numpy (spec §6 logical).
TEST(NumpyOpsTests, logicalMatchesNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        boolean[] ba = heap boolean[4];\n"
        "        ba[0] = true; ba[1] = true; ba[2] = false; ba[3] = false;\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<boolean> A = Tensor.of<boolean>(ba, s22);\n"
        "        boolean[] bb = heap boolean[4];\n"
        "        bb[0] = true; bb[1] = false; bb[2] = true; bb[3] = false;\n"
        "        Tensor<boolean> B = Tensor.of<boolean>(bb, s22);\n"
        "        Tensor<boolean> an = Tensor.and(A, B);\n"                  // T,F,F,F
        "        if (!an.get2(0,0) || an.get2(0,1) || an.get2(1,0) || an.get2(1,1)) { return -1; }\n"
        "        Tensor<boolean> orr = Tensor.or(A, B);\n"                  // T,T,T,F
        "        if (!orr.get2(0,0) || !orr.get2(0,1) || !orr.get2(1,0) || orr.get2(1,1)) { return -2; }\n"
        "        Tensor<boolean> xr = Tensor.xor(A, B);\n"                  // F,T,T,F
        "        if (xr.get2(0,0) || !xr.get2(0,1) || !xr.get2(1,0) || xr.get2(1,1)) { return -3; }\n"
        "        Tensor<boolean> nt = Tensor.not(A);\n"                     // F,F,T,T
        "        if (nt.get2(0,0) || nt.get2(0,1) || !nt.get2(1,0) || !nt.get2(1,1)) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 4.1.3 — bitwise op on a floating tensor is a compile error (Integral domain bound,
// spec 6.2.4). Negative test: the JIT compile throws.
TEST(NumpyOpsTests, bitwiseDomainRejectsFloat) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float32[] fa = { 1.0f, 2.0f };\n"
        "        int64[] s2 = heap int64[1]; s2[0] = 2;\n"
        "        Tensor<float32> a = Tensor.of<float32>(fa, s2);\n"
        "        Tensor<float32> r = Tensor.bitAnd<float32>(a, a);\n"       // float32 ∉ Integral
        "        return r.size() > 0 ? 1 : 0;\n"
        "    }\n"
        "}\n";
    EXPECT_THROW(runI32(src), cajeta::Exception);
}

// 5.1.1 — unary transcendental/rounding on a float tensor match numpy; neg/abs work on
// any numeric (spec §6 transcendental/rounding). Inputs chosen for exact results.
TEST(NumpyOpsTests, unaryFloatMatchesNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        float32[] fa = { 4.0f, 9.0f, 16.0f, 25.0f };\n"
        "        Tensor<float32> t = Tensor.of<float32>(fa, s22);\n"
        "        Tensor<float32> sq = Tensor.sqrt<float32>(t);\n"          // 2,3,4,5
        "        if (sq.get2(0,0)!=2.0f || sq.get2(0,1)!=3.0f || sq.get2(1,0)!=4.0f || sq.get2(1,1)!=5.0f) { return -1; }\n"
        "        Tensor<float32> ng = Tensor.neg<float32>(t);\n"           // -4,-9,-16,-25
        "        if (ng.get2(0,0)!=-4.0f || ng.get2(1,1)!=-25.0f) { return -2; }\n"
        "        Tensor<float32> ab = Tensor.abs<float32>(ng);\n"          // 4,9,16,25
        "        if (ab.get2(0,0)!=4.0f || ab.get2(1,1)!=25.0f) { return -3; }\n"
        "        float32[] zd = { 0.0f, 0.0f, 0.0f, 0.0f };\n"
        "        Tensor<float32> z = Tensor.of<float32>(zd, s22);\n"
        "        Tensor<float32> ex = Tensor.exp<float32>(z);\n"           // 1,1,1,1
        "        if (ex.get2(0,0)!=1.0f || ex.get2(1,1)!=1.0f) { return -4; }\n"
        "        Tensor<float32> si = Tensor.sin<float32>(z);\n"           // 0,0,0,0
        "        if (si.get2(0,0)!=0.0f) { return -5; }\n"
        "        Tensor<float32> co = Tensor.cos<float32>(z);\n"           // 1,1,1,1
        "        if (co.get2(0,0)!=1.0f) { return -6; }\n"
        "        float32[] od = { 1.0f, 1.0f, 1.0f, 1.0f };\n"
        "        Tensor<float32> o = Tensor.of<float32>(od, s22);\n"
        "        Tensor<float32> lg = Tensor.log<float32>(o);\n"           // 0,0,0,0
        "        if (lg.get2(0,0)!=0.0f) { return -7; }\n"
        "        float32[] fr = { 3.7f, 3.2f, -1.2f, -1.8f };\n"
        "        Tensor<float32> tf = Tensor.of<float32>(fr, s22);\n"
        "        Tensor<float32> fl = Tensor.floor<float32>(tf);\n"        // 3,3,-2,-2
        "        if (fl.get2(0,0)!=3.0f || fl.get2(0,1)!=3.0f || fl.get2(1,0)!=-2.0f || fl.get2(1,1)!=-2.0f) { return -8; }\n"
        "        Tensor<float32> cl = Tensor.ceil<float32>(tf);\n"         // 4,4,-1,-1
        "        if (cl.get2(0,0)!=4.0f || cl.get2(0,1)!=4.0f || cl.get2(1,0)!=-1.0f || cl.get2(1,1)!=-1.0f) { return -9; }\n"
        "        Tensor<float32> rd = Tensor.round<float32>(tf);\n"        // 4,3,-1,-2
        "        if (rd.get2(0,0)!=4.0f || rd.get2(0,1)!=3.0f || rd.get2(1,0)!=-1.0f || rd.get2(1,1)!=-2.0f) { return -10; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 5.1.2 — a transcendental on an integer tensor is a compile error (Floating bound,
// spec 6.2.5). neg/abs are NOT rejected (Numeric domain).
TEST(NumpyOpsTests, unaryDomainRejectsInt) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] da = { 1, 2 };\n"
        "        int64[] s2 = heap int64[1]; s2[0] = 2;\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s2);\n"
        "        Tensor<int32> r = Tensor.sin<int32>(a);\n"                // int32 ∉ Floating
        "        return r.size() > 0 ? 1 : 0;\n"
        "    }\n"
        "}\n";
    EXPECT_THROW(runI32(src), cajeta::Exception);
}
