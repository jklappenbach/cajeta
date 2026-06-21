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
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// 3c — the GPU-lowering tests need the XPU backend enabled so @Kernel lowers and
// launches (on the portable CPU backend in-process, same discipline as
// TensorTests::runI32Xpu). The Tensor op routes on placement; the kernel runs
// through the real launch FFI either way.
int32_t runI32Xpu(const std::string& src) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(src, "test.D", o);
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

// 3a — arange<E>(start, stop, step): half-open [start, stop) in steps of `step`
// (numpy 3-arg arange). count = ceil((stop-start)/step), clamped to >= 0; value[i]
// = start + i*step. Coexists with the 1-arg arange<E>(n) via value-param arity.
TEST(NumpyOpsTests, arangeRangeMatchesNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Tensor<int32> r = Tensor.arange<int32>(2, 10, 2);\n"            // [2,4,6,8]
        "        if (r.ndim() != 1) { return -1; }\n"
        "        if (r.size() != 4) { return -2; }\n"
        "        if (r.get1(0) != 2 || r.get1(1) != 4 || r.get1(2) != 6 || r.get1(3) != 8) { return -3; }\n"
        "        Tensor<float32> f = Tensor.arange<float32>(0.0f, 1.0f, 0.25f);\n" // [0,0.25,0.5,0.75]
        "        if (f.size() != 4) { return -4; }\n"
        "        if (f.get1(0) != 0.0f || f.get1(1) != 0.25f || f.get1(2) != 0.5f || f.get1(3) != 0.75f) { return -5; }\n"
        "        Tensor<int32> neg = Tensor.arange<int32>(10, 0, -3);\n"          // [10,7,4,1]
        "        if (neg.size() != 4) { return -6; }\n"
        "        if (neg.get1(0) != 10 || neg.get1(1) != 7 || neg.get1(2) != 4 || neg.get1(3) != 1) { return -7; }\n"
        "        Tensor<int32> empty = Tensor.arange<int32>(5, 5, 1);\n"          // []
        "        if (empty.size() != 0) { return -8; }\n"
        "        Tensor<int32> one = Tensor.arange<int32>(7);\n"                  // 1-arg still resolves
        "        if (one.size() != 7 || one.get1(6) != 6) { return -9; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 3a — meshgrid<E>(x, y): coordinate grids (numpy default 'xy' indexing). For x of
// length Nx and y of length Ny returns [X, Y], each shaped (Ny, Nx), with
// X[i,j] = x[j] and Y[i,j] = y[i].
TEST(NumpyOpsTests, meshgridMatchesNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] dx = { 1, 2, 3 };\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<int32> x = Tensor.of<int32>(dx, s3);\n"
        "        int32[] dy = { 10, 20 };\n"
        "        int64[] s2 = heap int64[1]; s2[0] = 2;\n"
        "        Tensor<int32> y = Tensor.of<int32>(dy, s2);\n"
        "        Tensor<int32>[] g = Tensor.meshgrid<int32>(x, y);\n"
        "        Tensor<int32> X = g[0];\n"
        "        Tensor<int32> Y = g[1];\n"
        "        if (X.ndim() != 2 || Y.ndim() != 2) { return -1; }\n"
        "        if (X.shapeAt(0) != 2 || X.shapeAt(1) != 3) { return -2; }\n"    // (Ny,Nx) = (2,3)
        "        if (Y.shapeAt(0) != 2 || Y.shapeAt(1) != 3) { return -3; }\n"
        "        if (X.get2(0, 0) != 1 || X.get2(0, 1) != 2 || X.get2(0, 2) != 3) { return -4; }\n"
        "        if (X.get2(1, 0) != 1 || X.get2(1, 1) != 2 || X.get2(1, 2) != 3) { return -5; }\n"
        "        if (Y.get2(0, 0) != 10 || Y.get2(0, 1) != 10 || Y.get2(0, 2) != 10) { return -6; }\n"
        "        if (Y.get2(1, 0) != 20 || Y.get2(1, 1) != 20 || Y.get2(1, 2) != 20) { return -7; }\n"
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

// 6.1.1 — where(cond, a, b) selects elementwise with broadcasting; result dtype is the
// explicit promote(A,B) width (spec §6 where).
TEST(NumpyOpsTests, whereMatchesNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        boolean[] cd = heap boolean[4];\n"
        "        cd[0] = true; cd[1] = false; cd[2] = true; cd[3] = false;\n"
        "        Tensor<boolean> cond = Tensor.of<boolean>(cd, s22);\n"
        "        int32[] da = { 1, 2, 3, 4 };\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s22);\n"
        "        int32[] db = { 10, 20, 30, 40 };\n"
        "        Tensor<int32> b = Tensor.of<int32>(db, s22);\n"
        "        Tensor<int32> w = Tensor.where<int32,int32,int32>(cond, a, b);\n"  // 1,20,3,40
        "        if (w.get2(0,0)!=1 || w.get2(0,1)!=20 || w.get2(1,0)!=3 || w.get2(1,1)!=40) { return -1; }\n"
        // mixed dtype → float64, cond broadcasts (2,1) → (2,2)
        "        boolean[] cc = heap boolean[2]; cc[0] = true; cc[1] = false;\n"
        "        int64[] s21 = heap int64[2]; s21[0] = 2; s21[1] = 1;\n"
        "        Tensor<boolean> cond2 = Tensor.of<boolean>(cc, s21);\n"
        "        float32[] fb = { 0.5f, 0.5f, 0.5f, 0.5f };\n"
        "        Tensor<float32> bf = Tensor.of<float32>(fb, s22);\n"
        "        Tensor<float64> w2 = Tensor.where<int32,float32,float64>(cond2, a, bf);\n"
        // row0 cond=true → a [1,2]; row1 cond=false → bf [0.5,0.5]
        "        if (w2.get2(0,0)!=1.0 || w2.get2(0,1)!=2.0 || w2.get2(1,0)!=0.5 || w2.get2(1,1)!=0.5) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 6.1.2 — clip(t, lo, hi) clamps elementwise, keeping t's dtype (spec §6 clip).
TEST(NumpyOpsTests, clipMatchesNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        int32[] da = { -5, 3, 20, 8 };\n"
        "        Tensor<int32> t = Tensor.of<int32>(da, s22);\n"
        "        Tensor<int32> c = Tensor.clip<int32>(t, 0, 10);\n"        // 0,3,10,8
        "        if (c.get2(0,0)!=0 || c.get2(0,1)!=3 || c.get2(1,0)!=10 || c.get2(1,1)!=8) { return -1; }\n"
        "        float32[] fa = { -1.5f, 0.5f, 2.5f, 1.0f };\n"
        "        Tensor<float32> tf = Tensor.of<float32>(fa, s22);\n"
        "        Tensor<float32> cf = Tensor.clip<float32>(tf, 0.0f, 1.0f);\n"  // 0,0.5,1,1
        "        if (cf.get2(0,0)!=0.0f || cf.get2(0,1)!=0.5f || cf.get2(1,0)!=1.0f || cf.get2(1,1)!=1.0f) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

const char* PRE7 =
    "package test;\n"
    "import cajeta.math.Tensor;\n"
    "import cajeta.lang.Numeric;\n"
    "import cajeta.lang.Floating;\n";

// 7.1.1 — auto-promote add (no explicit R): mixed int32/float32 returns a bounded
// wildcard Tensor<? extends Numeric> whose runtime dtype is promote(int32,float32)=
// float64, capturable as (Tensor<float64>) r (spec 5.2.1).
TEST(NumpyOpsTests, autoPromoteAddMixed) {
    std::string src = std::string(PRE7) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        int32[] da = { 1, 2, 3, 4 };\n"
        "        Tensor<int32> ai = Tensor.of<int32>(da, s22);\n"
        "        float32[] fb = { 0.5f, 0.5f, 0.5f, 0.5f };\n"
        "        Tensor<float32> bf = Tensor.of<float32>(fb, s22);\n"
        "        Tensor<? extends Numeric> r = Tensor.add(ai, bf);\n"      // inference → auto-promote
        "        if (!(r instanceof Tensor<float64>)) { return -1; }\n"    // runtime dtype float64
        "        if (r instanceof Tensor<float32>) { return -2; }\n"
        "        Tensor<float64> rc = (Tensor<float64>) r;\n"
        "        if (rc.get2(0,0)!=1.5 || rc.get2(0,1)!=2.5 || rc.get2(1,0)!=3.5 || rc.get2(1,1)!=4.5) { return -3; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 7.1.2 — auto-promote div is always floating: int/int returns Tensor<? extends
// Floating> with runtime dtype float64 (spec 5.2.2).
TEST(NumpyOpsTests, autoPromoteDivIsFloating) {
    std::string src = std::string(PRE7) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        int32[] da = { 6, 7, 8, 9 };\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s22);\n"
        "        int32[] db = { 4, 2, 8, 3 };\n"
        "        Tensor<int32> b = Tensor.of<int32>(db, s22);\n"
        "        Tensor<? extends Floating> r = Tensor.div(a, b);\n"       // int/int → floating
        "        if (!(r instanceof Tensor<float64>)) { return -1; }\n"
        "        Tensor<float64> rc = (Tensor<float64>) r;\n"
        "        if (rc.get2(0,0)!=1.5 || rc.get2(0,1)!=3.5 || rc.get2(1,0)!=1.0 || rc.get2(1,1)!=3.0) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 7.1.3 — the bounded-wildcard result discriminates dtype cleanly via the reified
// airlock: a wrong-dtype instanceof is false (no UB), only the true dtype matches
// (spec 5.2.4).
TEST(NumpyOpsTests, autoPromoteCaptureMismatchClean) {
    std::string src = std::string(PRE7) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        int32[] da = { 1, 2, 3, 4 };\n"
        "        Tensor<int32> ai = Tensor.of<int32>(da, s22);\n"
        "        float32[] fb = { 0.5f, 0.5f, 0.5f, 0.5f };\n"
        "        Tensor<float32> bf = Tensor.of<float32>(fb, s22);\n"
        "        Tensor<? extends Numeric> r = Tensor.add(ai, bf);\n"      // float64
        "        if (r instanceof Tensor<int32>) { return -1; }\n"        // wrong dtype rejected
        "        if (r instanceof Tensor<float32>) { return -2; }\n"      // wrong dtype rejected
        "        if (!(r instanceof Tensor<float64>)) { return -3; }\n"   // true dtype matched
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 8.1.1 — weak scalar of same-or-lower category adopts the tensor's dtype (NEP-50,
// spec 7.2.1/7.2.2): int8 + 5 → Tensor<int8> (value irrelevant, no widening); float32 +
// 1.0 → Tensor<float32> (not float64). Static result types confirm no widening.
TEST(NumpyOpsTests, scalarSameOrLowerCategory) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        int8[] d8 = heap int8[4];\n"
        "        d8[0] = (int8) 10; d8[1] = (int8) 20; d8[2] = (int8) 30; d8[3] = (int8) 40;\n"
        "        Tensor<int8> t8 = Tensor.of<int8>(d8, s22);\n"
        "        Tensor<int8> r8 = Tensor.addScalar<int8>(t8, (int8) 5);\n"   // 15,25,35,45 (int8)
        "        if (r8.get2(0,0) != (int8) 15 || r8.get2(1,1) != (int8) 45) { return -1; }\n"
        "        float32[] fa = { 1.5f, 2.5f, 3.5f, 4.5f };\n"
        "        Tensor<float32> tf = Tensor.of<float32>(fa, s22);\n"
        "        Tensor<float32> rf = Tensor.addScalar<float32>(tf, 1.0f);\n" // 2.5,3.5,4.5,5.5 (float32)
        "        if (rf.get2(0,0) != 2.5f || rf.get2(1,1) != 5.5f) { return -2; }\n"
        "        Tensor<float32> mf = Tensor.mulScalar<float32>(tf, 2.0f);\n" // 3,5,7,9
        "        if (mf.get2(0,0) != 3.0f || mf.get2(1,1) != 9.0f) { return -3; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 8.1.2 — a weak float scalar over an int tensor bumps to the default float (float64):
// int32 + 2.0 → Tensor<float64> (NEP-50 category bump, spec 7.2.3). Static Tensor<float64>
// result type confirms the bump.
TEST(NumpyOpsTests, scalarCategoryBump) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        int32[] da = { 1, 2, 3, 4 };\n"
        "        Tensor<int32> t = Tensor.of<int32>(da, s22);\n"
        "        Tensor<float64> r = Tensor.addScalarF<int32>(t, 2.0);\n"     // 3.0,4.0,5.0,6.0
        "        if (r.get2(0,0) != 3.0 || r.get2(0,1) != 4.0 || r.get2(1,0) != 5.0 || r.get2(1,1) != 6.0) { return -1; }\n"
        "        Tensor<float64> m = Tensor.mulScalarF<int32>(t, 0.5);\n"     // 0.5,1.0,1.5,2.0
        "        if (m.get2(0,0) != 0.5 || m.get2(1,1) != 2.0) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 3c — elementwiseCpuGpuAgree: the stdlib elementwise lowering (cajeta.math.Ewise)
// routes on placement — both operands on-device → the float32 GPU @Kernel; host
// operands → the CPU loop — and the two paths agree (the cross-check). Proven over
// the full arithmetic family (add/sub/mul/div) against a host oracle. Values chosen
// so every op is exact in float32, so agreement is bit-exact. Runs on the cajeta.gpu
// CPU backend in-process (no GPU required); on-device validation rides device gates.
TEST(NumpyOpsTests, elementwiseCpuGpuAgree) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.math.Ewise;\n"
        "public final class D {\n"
        // fresh equal-shaped float32 operands (Tensor.of takes ownership of the
        // shape array, so each tensor gets its own).
        "    public static #Tensor<float32> mk(float32[] d) {\n"
        "        int64[] shp = heap int64[1]; shp[0] = 8;\n"
        "        return Tensor.of<float32>(d, shp);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        float32[] da = { 2.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f, 20.0f, 24.0f };\n"
        "        float32[] db = { 1.0f, 2.0f, 3.0f, 4.0f,  6.0f,  8.0f, 10.0f, 12.0f };\n"
        "        int32 op = 0;\n"
        "        while (op < 4) {\n"
        "            Tensor<float32> a = D.mk(da);\n"
        "            Tensor<float32> b = D.mk(db);\n"
        "            Tensor<float32> cCpu = Ewise.arithF32Op(a, b, op);\n"      // both host → CPU path
        "            Tensor<float32> a2 = D.mk(da);\n"
        "            Tensor<float32> b2 = D.mk(db);\n"
        "            a2.gpu();\n"
        "            b2.gpu();\n"
        "            Tensor<float32> cGpu = Ewise.arithF32Op(a2, b2, op);\n"    // both device → GPU path
        "            cGpu.cpu();\n"
        "            int64 i = 0;\n"
        "            while (i < 8) {\n"
        "                float32 x = da[(int32) i];\n"
        "                float32 y = db[(int32) i];\n"
        "                float32 want = x + y;\n"
        "                if (op == 1) { want = x - y; }\n"
        "                if (op == 2) { want = x * y; }\n"
        "                if (op == 3) { want = x / y; }\n"
        "                if (cCpu.get1(i) != want) { return -1 - op * 10; }\n"  // CPU matches oracle
        "                if (cGpu.get1(i) != cCpu.get1(i)) { return -2 - op * 10; }\n"  // GPU agrees with CPU
        "                i = i + 1;\n"
        "            }\n"
        "            op = op + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 4a — full-array reductions to a scalar (numpy a.sum()/prod()/min()/max()/mean()
// with no axis). Explicit accumulator dtype R (numpy dtype= kwarg); walks the full
// multi-index so it is correct for n-D tensors. Values chosen exact.
TEST(NumpyOpsTests, reductionsFullMatchNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] d1 = { 1, 2, 3, 4 };\n"
        "        int64[] sa = heap int64[1]; sa[0] = 4;\n"
        "        Tensor<int32> a = Tensor.of<int32>(d1, sa);\n"               // [1,2,3,4]
        "        if (Tensor.sum<int32, int32>(a) != 10) { return -1; }\n"
        "        if (Tensor.prod<int32, int32>(a) != 24) { return -2; }\n"
        "        if (Tensor.min<int32>(a) != 1) { return -3; }\n"
        "        if (Tensor.max<int32>(a) != 4) { return -4; }\n"
        "        if (Tensor.mean<int32, float64>(a) != 2.5) { return -5; }\n"
        "        int32[] d2 = { 1, 2, 3, 4, 5, 6 };\n"
        "        int64[] sb = heap int64[2]; sb[0] = 2; sb[1] = 3;\n"
        "        Tensor<int32> b = Tensor.of<int32>(d2, sb);\n"             // [[1,2,3],[4,5,6]]
        "        if (Tensor.sum<int32, int32>(b) != 21) { return -6; }\n"     // n-D walk
        "        if (Tensor.prod<int32, int32>(b) != 720) { return -7; }\n"
        "        if (Tensor.min<int32>(b) != 1) { return -8; }\n"
        "        if (Tensor.max<int32>(b) != 6) { return -9; }\n"
        "        if (Tensor.mean<int32, float64>(b) != 3.5) { return -10; }\n"
        "        float32[] d3 = { 1.0f, 2.0f, 3.0f, 4.0f };\n"
        "        int64[] sc = heap int64[1]; sc[0] = 4;\n"                    // own shape (not shared with a)
        "        Tensor<float32> c = Tensor.of<float32>(d3, sc);\n"
        "        if (Tensor.sum<float32, float32>(c) != 10.0f) { return -11; }\n"
        "        if (Tensor.mean<float32, float32>(c) != 2.5f) { return -12; }\n"
        "        if (Tensor.min<float32>(c) != 1.0f) { return -13; }\n"
        "        if (Tensor.max<float32>(c) != 4.0f) { return -14; }\n"
        // accumulator upcast: int8 elements summed into int64 (sum overflows int8)
        "        int8[] d4 = { 100, 100, 100 };\n"
        "        int64[] sd = heap int64[1]; sd[0] = 3;\n"
        "        Tensor<int8> e = Tensor.of<int8>(d4, sd);\n"
        "        if (Tensor.sum<int8, int64>(e) != 300) { return -15; }\n"    // 300 > int8 max
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 4a — axis reductions (numpy a.sum(axis=)/prod/min/max/mean, keepdims + negative
// axis). keepdims=false removes the axis (ndim-1); keepdims=true keeps it as size 1.
TEST(NumpyOpsTests, reductionsAxisMatchNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] d2 = { 1, 2, 3, 4, 5, 6 };\n"
        "        int64[] sb = heap int64[2]; sb[0] = 2; sb[1] = 3;\n"
        "        Tensor<int32> b = Tensor.of<int32>(d2, sb);\n"            // [[1,2,3],[4,5,6]]
        "        Tensor<int32> s0 = Tensor.sumAxis<int32, int32>(b, 0, false);\n"   // [5,7,9]
        "        if (s0.ndim() != 1 || s0.shapeAt(0) != 3) { return -1; }\n"
        "        if (s0.get1(0) != 5 || s0.get1(1) != 7 || s0.get1(2) != 9) { return -2; }\n"
        "        Tensor<int32> s1 = Tensor.sumAxis<int32, int32>(b, 1, false);\n"   // [6,15]
        "        if (s1.shapeAt(0) != 2 || s1.get1(0) != 6 || s1.get1(1) != 15) { return -3; }\n"
        "        Tensor<int32> s0k = Tensor.sumAxis<int32, int32>(b, 0, true);\n"   // (1,3)
        "        if (s0k.ndim() != 2 || s0k.shapeAt(0) != 1 || s0k.shapeAt(1) != 3) { return -4; }\n"
        "        if (s0k.get2(0, 0) != 5 || s0k.get2(0, 2) != 9) { return -5; }\n"
        "        Tensor<int32> s1k = Tensor.sumAxis<int32, int32>(b, 1, true);\n"   // (2,1)
        "        if (s1k.shapeAt(0) != 2 || s1k.shapeAt(1) != 1) { return -6; }\n"
        "        if (s1k.get2(0, 0) != 6 || s1k.get2(1, 0) != 15) { return -7; }\n"
        "        Tensor<int32> neg = Tensor.sumAxis<int32, int32>(b, -1, false);\n" // axis -1 == 1
        "        if (neg.get1(0) != 6 || neg.get1(1) != 15) { return -8; }\n"
        "        Tensor<int32> p0 = Tensor.prodAxis<int32, int32>(b, 0, false);\n"  // [4,10,18]
        "        if (p0.get1(0) != 4 || p0.get1(1) != 10 || p0.get1(2) != 18) { return -9; }\n"
        "        Tensor<int32> p1 = Tensor.prodAxis<int32, int32>(b, 1, false);\n"  // [6,120]
        "        if (p1.get1(0) != 6 || p1.get1(1) != 120) { return -10; }\n"
        "        Tensor<int32> mn0 = Tensor.minAxis<int32>(b, 0, false);\n"         // [1,2,3]
        "        if (mn0.get1(0) != 1 || mn0.get1(1) != 2 || mn0.get1(2) != 3) { return -11; }\n"
        "        Tensor<int32> mx1 = Tensor.maxAxis<int32>(b, 1, false);\n"         // [3,6]
        "        if (mx1.get1(0) != 3 || mx1.get1(1) != 6) { return -12; }\n"
        "        Tensor<float64> me0 = Tensor.meanAxis<int32, float64>(b, 0, false);\n" // [2.5,3.5,4.5]
        "        if (me0.get1(0) != 2.5 || me0.get1(1) != 3.5 || me0.get1(2) != 4.5) { return -13; }\n"
        "        Tensor<float64> me1 = Tensor.meanAxis<int32, float64>(b, 1, false);\n" // [2.0,5.0]
        "        if (me1.get1(0) != 2.0 || me1.get1(1) != 5.0) { return -14; }\n"
        // 3-D middle-axis reduction exercises the skip-axis C-order walk
        "        int32[] d3 = { 0, 1, 2, 3, 4, 5, 6, 7 };\n"
        "        int64[] sc = heap int64[3]; sc[0] = 2; sc[1] = 2; sc[2] = 2;\n"
        "        Tensor<int32> c = Tensor.of<int32>(d3, sc);\n"
        "        Tensor<int32> cm = Tensor.sumAxis<int32, int32>(c, 1, false);\n"   // (2,2) [[2,4],[10,12]]
        "        if (cm.ndim() != 2 || cm.shapeAt(0) != 2 || cm.shapeAt(1) != 2) { return -15; }\n"
        "        if (cm.get2(0, 0) != 2 || cm.get2(0, 1) != 4 || cm.get2(1, 0) != 10 || cm.get2(1, 1) != 12) { return -16; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 4a — index/predicate reductions: argmin/argmax (C-order flat index, first on ties),
// count_nonzero, any/all (numeric truthiness), and boolean-mask anyTrue/allTrue.
TEST(NumpyOpsTests, argAnyAllCountMatchNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] da = { 3, 1, 4, 1, 5, 9, 2, 6 };\n"
        "        int64[] sa = heap int64[1]; sa[0] = 8;\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, sa);\n"
        "        if (Tensor.argmin<int32>(a) != 1) { return -1; }\n"          // first min(1) at idx 1
        "        if (Tensor.argmax<int32>(a) != 5) { return -2; }\n"          // max(9) at idx 5
        "        if (Tensor.countNonzero<int32>(a) != 8) { return -3; }\n"
        "        if (!Tensor.any<int32>(a)) { return -4; }\n"
        "        if (!Tensor.all<int32>(a)) { return -5; }\n"
        "        int32[] dz = { 0, 3, 0, 2 };\n"
        "        int64[] sz = heap int64[1]; sz[0] = 4;\n"
        "        Tensor<int32> z = Tensor.of<int32>(dz, sz);\n"
        "        if (Tensor.countNonzero<int32>(z) != 2) { return -6; }\n"
        "        if (!Tensor.any<int32>(z)) { return -7; }\n"
        "        if (Tensor.all<int32>(z)) { return -8; }\n"                  // has a zero
        "        if (Tensor.argmin<int32>(z) != 0) { return -9; }\n"         // min(0) at idx 0
        "        int32[] d0 = { 0, 0, 0 };\n"
        "        int64[] s0 = heap int64[1]; s0[0] = 3;\n"
        "        Tensor<int32> zero = Tensor.of<int32>(d0, s0);\n"
        "        if (Tensor.any<int32>(zero)) { return -10; }\n"
        "        if (Tensor.all<int32>(zero)) { return -11; }\n"
        "        if (Tensor.countNonzero<int32>(zero) != 0) { return -12; }\n"
        // 2-D argmin/argmax return the flattened C-order index
        "        int32[] dm = { 5, 2, 8, 1 };\n"
        "        int64[] sm = heap int64[2]; sm[0] = 2; sm[1] = 2;\n"
        "        Tensor<int32> m = Tensor.of<int32>(dm, sm);\n"             // [[5,2],[8,1]]
        "        if (Tensor.argmin<int32>(m) != 3) { return -13; }\n"        // 1 at flat idx 3
        "        if (Tensor.argmax<int32>(m) != 2) { return -14; }\n"        // 8 at flat idx 2
        // boolean masks
        "        boolean[] bm1 = { true, false, true };\n"
        "        int64[] sb1 = heap int64[1]; sb1[0] = 3;\n"
        "        Tensor<boolean> m1 = Tensor.of<boolean>(bm1, sb1);\n"
        "        if (!Tensor.anyTrue(m1)) { return -15; }\n"
        "        if (Tensor.allTrue(m1)) { return -16; }\n"                  // has a false
        "        boolean[] bm2 = { true, true, true };\n"
        "        int64[] sb2 = heap int64[1]; sb2[0] = 3;\n"
        "        Tensor<boolean> m2 = Tensor.of<boolean>(bm2, sb2);\n"
        "        if (!Tensor.allTrue(m2)) { return -17; }\n"
        "        boolean[] bm3 = { false, false };\n"
        "        int64[] sb3 = heap int64[1]; sb3[0] = 2;\n"
        "        Tensor<boolean> m3 = Tensor.of<boolean>(bm3, sb3);\n"
        "        if (Tensor.anyTrue(m3)) { return -18; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 4a — var/std (with ddof) and nan-variants. Textbook set [2,4,4,4,5,5,7,9]:
// mean 5, Σ(x-μ)²=32, var(ddof=0)=4, std=2 (both exact in float32). nansum/nanmean
// skip NaN; a plain sum over NaN data is NaN.
TEST(NumpyOpsTests, varStdNanMatchNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float32[] dx = { 2.0f, 4.0f, 4.0f, 4.0f, 5.0f, 5.0f, 7.0f, 9.0f };\n"
        "        int64[] sx = heap int64[1]; sx[0] = 8;\n"
        "        Tensor<float32> x = Tensor.of<float32>(dx, sx);\n"
        "        if (Tensor.variance<float32, float32>(x, 0) != 4.0f) { return -1; }\n"   // population
        "        if (Tensor.std<float32, float32>(x, 0) != 2.0f) { return -2; }\n"
        // sample variance ddof=1 = 32/7 ≈ 4.5714286 (tolerance compare)
        "        float32 v1 = Tensor.variance<float32, float32>(x, 1);\n"
        "        float32 dv = v1 - 4.5714285f; if (dv < 0.0f) { dv = 0.0f - dv; }\n"
        "        if (dv > 0.001f) { return -3; }\n"
        // nan handling
        "        float32[] dn = { 1.0f, 2.0f, 3.0f };\n"
        "        int64[] sn = heap int64[1]; sn[0] = 3;\n"
        "        Tensor<float32> t = Tensor.of<float32>(dn, sn);\n"
        "        float32 zz = 0.0f; float32 nan = zz / zz;\n"                       // NaN
        "        t.set1(1, nan);\n"                                                  // [1, NaN, 3]
        "        if (Tensor.nansum<float32, float32>(t) != 4.0f) { return -4; }\n"   // 1+3
        "        if (Tensor.nanmean<float32, float32>(t) != 2.0f) { return -5; }\n"  // 4/2
        "        float32 rs = Tensor.sum<float32, float32>(t);\n"
        "        if (rs == rs) { return -6; }\n"                                     // plain sum is NaN
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 4b — scans: cumsum/cumprod flattened (numpy cumsum(a)) and along an axis
// (cumsum(a, axis=)). Flattened forms return 1-D; axis forms keep the input shape.
TEST(NumpyOpsTests, scansMatchNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] d1 = { 1, 2, 3, 4 };\n"
        "        int64[] s4 = heap int64[1]; s4[0] = 4;\n"
        "        Tensor<int32> a = Tensor.of<int32>(d1, s4);\n"
        "        Tensor<int32> cs = Tensor.cumsum<int32, int32>(a);\n"        // [1,3,6,10]
        "        if (cs.ndim() != 1 || cs.size() != 4) { return -1; }\n"
        "        if (cs.get1(0) != 1 || cs.get1(1) != 3 || cs.get1(2) != 6 || cs.get1(3) != 10) { return -2; }\n"
        "        Tensor<int32> cp = Tensor.cumprod<int32, int32>(a);\n"       // [1,2,6,24]
        "        if (cp.get1(0) != 1 || cp.get1(1) != 2 || cp.get1(2) != 6 || cp.get1(3) != 24) { return -3; }\n"
        // 2-D flatten cumsum
        "        int32[] d2 = { 1, 2, 3, 4 };\n"
        "        int64[] sm = heap int64[2]; sm[0] = 2; sm[1] = 2;\n"
        "        Tensor<int32> m = Tensor.of<int32>(d2, sm);\n"             // [[1,2],[3,4]]
        "        Tensor<int32> fcs = Tensor.cumsum<int32, int32>(m);\n"       // flat [1,3,6,10]
        "        if (fcs.ndim() != 1 || fcs.get1(3) != 10) { return -4; }\n"
        // axis cumsum
        "        int32[] d3 = { 1, 2, 3, 4 };\n"
        "        int64[] sm2 = heap int64[2]; sm2[0] = 2; sm2[1] = 2;\n"
        "        Tensor<int32> m2 = Tensor.of<int32>(d3, sm2);\n"           // [[1,2],[3,4]]
        "        Tensor<int32> a0 = Tensor.cumsumAxis<int32, int32>(m2, 0);\n" // [[1,2],[4,6]]
        "        if (a0.ndim() != 2 || a0.shapeAt(0) != 2 || a0.shapeAt(1) != 2) { return -5; }\n"
        "        if (a0.get2(0, 0) != 1 || a0.get2(0, 1) != 2 || a0.get2(1, 0) != 4 || a0.get2(1, 1) != 6) { return -6; }\n"
        "        int32[] d4 = { 1, 2, 3, 4 };\n"
        "        int64[] sm3 = heap int64[2]; sm3[0] = 2; sm3[1] = 2;\n"
        "        Tensor<int32> m3 = Tensor.of<int32>(d4, sm3);\n"
        "        Tensor<int32> a1 = Tensor.cumsumAxis<int32, int32>(m3, 1);\n" // [[1,3],[3,7]]
        "        if (a1.get2(0, 0) != 1 || a1.get2(0, 1) != 3 || a1.get2(1, 0) != 3 || a1.get2(1, 1) != 7) { return -7; }\n"
        // axis cumprod
        "        int32[] d5 = { 1, 2, 3, 4 };\n"
        "        int64[] sm4 = heap int64[2]; sm4[0] = 2; sm4[1] = 2;\n"
        "        Tensor<int32> m4 = Tensor.of<int32>(d5, sm4);\n"
        "        Tensor<int32> p0 = Tensor.cumprodAxis<int32, int32>(m4, 0);\n" // [[1,2],[3,8]]
        "        if (p0.get2(0, 0) != 1 || p0.get2(0, 1) != 2 || p0.get2(1, 0) != 3 || p0.get2(1, 1) != 8) { return -8; }\n"
        "        int32[] d6 = { 1, 2, 3, 4 };\n"
        "        int64[] sm5 = heap int64[2]; sm5[0] = 2; sm5[1] = 2;\n"
        "        Tensor<int32> m5 = Tensor.of<int32>(d6, sm5);\n"
        "        Tensor<int32> p1 = Tensor.cumprodAxis<int32, int32>(m5, 1);\n" // [[1,2],[3,12]]
        "        if (p1.get2(0, 0) != 1 || p1.get2(0, 1) != 2 || p1.get2(1, 0) != 3 || p1.get2(1, 1) != 12) { return -9; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 4c — reductionsCpuGpuAgree: the stdlib GPU reduction (Ewise.sumF32, atomic
// parallel sum) routes on placement — on-device → atomicAdd reduction; host → CPU
// loop — and the two agree, and agree with the generic CPU Tensor.sum. Integer-
// valued floats (sum 36 < 2^24) → exact regardless of atomic order. cajeta.gpu CPU
// backend in-process (no GPU required).
TEST(NumpyOpsTests, reductionsCpuGpuAgree) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.math.Ewise;\n"
        "public final class D {\n"
        "    public static #Tensor<float32> mk() {\n"
        "        float32[] d = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f };\n"
        "        int64[] s = heap int64[1]; s[0] = 8;\n"
        "        return Tensor.of<float32>(d, s);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Tensor<float32> a = D.mk();\n"
        "        float32 cpu = Ewise.sumF32(a);\n"             // host path
        "        if (cpu != 36.0f) { return -1; }\n"
        "        Tensor<float32> b = D.mk();\n"
        "        b.gpu();\n"
        "        float32 gpu = Ewise.sumF32(b);\n"             // device path (atomic)
        "        if (gpu != cpu) { return -2; }\n"             // GPU agrees with CPU
        "        Tensor<float32> c = D.mk();\n"
        "        if (Tensor.sum<float32, float32>(c) != gpu) { return -3; }\n"  // and with generic reduction
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 5a — reshape-family view ops: ravel/flatten (1-D), swapaxes/transposeAxes/moveaxis
// (axis permutation views), flipAll (reverse every axis). Views share storage; values
// verified through get1/get2, and moveaxis through a contiguous copy.
TEST(NumpyOpsTests, shapeViewOpsMatchNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] d = { 1, 2, 3, 4, 5, 6 };\n"
        "        int64[] s23 = heap int64[2]; s23[0] = 2; s23[1] = 3;\n"
        "        Tensor<int32> a = Tensor.of<int32>(d, s23);\n"            // [[1,2,3],[4,5,6]]
        "        Tensor<int32> r = a.ravel();\n"
        "        if (r.ndim() != 1 || r.size() != 6) { return -1; }\n"
        "        if (r.get1(0) != 1 || r.get1(5) != 6) { return -2; }\n"
        "        Tensor<int32> f = a.flatten();\n"
        "        if (f.ndim() != 1 || f.get1(3) != 4) { return -3; }\n"
        "        Tensor<int32> sw = a.swapaxes(0, 1);\n"                   // (3,2) [[1,4],[2,5],[3,6]]
        "        if (sw.shapeAt(0) != 3 || sw.shapeAt(1) != 2) { return -4; }\n"
        "        if (sw.get2(0, 0) != 1 || sw.get2(0, 1) != 4 || sw.get2(2, 1) != 6 || sw.get2(1, 0) != 2) { return -5; }\n"
        "        int32[] perm = { 1, 0 };\n"
        "        Tensor<int32> ta = a.transposeAxes(perm);\n"             // same as swapaxes(0,1)
        "        if (ta.shapeAt(0) != 3 || ta.get2(0, 1) != 4 || ta.get2(2, 0) != 3) { return -6; }\n"
        "        Tensor<int32> mv = a.moveaxis(0, 1);\n"                   // 2-D move == swap → (3,2)
        "        if (mv.shapeAt(0) != 3 || mv.shapeAt(1) != 2 || mv.get2(0, 1) != 4) { return -7; }\n"
        "        Tensor<int32> fa = a.flipAll();\n"                        // reverse both axes
        "        if (fa.get2(0, 0) != 6 || fa.get2(1, 2) != 1 || fa.get2(0, 2) != 4) { return -8; }\n"
        // 3-D moveaxis(0,2): (1,2,3) -> (2,3,1); verify shape + values via a contiguous copy
        "        int32[] d3 = { 1, 2, 3, 4, 5, 6 };\n"
        "        int64[] s123 = heap int64[3]; s123[0] = 1; s123[1] = 2; s123[2] = 3;\n"
        "        Tensor<int32> b = Tensor.of<int32>(d3, s123);\n"         // [[[1,2,3],[4,5,6]]]
        "        Tensor<int32> bm = b.moveaxis(0, 2);\n"                  // (2,3,1)
        "        if (bm.ndim() != 3 || bm.shapeAt(0) != 2 || bm.shapeAt(1) != 3 || bm.shapeAt(2) != 1) { return -9; }\n"
        "        Tensor<int32> bc = bm.copy();\n"                          // contiguous C-order: out[i][j][0]=b[0][i][j]
        "        if (bc.flatGet(0) != 1 || bc.flatGet(1) != 2 || bc.flatGet(2) != 3) { return -10; }\n"
        "        if (bc.flatGet(3) != 4 || bc.flatGet(5) != 6) { return -11; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 5a — join/split: concatenate (existing axis), stack (new axis), split (equal parts).
TEST(NumpyOpsTests, joinSplitMatchNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static #Tensor<int32> mk2x2(int32 base) {\n"
        "        int32[] d = heap int32[4];\n"
        "        d[0] = base; d[1] = base + 1; d[2] = base + 2; d[3] = base + 3;\n"
        "        int64[] s = heap int64[2]; s[0] = 2; s[1] = 2;\n"
        "        return Tensor.of<int32>(d, s);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Tensor<int32>[] ps = heap Tensor<int32>[2];\n"
        "        ps[0] = D.mk2x2(1);\n"   // [[1,2],[3,4]]
        "        ps[1] = D.mk2x2(5);\n"   // [[5,6],[7,8]]
        "        Tensor<int32> c0 = Tensor.concatenate<int32>(ps, 0);\n"   // (4,2) [[1,2],[3,4],[5,6],[7,8]]
        "        if (c0.shapeAt(0) != 4 || c0.shapeAt(1) != 2) { return -1; }\n"
        "        if (c0.get2(0, 0) != 1 || c0.get2(2, 0) != 5 || c0.get2(3, 1) != 8) { return -2; }\n"
        "        Tensor<int32>[] ps2 = heap Tensor<int32>[2];\n"
        "        ps2[0] = D.mk2x2(1); ps2[1] = D.mk2x2(5);\n"
        "        Tensor<int32> c1 = Tensor.concatenate<int32>(ps2, 1);\n"  // (2,4) [[1,2,5,6],[3,4,7,8]]
        "        if (c1.shapeAt(0) != 2 || c1.shapeAt(1) != 4) { return -3; }\n"
        "        if (c1.get2(0, 2) != 5 || c1.get2(1, 3) != 8 || c1.get2(0, 1) != 2) { return -4; }\n"
        "        Tensor<int32>[] ps3 = heap Tensor<int32>[2];\n"
        "        ps3[0] = D.mk2x2(1); ps3[1] = D.mk2x2(5);\n"
        "        Tensor<int32> st = Tensor.stackTensors<int32>(ps3, 0);\n"        // (2,2,2)
        "        if (st.ndim() != 3 || st.shapeAt(0) != 2 || st.shapeAt(1) != 2 || st.shapeAt(2) != 2) { return -5; }\n"
        "        Tensor<int32> stc = st.copy();\n"                          // C-order [1,2,3,4,5,6,7,8]
        "        if (stc.flatGet(0) != 1 || stc.flatGet(3) != 4 || stc.flatGet(4) != 5 || stc.flatGet(7) != 8) { return -6; }\n"
        // split c0 (4,2) into 2 parts along axis 0 → two (2,2)
        "        Tensor<int32>[] sp = Tensor.split<int32>(c0, 2, 0);\n"
        "        Tensor<int32> s0 = sp[0];\n"
        "        Tensor<int32> s1 = sp[1];\n"
        "        if (s0.shapeAt(0) != 2 || s0.shapeAt(1) != 2) { return -7; }\n"
        "        if (s0.get2(0, 0) != 1 || s0.get2(1, 1) != 4) { return -8; }\n"
        "        if (s1.get2(0, 0) != 5 || s1.get2(1, 1) != 8) { return -9; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 5a — replication/shift/pad: tile (per-axis reps), repeat (each elem n× along axis),
// pad (constant), roll (cyclic shift). All produce fresh copies.
TEST(NumpyOpsTests, replicateShiftPadMatchNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] d = { 1, 2, 3 };\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<int32> a = Tensor.of<int32>(d, s3);\n"           // [1,2,3]
        // tile by 2 → [1,2,3,1,2,3]
        "        int64[] reps = heap int64[1]; reps[0] = 2;\n"
        "        Tensor<int32> ti = Tensor.tile<int32>(a, reps);\n"
        "        if (ti.size() != 6 || ti.get1(0) != 1 || ti.get1(3) != 1 || ti.get1(5) != 3) { return -1; }\n"
        // repeat each elem 2× → [1,1,2,2,3,3]
        "        Tensor<int32> rp = Tensor.repeat<int32>(a, 2, 0);\n"
        "        if (rp.size() != 6 || rp.get1(0) != 1 || rp.get1(1) != 1 || rp.get1(2) != 2 || rp.get1(5) != 3) { return -2; }\n"
        // pad (1 before, 2 after) value 0 → [0,1,2,3,0,0]
        "        int64[] bef = heap int64[1]; bef[0] = 1;\n"
        "        int64[] aft = heap int64[1]; aft[0] = 2;\n"
        "        Tensor<int32> pd = Tensor.pad<int32>(a, bef, aft, 0);\n"
        "        if (pd.size() != 6 || pd.get1(0) != 0 || pd.get1(1) != 1 || pd.get1(3) != 3 || pd.get1(4) != 0 || pd.get1(5) != 0) { return -3; }\n"
        // roll by 1 → [3,1,2]
        "        Tensor<int32> rl = Tensor.roll<int32>(a, 1, 0);\n"
        "        if (rl.get1(0) != 3 || rl.get1(1) != 1 || rl.get1(2) != 2) { return -4; }\n"
        // roll by -1 → [2,3,1]
        "        Tensor<int32> rln = Tensor.roll<int32>(a, -1, 0);\n"
        "        if (rln.get1(0) != 2 || rln.get1(1) != 3 || rln.get1(2) != 1) { return -5; }\n"
        // 2-D tile: [[1,2],[3,4]] tiled (2,1) → (4,2)
        "        int32[] d4 = { 1, 2, 3, 4 };\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<int32> m = Tensor.of<int32>(d4, s22);\n"
        "        int64[] reps2 = heap int64[2]; reps2[0] = 2; reps2[1] = 1;\n"
        "        Tensor<int32> tm = Tensor.tile<int32>(m, reps2);\n"      // (4,2)
        "        if (tm.shapeAt(0) != 4 || tm.shapeAt(1) != 2) { return -6; }\n"
        "        if (tm.get2(0, 0) != 1 || tm.get2(2, 0) != 1 || tm.get2(3, 1) != 4) { return -7; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 5a — matrix extraction: diagonal (k-th diagonal → 1-D), tril/triu (lower/upper
// triangle with the rest zeroed).
TEST(NumpyOpsTests, diagTriMatchNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] d = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };\n"
        "        int64[] s33 = heap int64[2]; s33[0] = 3; s33[1] = 3;\n"
        "        Tensor<int32> a = Tensor.of<int32>(d, s33);\n"          // [[1,2,3],[4,5,6],[7,8,9]]
        "        Tensor<int32> dg = Tensor.diagonal<int32>(a, 0);\n"      // [1,5,9]
        "        if (dg.size() != 3 || dg.get1(0) != 1 || dg.get1(1) != 5 || dg.get1(2) != 9) { return -1; }\n"
        "        Tensor<int32> dgu = Tensor.diagonal<int32>(a, 1);\n"     // [2,6]
        "        if (dgu.size() != 2 || dgu.get1(0) != 2 || dgu.get1(1) != 6) { return -2; }\n"
        "        Tensor<int32> dgl = Tensor.diagonal<int32>(a, -1);\n"    // [4,8]
        "        if (dgl.size() != 2 || dgl.get1(0) != 4 || dgl.get1(1) != 8) { return -3; }\n"
        "        Tensor<int32> lo = Tensor.tril<int32>(a, 0);\n"          // [[1,0,0],[4,5,0],[7,8,9]]
        "        if (lo.get2(0, 0) != 1 || lo.get2(0, 1) != 0 || lo.get2(0, 2) != 0) { return -4; }\n"
        "        if (lo.get2(1, 0) != 4 || lo.get2(1, 1) != 5 || lo.get2(1, 2) != 0) { return -5; }\n"
        "        if (lo.get2(2, 2) != 9) { return -6; }\n"
        "        Tensor<int32> up = Tensor.triu<int32>(a, 0);\n"          // [[1,2,3],[0,5,6],[0,0,9]]
        "        if (up.get2(0, 0) != 1 || up.get2(1, 0) != 0 || up.get2(2, 0) != 0 || up.get2(2, 1) != 0) { return -7; }\n"
        "        if (up.get2(1, 1) != 5 || up.get2(0, 2) != 3) { return -8; }\n"
        "        Tensor<int32> lo1 = Tensor.tril<int32>(a, 1);\n"         // keeps c<=r+1
        "        if (lo1.get2(0, 1) != 2 || lo1.get2(0, 2) != 0 || lo1.get2(1, 2) != 6) { return -9; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 5b — conditional/fancy selection: compress (keep axis slices where a 1-D mask is
// true) and choose (per-element pick from choice tensors by an index tensor). Plus a
// round-trip of the existing maskedSelect/maskedAssign compaction read/write.
TEST(NumpyOpsTests, compressChooseMaskMatchNumpy) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // compress rows of [[1,2],[3,4],[5,6]] by [true,false,true] → [[1,2],[5,6]]
        "        int32[] d = { 1, 2, 3, 4, 5, 6 };\n"
        "        int64[] s32 = heap int64[2]; s32[0] = 3; s32[1] = 2;\n"
        "        Tensor<int32> a = Tensor.of<int32>(d, s32);\n"
        "        boolean[] cb = { true, false, true };\n"
        "        int64[] sc = heap int64[1]; sc[0] = 3;\n"
        "        Tensor<boolean> cond = Tensor.of<boolean>(cb, sc);\n"
        "        Tensor<int32> cp = Tensor.compress<int32>(a, cond, 0);\n"   // (2,2)
        "        if (cp.shapeAt(0) != 2 || cp.shapeAt(1) != 2) { return -1; }\n"
        "        if (cp.get2(0, 0) != 1 || cp.get2(0, 1) != 2 || cp.get2(1, 0) != 5 || cp.get2(1, 1) != 6) { return -2; }\n"
        // choose: indices [0,1,0,1] pick between choices c0=[10,11,12,13], c1=[20,21,22,23]
        "        int64[] di = { 0, 1, 0, 1 };\n"
        "        int64[] s4 = heap int64[1]; s4[0] = 4;\n"
        "        Tensor<int64> ix = Tensor.of<int64>(di, s4);\n"
        "        int64[] dc0 = { 10, 11, 12, 13 };\n"
        "        int64[] s4a = heap int64[1]; s4a[0] = 4;\n"
        "        int64[] dc1 = { 20, 21, 22, 23 };\n"
        "        int64[] s4b = heap int64[1]; s4b[0] = 4;\n"
        "        Tensor<int64>[] ch = heap Tensor<int64>[2];\n"
        "        ch[0] = Tensor.of<int64>(dc0, s4a);\n"
        "        ch[1] = Tensor.of<int64>(dc1, s4b);\n"
        "        Tensor<int64> co = Tensor.choose<int64>(ix, ch);\n"        // [10,21,12,23]
        "        if (co.get1(0) != 10 || co.get1(1) != 21 || co.get1(2) != 12 || co.get1(3) != 23) { return -3; }\n"
        // maskedSelect/maskedAssign round-trip
        "        int32[] dm = { 1, 2, 3, 4 };\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<int32> m = Tensor.of<int32>(dm, s22);\n"
        "        boolean[] bm = { true, false, false, true };\n"
        "        int64[] s22b = heap int64[2]; s22b[0] = 2; s22b[1] = 2;\n"
        "        Tensor<boolean> mask = Tensor.of<boolean>(bm, s22b);\n"
        "        Tensor<int32> sel = m.maskedSelect(mask);\n"               // [1,4]
        "        if (sel.size() != 2 || sel.get1(0) != 1 || sel.get1(1) != 4) { return -4; }\n"
        "        m.maskedAssign(mask, 0);\n"                                 // [[0,2],[3,0]]
        "        if (m.get2(0, 0) != 0 || m.get2(0, 1) != 2 || m.get2(1, 0) != 3 || m.get2(1, 1) != 0) { return -5; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 5b/5c — gatherCpuGpuAgree: the stdlib GPU gather (Ewise.takeF32) routes on
// placement — on-device → the gatherF32 kernel (out[i]=in[idx[i]]); host → CPU
// loop — and the two agree. cajeta.gpu CPU backend in-process (no GPU required).
TEST(NumpyOpsTests, gatherCpuGpuAgree) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.math.Ewise;\n"
        "public final class D {\n"
        "    public static #Tensor<float32> data() {\n"
        "        float32[] d = { 10.0f, 20.0f, 30.0f, 40.0f };\n"
        "        int64[] s = heap int64[1]; s[0] = 4;\n"
        "        return Tensor.of<float32>(d, s);\n"
        "    }\n"
        "    public static #Tensor<int64> idx() {\n"
        "        int64[] d = { 3, 1, 0, 2 };\n"
        "        int64[] s = heap int64[1]; s[0] = 4;\n"
        "        return Tensor.of<int64>(d, s);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Tensor<float32> tc = D.data();\n"
        "        Tensor<int64> ic = D.idx();\n"
        "        Tensor<float32> cpu = Ewise.takeF32(tc, ic);\n"           // host → [40,20,10,30]
        "        if (cpu.get1(0) != 40.0f || cpu.get1(1) != 20.0f || cpu.get1(2) != 10.0f || cpu.get1(3) != 30.0f) { return -1; }\n"
        "        Tensor<float32> tg = D.data();\n"
        "        Tensor<int64> ig = D.idx();\n"
        "        tg.gpu();\n"
        "        ig.gpu();\n"
        "        Tensor<float32> gpu = Ewise.takeF32(tg, ig);\n"           // device → gather kernel
        "        gpu.cpu();\n"
        "        int64 i = 0;\n"
        "        while (i < 4) {\n"
        "            if (gpu.get1(i) != cpu.get1(i)) { return -2; }\n"     // GPU agrees with CPU
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 6a — 2-D matmul + 1-D dot (CPU GEMM floor). out[i,j] = Σ_p a[i,p]*b[p,j].
TEST(NumpyOpsTests, matmulDotMatchNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // 2x2 · 2x2
        "        int32[] da = { 1, 2, 3, 4 };\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s22);\n"          // [[1,2],[3,4]]
        "        int32[] db = { 5, 6, 7, 8 };\n"
        "        int64[] s22b = heap int64[2]; s22b[0] = 2; s22b[1] = 2;\n"
        "        Tensor<int32> b = Tensor.of<int32>(db, s22b);\n"        // [[5,6],[7,8]]
        "        Tensor<int32> c = Tensor.matmul<int32>(a, b);\n"         // [[19,22],[43,50]]
        "        if (c.shapeAt(0) != 2 || c.shapeAt(1) != 2) { return -1; }\n"
        "        if (c.get2(0, 0) != 19 || c.get2(0, 1) != 22 || c.get2(1, 0) != 43 || c.get2(1, 1) != 50) { return -2; }\n"
        // non-square (2,3)·(3,2) → (2,2)
        "        int32[] de = { 1, 2, 3, 4, 5, 6 };\n"
        "        int64[] s23 = heap int64[2]; s23[0] = 2; s23[1] = 3;\n"
        "        Tensor<int32> e = Tensor.of<int32>(de, s23);\n"         // [[1,2,3],[4,5,6]]
        "        int32[] df = { 7, 8, 9, 10, 11, 12 };\n"
        "        int64[] s32 = heap int64[2]; s32[0] = 3; s32[1] = 2;\n"
        "        Tensor<int32> f = Tensor.of<int32>(df, s32);\n"         // [[7,8],[9,10],[11,12]]
        "        Tensor<int32> g = Tensor.matmul<int32>(e, f);\n"         // [[58,64],[139,154]]
        "        if (g.shapeAt(0) != 2 || g.shapeAt(1) != 2) { return -3; }\n"
        "        if (g.get2(0, 0) != 58 || g.get2(0, 1) != 64 || g.get2(1, 0) != 139 || g.get2(1, 1) != 154) { return -4; }\n"
        // 1-D dot
        "        int32[] dv = { 1, 2, 3 };\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<int32> v = Tensor.of<int32>(dv, s3);\n"
        "        int32[] dw = { 4, 5, 6 };\n"
        "        int64[] s3b = heap int64[1]; s3b[0] = 3;\n"
        "        Tensor<int32> w = Tensor.of<int32>(dw, s3b);\n"
        "        if (Tensor.dot<int32>(v, w) != 32) { return -5; }\n"     // 4+10+18
        // float matmul exactness
        "        float32[] dp = { 1.0f, 0.0f, 0.0f, 1.0f };\n"
        "        int64[] s22c = heap int64[2]; s22c[0] = 2; s22c[1] = 2;\n"
        "        Tensor<float32> id = Tensor.of<float32>(dp, s22c);\n"   // identity
        "        float32[] dq = { 2.0f, 3.0f, 4.0f, 5.0f };\n"
        "        int64[] s22d = heap int64[2]; s22d[0] = 2; s22d[1] = 2;\n"
        "        Tensor<float32> q = Tensor.of<float32>(dq, s22d);\n"
        "        Tensor<float32> r = Tensor.matmul<float32>(id, q);\n"    // == q
        "        if (r.get2(0, 0) != 2.0f || r.get2(1, 1) != 5.0f || r.get2(0, 1) != 3.0f) { return -6; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 6a — vdot (flattened inner product → scalar) + outer (1-D × 1-D → 2-D).
TEST(NumpyOpsTests, vdotOuterMatchNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // vdot of two 2x2 → 1*5+2*6+3*7+4*8 = 70
        "        int32[] da = { 1, 2, 3, 4 };\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s22);\n"
        "        int32[] db = { 5, 6, 7, 8 };\n"
        "        int64[] s22b = heap int64[2]; s22b[0] = 2; s22b[1] = 2;\n"
        "        Tensor<int32> b = Tensor.of<int32>(db, s22b);\n"
        "        if (Tensor.vdot<int32>(a, b) != 70) { return -1; }\n"
        // outer([1,2,3],[4,5]) → (3,2) [[4,5],[8,10],[12,15]]
        "        int32[] dv = { 1, 2, 3 };\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<int32> v = Tensor.of<int32>(dv, s3);\n"
        "        int32[] dw = { 4, 5 };\n"
        "        int64[] s2 = heap int64[1]; s2[0] = 2;\n"
        "        Tensor<int32> w = Tensor.of<int32>(dw, s2);\n"
        "        Tensor<int32> o = Tensor.outer<int32>(v, w);\n"
        "        if (o.shapeAt(0) != 3 || o.shapeAt(1) != 2) { return -2; }\n"
        "        if (o.get2(0, 0) != 4 || o.get2(0, 1) != 5 || o.get2(1, 0) != 8 || o.get2(2, 1) != 15) { return -3; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 6a — trace (diagonal sum), kron (Kronecker product), matrix_power (repeated matmul).
TEST(NumpyOpsTests, traceKronPowerMatchNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] d9 = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };\n"
        "        int64[] s33 = heap int64[2]; s33[0] = 3; s33[1] = 3;\n"
        "        Tensor<int32> a = Tensor.of<int32>(d9, s33);\n"
        "        if (Tensor.trace<int32>(a, 0) != 15) { return -1; }\n"     // 1+5+9
        "        if (Tensor.trace<int32>(a, 1) != 8) { return -2; }\n"      // 2+6
        // kron [[1,2],[3,4]] ⊗ [[10,20],[30,40]] → (4,4)
        "        int32[] da = { 1, 2, 3, 4 };\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<int32> ka = Tensor.of<int32>(da, s22);\n"
        "        int32[] dbk = { 10, 20, 30, 40 };\n"
        "        int64[] s22b = heap int64[2]; s22b[0] = 2; s22b[1] = 2;\n"
        "        Tensor<int32> kb = Tensor.of<int32>(dbk, s22b);\n"
        "        Tensor<int32> kr = Tensor.kron<int32>(ka, kb);\n"
        "        if (kr.shapeAt(0) != 4 || kr.shapeAt(1) != 4) { return -3; }\n"
        "        if (kr.get2(0, 0) != 10 || kr.get2(0, 2) != 20 || kr.get2(2, 0) != 30 || kr.get2(3, 3) != 160) { return -4; }\n"
        "        if (kr.get2(1, 1) != 40 || kr.get2(0, 1) != 20) { return -5; }\n"
        // matrix_power of [[1,1],[0,1]]: ^2 → [[1,2],[0,1]], ^3 → [[1,3],[0,1]], ^0 → I
        "        int32[] dp = { 1, 1, 0, 1 };\n"
        "        int64[] s22c = heap int64[2]; s22c[0] = 2; s22c[1] = 2;\n"
        "        Tensor<int32> m = Tensor.of<int32>(dp, s22c);\n"
        "        Tensor<int32> m2 = Tensor.matrixPower<int32>(m, 2);\n"
        "        if (m2.get2(0, 1) != 2 || m2.get2(0, 0) != 1 || m2.get2(1, 0) != 0 || m2.get2(1, 1) != 1) { return -6; }\n"
        "        Tensor<int32> m3 = Tensor.matrixPower<int32>(m, 3);\n"
        "        if (m3.get2(0, 1) != 3) { return -7; }\n"
        "        Tensor<int32> m0 = Tensor.matrixPower<int32>(m, 0);\n"
        "        if (m0.get2(0, 0) != 1 || m0.get2(0, 1) != 0 || m0.get2(1, 1) != 1) { return -8; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 6a — tensordot (integer axes): contracts last `naxes` of a with first `naxes` of b.
// naxes=1 over (m,k)·(k,n) reproduces matmul; (m,k)·(k,) gives matrix-vector.
TEST(NumpyOpsTests, tensordotMatchNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // (2,3)·(3,2) naxes=1 → (2,2) == matmul → [[58,64],[139,154]]
        "        int32[] de = { 1, 2, 3, 4, 5, 6 };\n"
        "        int64[] s23 = heap int64[2]; s23[0] = 2; s23[1] = 3;\n"
        "        Tensor<int32> e = Tensor.of<int32>(de, s23);\n"
        "        int32[] df = { 7, 8, 9, 10, 11, 12 };\n"
        "        int64[] s32 = heap int64[2]; s32[0] = 3; s32[1] = 2;\n"
        "        Tensor<int32> f = Tensor.of<int32>(df, s32);\n"
        "        Tensor<int32> g = Tensor.tensordot<int32>(e, f, 1);\n"
        "        if (g.ndim() != 2 || g.shapeAt(0) != 2 || g.shapeAt(1) != 2) { return -1; }\n"
        "        if (g.get2(0, 0) != 58 || g.get2(0, 1) != 64 || g.get2(1, 0) != 139 || g.get2(1, 1) != 154) { return -2; }\n"
        // (2,3)·(3,) naxes=1 → (2,) matrix-vector; b=[1,1,1] → [6,15]
        "        int32[] dv = { 1, 1, 1 };\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<int32> v = Tensor.of<int32>(dv, s3);\n"
        "        Tensor<int32> mv = Tensor.tensordot<int32>(e, v, 1);\n"
        "        if (mv.ndim() != 1 || mv.shapeAt(0) != 2) { return -3; }\n"
        "        if (mv.get1(0) != 6 || mv.get1(1) != 15) { return -4; }\n"
        // 3-D · 2-D, naxes=1: (2,2,3)·(3,4) → (2,2,4); spot-check via copy/flatGet
        "        int32[] d12 = { 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1 };\n"   // (2,2,3)
        "        int64[] s223 = heap int64[3]; s223[0] = 2; s223[1] = 2; s223[2] = 3;\n"
        "        Tensor<int32> a3 = Tensor.of<int32>(d12, s223);\n"
        "        int32[] dB = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };\n"  // (3,4)
        "        int64[] s34 = heap int64[2]; s34[0] = 3; s34[1] = 4;\n"
        "        Tensor<int32> b34 = Tensor.of<int32>(dB, s34);\n"
        "        Tensor<int32> t3 = Tensor.tensordot<int32>(a3, b34, 1);\n"  // (2,2,4)
        "        if (t3.ndim() != 3 || t3.shapeAt(0) != 2 || t3.shapeAt(1) != 2 || t3.shapeAt(2) != 4) { return -5; }\n"
        // row [1,0,0]·b = b row0 = [1,2,3,4]; row [1,1,1]·b = col sums = [15,18,21,24]
        "        Tensor<int32> t3c = t3.copy();\n"
        "        if (t3c.flatGet(0) != 1 || t3c.flatGet(3) != 4) { return -6; }\n"          // a3[0,0]=[1,0,0]
        "        if (t3c.flatGet(12) != 15 || t3c.flatGet(15) != 24) { return -7; }\n"      // a3[1,1]=[1,1,1]
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 6d — inner: contract the LAST axis of each operand. For a (...i,k) and b (...j,k)
// (equal last axis k) → (...i,...j) with out[i,j] = Σ_k a[i,k]*b[j,k]. Two 1-D
// vectors give a 0-D scalar tensor (read via getAt with any index — ndim 0).
TEST(NumpyOpsTests, innerMatchNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // 2-D inner: (2,3) inner (2,3) → (2,2)
        "        int32[] da = { 1, 2, 3, 4, 5, 6 };\n"
        "        int64[] s23 = heap int64[2]; s23[0] = 2; s23[1] = 3;\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s23);\n"           // [[1,2,3],[4,5,6]]
        "        int32[] db = { 1, 0, 0, 0, 1, 0 };\n"
        "        int64[] s23b = heap int64[2]; s23b[0] = 2; s23b[1] = 3;\n"
        "        Tensor<int32> b = Tensor.of<int32>(db, s23b);\n"          // [[1,0,0],[0,1,0]]
        "        Tensor<int32> c = Tensor.inner<int32>(a, b);\n"           // [[1,2],[4,5]]
        "        if (c.ndim() != 2 || c.shapeAt(0) != 2 || c.shapeAt(1) != 2) { return -1; }\n"
        "        if (c.get2(0, 0) != 1 || c.get2(0, 1) != 2 || c.get2(1, 0) != 4 || c.get2(1, 1) != 5) { return -2; }\n"
        // 1-D inner → 0-D scalar: [1,2,3]·[4,5,6] = 32
        "        int32[] dv = { 1, 2, 3 };\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<int32> v = Tensor.of<int32>(dv, s3);\n"
        "        int32[] dw = { 4, 5, 6 };\n"
        "        int64[] s3b = heap int64[1]; s3b[0] = 3;\n"
        "        Tensor<int32> w = Tensor.of<int32>(dw, s3b);\n"
        "        Tensor<int32> sc = Tensor.inner<int32>(v, w);\n"
        "        if (sc.ndim() != 0 || sc.size() != 1) { return -3; }\n"
        "        int64[] dummy = heap int64[1]; dummy[0] = 0;\n"
        "        if (sc.getAt(dummy) != 32) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 6c/6f — einsum: parse the subscript spec → contraction plan → nested sum walk.
// Representative set: transpose, trace, matmul, batched matmul, diagonal, sum-all,
// row-sum, and the 1-D inner product (dot). Explicit `->` output required.
TEST(NumpyOpsTests, einsumMatchesNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64[] dummy = heap int64[2]; dummy[0] = 0; dummy[1] = 0;\n"
        // transpose "ij->ji" on (2,3) [[1,2,3],[4,5,6]] → (3,2) [[1,4],[2,5],[3,6]]
        "        int32[] da = { 1, 2, 3, 4, 5, 6 };\n"
        "        int64[] s23 = heap int64[2]; s23[0] = 2; s23[1] = 3;\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s23);\n"
        "        Tensor<int32>[] o1 = heap Tensor<int32>[1]; o1[0] = a;\n"
        "        Tensor<int32> tr = Tensor.einsum<int32>(\"ij->ji\", o1);\n"
        "        if (tr.ndim() != 2 || tr.shapeAt(0) != 3 || tr.shapeAt(1) != 2) { return -1; }\n"
        "        if (tr.get2(0, 1) != 4 || tr.get2(2, 0) != 3 || tr.get2(1, 1) != 5) { return -2; }\n"
        // trace "ii->" on (3,3) [1..9] diag 1,5,9 → 15 (0-D scalar)
        "        int32[] d9 = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };\n"
        "        int64[] s33 = heap int64[2]; s33[0] = 3; s33[1] = 3;\n"
        "        Tensor<int32> m = Tensor.of<int32>(d9, s33);\n"
        "        Tensor<int32>[] o2 = heap Tensor<int32>[1]; o2[0] = m;\n"
        "        Tensor<int32> trc = Tensor.einsum<int32>(\"ii->\", o2);\n"
        "        if (trc.ndim() != 0 || trc.getAt(dummy) != 15) { return -3; }\n"
        // diagonal "ii->i" on (3,3) → [1,5,9]
        "        Tensor<int32>[] o3 = heap Tensor<int32>[1]; o3[0] = m;\n"
        "        Tensor<int32> dg = Tensor.einsum<int32>(\"ii->i\", o3);\n"
        "        if (dg.ndim() != 1 || dg.shapeAt(0) != 3) { return -4; }\n"
        "        if (dg.get1(0) != 1 || dg.get1(1) != 5 || dg.get1(2) != 9) { return -5; }\n"
        // sum-all "ij->" on (2,3) [1..6] → 21
        "        Tensor<int32>[] o4 = heap Tensor<int32>[1]; o4[0] = a;\n"
        "        Tensor<int32> sm = Tensor.einsum<int32>(\"ij->\", o4);\n"
        "        if (sm.getAt(dummy) != 21) { return -6; }\n"
        // row-sum "ij->i" on (2,3) [[1,2,3],[4,5,6]] → [6,15]
        "        Tensor<int32>[] o5 = heap Tensor<int32>[1]; o5[0] = a;\n"
        "        Tensor<int32> rs = Tensor.einsum<int32>(\"ij->i\", o5);\n"
        "        if (rs.ndim() != 1 || rs.shapeAt(0) != 2) { return -7; }\n"
        "        if (rs.get1(0) != 6 || rs.get1(1) != 15) { return -8; }\n"
        // matmul "ij,jk->ik" (2,3)·(3,2) → [[58,64],[139,154]]
        "        int32[] de = { 1, 2, 3, 4, 5, 6 };\n"
        "        int64[] s23e = heap int64[2]; s23e[0] = 2; s23e[1] = 3;\n"
        "        Tensor<int32> e = Tensor.of<int32>(de, s23e);\n"
        "        int32[] df = { 7, 8, 9, 10, 11, 12 };\n"
        "        int64[] s32 = heap int64[2]; s32[0] = 3; s32[1] = 2;\n"
        "        Tensor<int32> f = Tensor.of<int32>(df, s32);\n"
        "        Tensor<int32>[] o6 = heap Tensor<int32>[2]; o6[0] = e; o6[1] = f;\n"
        "        Tensor<int32> mm = Tensor.einsum<int32>(\"ij,jk->ik\", o6);\n"
        "        if (mm.shapeAt(0) != 2 || mm.shapeAt(1) != 2) { return -9; }\n"
        "        if (mm.get2(0, 0) != 58 || mm.get2(0, 1) != 64 || mm.get2(1, 0) != 139 || mm.get2(1, 1) != 154) { return -10; }\n"
        // 1-D inner "i,i->" [1,2,3]·[4,5,6] → 32
        "        int32[] dv = { 1, 2, 3 };\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<int32> v = Tensor.of<int32>(dv, s3);\n"
        "        int32[] dw = { 4, 5, 6 };\n"
        "        int64[] s3b = heap int64[1]; s3b[0] = 3;\n"
        "        Tensor<int32> w = Tensor.of<int32>(dw, s3b);\n"
        "        Tensor<int32>[] o7 = heap Tensor<int32>[2]; o7[0] = v; o7[1] = w;\n"
        "        Tensor<int32> ip = Tensor.einsum<int32>(\"i,i->\", o7);\n"
        "        if (ip.getAt(dummy) != 32) { return -11; }\n"
        // batched matmul "bij,bjk->bik" (2,2,2)·(2,2,2)
        "        int32[] dba = { 1, 2, 3, 4, 1, 0, 0, 1 };\n"     // batch0 [[1,2],[3,4]], batch1 I
        "        int64[] s222 = heap int64[3]; s222[0] = 2; s222[1] = 2; s222[2] = 2;\n"
        "        Tensor<int32> ba = Tensor.of<int32>(dba, s222);\n"
        "        int32[] dbb = { 5, 6, 7, 8, 2, 3, 4, 5 };\n"     // batch0 [[5,6],[7,8]], batch1 [[2,3],[4,5]]
        "        int64[] s222b = heap int64[3]; s222b[0] = 2; s222b[1] = 2; s222b[2] = 2;\n"
        "        Tensor<int32> bb = Tensor.of<int32>(dbb, s222b);\n"
        "        Tensor<int32>[] o8 = heap Tensor<int32>[2]; o8[0] = ba; o8[1] = bb;\n"
        "        Tensor<int32> bm = Tensor.einsum<int32>(\"bij,bjk->bik\", o8);\n"
        "        if (bm.ndim() != 3 || bm.shapeAt(0) != 2 || bm.shapeAt(1) != 2 || bm.shapeAt(2) != 2) { return -12; }\n"
        "        Tensor<int32> bmc = bm.copy();\n"                 // [19,22,43,50, 2,3,4,5]
        "        if (bmc.flatGet(0) != 19 || bmc.flatGet(3) != 50) { return -13; }\n"
        "        if (bmc.flatGet(4) != 2 || bmc.flatGet(7) != 5) { return -14; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
