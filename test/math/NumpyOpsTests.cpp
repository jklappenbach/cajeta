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
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <cstdlib>
#include <vector>

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
        "        int32[] dx = [ 1, 2, 3 ];\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<int32> x = Tensor.of<int32>(dx, s3);\n"
        "        int32[] dy = [ 10, 20 ];\n"
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
        "        int32[] da = [ 1, 2, 3, 4, 5, 6 ];\n"
        "        int64[] s23 = heap int64[2];\n"
        "        s23[0] = 2; s23[1] = 3;\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s23);\n"          // [[1,2,3],[4,5,6]]
        "        int32[] db = [ 10, 20, 30, 40, 50, 60 ];\n"
        "        Tensor<int32> b = Tensor.of<int32>(db, s23);\n"
        "        Tensor<int32> sum = Tensor.add<int32>(a, b);\n"
        "        if (sum.get2(0,0) != 11 || sum.get2(1,2) != 66) { return -1; }\n"
        "        Tensor<int32> diff = Tensor.sub<int32>(b, a);\n"
        "        if (diff.get2(0,0) != 9 || diff.get2(1,2) != 54) { return -2; }\n"
        "        Tensor<int32> prod = Tensor.mul<int32>(a, b);\n"
        "        if (prod.get2(0,1) != 40 || prod.get2(1,0) != 160) { return -3; }\n"  // 2*20, 4*40
        // broadcast a (2,3) + row (3,)
        "        int32[] dr = [ 100, 200, 300 ];\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<int32> row = Tensor.of<int32>(dr, s3);\n"
        "        Tensor<int32> br = Tensor.add<int32>(a, row);\n"
        "        if (br.ndim() != 2 || br.shapeAt(0) != 2 || br.shapeAt(1) != 3) { return -4; }\n"
        "        if (br.get2(0,0) != 101 || br.get2(1,2) != 306) { return -5; }\n"
        // broadcast a (2,3) + col (2,1)
        "        int32[] dc = [ 1000, 2000 ];\n"
        "        int64[] s21 = heap int64[2]; s21[0] = 2; s21[1] = 1;\n"
        "        Tensor<int32> col = Tensor.of<int32>(dc, s21);\n"
        "        Tensor<int32> bc = Tensor.add<int32>(a, col);\n"
        "        if (bc.get2(0,0) != 1001 || bc.get2(1,0) != 2004) { return -6; }\n"
        "        if (bc.get2(0,2) != 1003 || bc.get2(1,2) != 2006) { return -7; }\n"
        // float path — the dtype-dispatch check (must emit fadd, not integer add)
        "        float32[] fa = [ 1.5f, 2.5f, 3.5f, 4.5f ];\n"
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
        "        int32[] da = [ 1, 2, 3, 4 ];\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<int32> ai = Tensor.of<int32>(da, s22);\n"          // [[1,2],[3,4]]
        "        float32[] fb = [ 0.5f, 0.5f, 0.5f, 0.5f ];\n"
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
        "        int32[] da = [ 1, 2, 3, 4 ];\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<int32> ai = Tensor.of<int32>(da, s22);\n"          // [[1,2],[3,4]]
        "        float32[] fb = [ 0.5f, 0.5f, 0.5f, 0.5f ];\n"
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
        "        int32[] da = [ 1, 2, 3, 4, 5, 6 ];\n"
        "        int64[] s23 = heap int64[2]; s23[0] = 2; s23[1] = 3;\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s23);\n"          // [[1,2,3],[4,5,6]]
        "        int32[] db = [ 1, 9, 3, 4, 0, 6 ];\n"
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
        "        int32[] dr = [ 1, 5, 6 ];\n"
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
        "        int32[] da = [ 3, 4 ];\n"
        "        int64[] s2 = heap int64[1]; s2[0] = 2;\n"
        "        Tensor<int32> ai = Tensor.of<int32>(da, s2);\n"         // [3, 4]
        "        float32[] fb = [ 3.0f, 3.5f ];\n"
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
        "        int32[] da = [ 6, 7, 8, 9 ];\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s22);\n"
        "        int32[] db = [ 4, 2, 8, 3 ];\n"
        "        Tensor<int32> b = Tensor.of<int32>(db, s22);\n"
        "        Tensor<float64> q = Tensor.div<int32,int32,float64>(a, b);\n"   // [1.5,3.5,1.0,3.0]
        "        if (q.get2(0,0) != 1.5) { return -1; }\n"
        "        if (q.get2(0,1) != 3.5) { return -2; }\n"
        "        if (q.get2(1,0) != 1.0) { return -3; }\n"
        "        if (q.get2(1,1) != 3.0) { return -4; }\n"
        "        float32[] fa = [ 1.0f, 3.0f, 7.0f, 9.0f ];\n"
        "        Tensor<float32> fA = Tensor.of<float32>(fa, s22);\n"
        "        float32[] fb = [ 2.0f, 2.0f, 2.0f, 2.0f ];\n"
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
        "        int32[] da = [ 7, -7, 7, -7 ];\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s22);\n"
        "        int32[] db = [ 2, 2, -2, -2 ];\n"
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
        "        float32[] fa = [ -7.0f, 7.5f, -7.5f, 8.0f ];\n"
        "        Tensor<float32> fA = Tensor.of<float32>(fa, s22);\n"
        "        float32[] fbv = [ 2.0f, 2.0f, 2.0f, 3.0f ];\n"
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
        "        int32[] da = [ 12, 10, 6, 5 ];\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s22);\n"
        "        int32[] db = [ 10, 6, 3, 1 ];\n"
        "        Tensor<int32> b = Tensor.of<int32>(db, s22);\n"
        "        Tensor<int32> an = Tensor.bitAnd<int32>(a, b);\n"          // 8,2,2,1
        "        if (an.get2(0,0)!=8 || an.get2(0,1)!=2 || an.get2(1,0)!=2 || an.get2(1,1)!=1) { return -1; }\n"
        "        Tensor<int32> orr = Tensor.bitOr<int32>(a, b);\n"          // 14,14,7,5
        "        if (orr.get2(0,0)!=14 || orr.get2(0,1)!=14 || orr.get2(1,0)!=7 || orr.get2(1,1)!=5) { return -2; }\n"
        "        Tensor<int32> xr = Tensor.bitXor<int32>(a, b);\n"          // 6,12,5,4
        "        if (xr.get2(0,0)!=6 || xr.get2(0,1)!=12 || xr.get2(1,0)!=5 || xr.get2(1,1)!=4) { return -3; }\n"
        "        int32[] sh = [ 1, 2, 0, 3 ];\n"
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
        "        float32[] fa = [ 1.0f, 2.0f ];\n"
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
        "        float32[] fa = [ 4.0f, 9.0f, 16.0f, 25.0f ];\n"
        "        Tensor<float32> t = Tensor.of<float32>(fa, s22);\n"
        "        Tensor<float32> sq = Tensor.sqrt<float32>(t);\n"          // 2,3,4,5
        "        if (sq.get2(0,0)!=2.0f || sq.get2(0,1)!=3.0f || sq.get2(1,0)!=4.0f || sq.get2(1,1)!=5.0f) { return -1; }\n"
        "        Tensor<float32> ng = Tensor.neg<float32>(t);\n"           // -4,-9,-16,-25
        "        if (ng.get2(0,0)!=-4.0f || ng.get2(1,1)!=-25.0f) { return -2; }\n"
        "        Tensor<float32> ab = Tensor.abs<float32>(ng);\n"          // 4,9,16,25
        "        if (ab.get2(0,0)!=4.0f || ab.get2(1,1)!=25.0f) { return -3; }\n"
        "        float32[] zd = [ 0.0f, 0.0f, 0.0f, 0.0f ];\n"
        "        Tensor<float32> z = Tensor.of<float32>(zd, s22);\n"
        "        Tensor<float32> ex = Tensor.exp<float32>(z);\n"           // 1,1,1,1
        "        if (ex.get2(0,0)!=1.0f || ex.get2(1,1)!=1.0f) { return -4; }\n"
        "        Tensor<float32> si = Tensor.sin<float32>(z);\n"           // 0,0,0,0
        "        if (si.get2(0,0)!=0.0f) { return -5; }\n"
        "        Tensor<float32> co = Tensor.cos<float32>(z);\n"           // 1,1,1,1
        "        if (co.get2(0,0)!=1.0f) { return -6; }\n"
        "        float32[] od = [ 1.0f, 1.0f, 1.0f, 1.0f ];\n"
        "        Tensor<float32> o = Tensor.of<float32>(od, s22);\n"
        "        Tensor<float32> lg = Tensor.log<float32>(o);\n"           // 0,0,0,0
        "        if (lg.get2(0,0)!=0.0f) { return -7; }\n"
        "        float32[] fr = [ 3.7f, 3.2f, -1.2f, -1.8f ];\n"
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
        "        int32[] da = [ 1, 2 ];\n"
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
        "        int32[] da = [ 1, 2, 3, 4 ];\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s22);\n"
        "        int32[] db = [ 10, 20, 30, 40 ];\n"
        "        Tensor<int32> b = Tensor.of<int32>(db, s22);\n"
        "        Tensor<int32> w = Tensor.where<int32,int32,int32>(cond, a, b);\n"  // 1,20,3,40
        "        if (w.get2(0,0)!=1 || w.get2(0,1)!=20 || w.get2(1,0)!=3 || w.get2(1,1)!=40) { return -1; }\n"
        // mixed dtype → float64, cond broadcasts (2,1) → (2,2)
        "        boolean[] cc = heap boolean[2]; cc[0] = true; cc[1] = false;\n"
        "        int64[] s21 = heap int64[2]; s21[0] = 2; s21[1] = 1;\n"
        "        Tensor<boolean> cond2 = Tensor.of<boolean>(cc, s21);\n"
        "        float32[] fb = [ 0.5f, 0.5f, 0.5f, 0.5f ];\n"
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
        "        int32[] da = [ -5, 3, 20, 8 ];\n"
        "        Tensor<int32> t = Tensor.of<int32>(da, s22);\n"
        "        Tensor<int32> c = Tensor.clip<int32>(t, 0, 10);\n"        // 0,3,10,8
        "        if (c.get2(0,0)!=0 || c.get2(0,1)!=3 || c.get2(1,0)!=10 || c.get2(1,1)!=8) { return -1; }\n"
        "        float32[] fa = [ -1.5f, 0.5f, 2.5f, 1.0f ];\n"
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
        "        int32[] da = [ 1, 2, 3, 4 ];\n"
        "        Tensor<int32> ai = Tensor.of<int32>(da, s22);\n"
        "        float32[] fb = [ 0.5f, 0.5f, 0.5f, 0.5f ];\n"
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
        "        int32[] da = [ 6, 7, 8, 9 ];\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s22);\n"
        "        int32[] db = [ 4, 2, 8, 3 ];\n"
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
        "        int32[] da = [ 1, 2, 3, 4 ];\n"
        "        Tensor<int32> ai = Tensor.of<int32>(da, s22);\n"
        "        float32[] fb = [ 0.5f, 0.5f, 0.5f, 0.5f ];\n"
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
        "        float32[] fa = [ 1.5f, 2.5f, 3.5f, 4.5f ];\n"
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
        "        int32[] da = [ 1, 2, 3, 4 ];\n"
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
// so every op is exact in float32, so agreement is bit-exact. Runs on the cajeta.xpu
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
        "        float32[] da = [ 2.0f, 4.0f, 6.0f, 8.0f, 12.0f, 16.0f, 20.0f, 24.0f ];\n"
        "        float32[] db = [ 1.0f, 2.0f, 3.0f, 4.0f,  6.0f,  8.0f, 10.0f, 12.0f ];\n"
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
        "        int32[] d1 = [ 1, 2, 3, 4 ];\n"
        "        int64[] sa = heap int64[1]; sa[0] = 4;\n"
        "        Tensor<int32> a = Tensor.of<int32>(d1, sa);\n"               // [1,2,3,4]
        "        if (Tensor.sum<int32, int32>(a) != 10) { return -1; }\n"
        "        if (Tensor.prod<int32, int32>(a) != 24) { return -2; }\n"
        "        if (Tensor.min<int32>(a) != 1) { return -3; }\n"
        "        if (Tensor.max<int32>(a) != 4) { return -4; }\n"
        "        if (Tensor.mean<int32, float64>(a) != 2.5) { return -5; }\n"
        "        int32[] d2 = [ 1, 2, 3, 4, 5, 6 ];\n"
        "        int64[] sb = heap int64[2]; sb[0] = 2; sb[1] = 3;\n"
        "        Tensor<int32> b = Tensor.of<int32>(d2, sb);\n"             // [[1,2,3],[4,5,6]]
        "        if (Tensor.sum<int32, int32>(b) != 21) { return -6; }\n"     // n-D walk
        "        if (Tensor.prod<int32, int32>(b) != 720) { return -7; }\n"
        "        if (Tensor.min<int32>(b) != 1) { return -8; }\n"
        "        if (Tensor.max<int32>(b) != 6) { return -9; }\n"
        "        if (Tensor.mean<int32, float64>(b) != 3.5) { return -10; }\n"
        "        float32[] d3 = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
        "        int64[] sc = heap int64[1]; sc[0] = 4;\n"                    // own shape (not shared with a)
        "        Tensor<float32> c = Tensor.of<float32>(d3, sc);\n"
        "        if (Tensor.sum<float32, float32>(c) != 10.0f) { return -11; }\n"
        "        if (Tensor.mean<float32, float32>(c) != 2.5f) { return -12; }\n"
        "        if (Tensor.min<float32>(c) != 1.0f) { return -13; }\n"
        "        if (Tensor.max<float32>(c) != 4.0f) { return -14; }\n"
        // accumulator upcast: int8 elements summed into int64 (sum overflows int8)
        "        int8[] d4 = [ 100, 100, 100 ];\n"
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
        "        int32[] d2 = [ 1, 2, 3, 4, 5, 6 ];\n"
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
        "        int32[] d3 = [ 0, 1, 2, 3, 4, 5, 6, 7 ];\n"
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
        "        int32[] da = [ 3, 1, 4, 1, 5, 9, 2, 6 ];\n"
        "        int64[] sa = heap int64[1]; sa[0] = 8;\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, sa);\n"
        "        if (Tensor.argmin<int32>(a) != 1) { return -1; }\n"          // first min(1) at idx 1
        "        if (Tensor.argmax<int32>(a) != 5) { return -2; }\n"          // max(9) at idx 5
        "        if (Tensor.countNonzero<int32>(a) != 8) { return -3; }\n"
        "        if (!Tensor.any<int32>(a)) { return -4; }\n"
        "        if (!Tensor.all<int32>(a)) { return -5; }\n"
        "        int32[] dz = [ 0, 3, 0, 2 ];\n"
        "        int64[] sz = heap int64[1]; sz[0] = 4;\n"
        "        Tensor<int32> z = Tensor.of<int32>(dz, sz);\n"
        "        if (Tensor.countNonzero<int32>(z) != 2) { return -6; }\n"
        "        if (!Tensor.any<int32>(z)) { return -7; }\n"
        "        if (Tensor.all<int32>(z)) { return -8; }\n"                  // has a zero
        "        if (Tensor.argmin<int32>(z) != 0) { return -9; }\n"         // min(0) at idx 0
        "        int32[] d0 = [ 0, 0, 0 ];\n"
        "        int64[] s0 = heap int64[1]; s0[0] = 3;\n"
        "        Tensor<int32> zero = Tensor.of<int32>(d0, s0);\n"
        "        if (Tensor.any<int32>(zero)) { return -10; }\n"
        "        if (Tensor.all<int32>(zero)) { return -11; }\n"
        "        if (Tensor.countNonzero<int32>(zero) != 0) { return -12; }\n"
        // 2-D argmin/argmax return the flattened C-order index
        "        int32[] dm = [ 5, 2, 8, 1 ];\n"
        "        int64[] sm = heap int64[2]; sm[0] = 2; sm[1] = 2;\n"
        "        Tensor<int32> m = Tensor.of<int32>(dm, sm);\n"             // [[5,2],[8,1]]
        "        if (Tensor.argmin<int32>(m) != 3) { return -13; }\n"        // 1 at flat idx 3
        "        if (Tensor.argmax<int32>(m) != 2) { return -14; }\n"        // 8 at flat idx 2
        // boolean masks
        "        boolean[] bm1 = [ true, false, true ];\n"
        "        int64[] sb1 = heap int64[1]; sb1[0] = 3;\n"
        "        Tensor<boolean> m1 = Tensor.of<boolean>(bm1, sb1);\n"
        "        if (!Tensor.anyTrue(m1)) { return -15; }\n"
        "        if (Tensor.allTrue(m1)) { return -16; }\n"                  // has a false
        "        boolean[] bm2 = [ true, true, true ];\n"
        "        int64[] sb2 = heap int64[1]; sb2[0] = 3;\n"
        "        Tensor<boolean> m2 = Tensor.of<boolean>(bm2, sb2);\n"
        "        if (!Tensor.allTrue(m2)) { return -17; }\n"
        "        boolean[] bm3 = [ false, false ];\n"
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
        "        float32[] dx = [ 2.0f, 4.0f, 4.0f, 4.0f, 5.0f, 5.0f, 7.0f, 9.0f ];\n"
        "        int64[] sx = heap int64[1]; sx[0] = 8;\n"
        "        Tensor<float32> x = Tensor.of<float32>(dx, sx);\n"
        "        if (Tensor.variance<float32, float32>(x, 0) != 4.0f) { return -1; }\n"   // population
        "        if (Tensor.std<float32, float32>(x, 0) != 2.0f) { return -2; }\n"
        // sample variance ddof=1 = 32/7 ≈ 4.5714286 (tolerance compare)
        "        float32 v1 = Tensor.variance<float32, float32>(x, 1);\n"
        "        float32 dv = v1 - 4.5714285f; if (dv < 0.0f) { dv = 0.0f - dv; }\n"
        "        if (dv > 0.001f) { return -3; }\n"
        // nan handling
        "        float32[] dn = [ 1.0f, 2.0f, 3.0f ];\n"
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
        "        int32[] d1 = [ 1, 2, 3, 4 ];\n"
        "        int64[] s4 = heap int64[1]; s4[0] = 4;\n"
        "        Tensor<int32> a = Tensor.of<int32>(d1, s4);\n"
        "        Tensor<int32> cs = Tensor.cumsum<int32, int32>(a);\n"        // [1,3,6,10]
        "        if (cs.ndim() != 1 || cs.size() != 4) { return -1; }\n"
        "        if (cs.get1(0) != 1 || cs.get1(1) != 3 || cs.get1(2) != 6 || cs.get1(3) != 10) { return -2; }\n"
        "        Tensor<int32> cp = Tensor.cumprod<int32, int32>(a);\n"       // [1,2,6,24]
        "        if (cp.get1(0) != 1 || cp.get1(1) != 2 || cp.get1(2) != 6 || cp.get1(3) != 24) { return -3; }\n"
        // 2-D flatten cumsum
        "        int32[] d2 = [ 1, 2, 3, 4 ];\n"
        "        int64[] sm = heap int64[2]; sm[0] = 2; sm[1] = 2;\n"
        "        Tensor<int32> m = Tensor.of<int32>(d2, sm);\n"             // [[1,2],[3,4]]
        "        Tensor<int32> fcs = Tensor.cumsum<int32, int32>(m);\n"       // flat [1,3,6,10]
        "        if (fcs.ndim() != 1 || fcs.get1(3) != 10) { return -4; }\n"
        // axis cumsum
        "        int32[] d3 = [ 1, 2, 3, 4 ];\n"
        "        int64[] sm2 = heap int64[2]; sm2[0] = 2; sm2[1] = 2;\n"
        "        Tensor<int32> m2 = Tensor.of<int32>(d3, sm2);\n"           // [[1,2],[3,4]]
        "        Tensor<int32> a0 = Tensor.cumsumAxis<int32, int32>(m2, 0);\n" // [[1,2],[4,6]]
        "        if (a0.ndim() != 2 || a0.shapeAt(0) != 2 || a0.shapeAt(1) != 2) { return -5; }\n"
        "        if (a0.get2(0, 0) != 1 || a0.get2(0, 1) != 2 || a0.get2(1, 0) != 4 || a0.get2(1, 1) != 6) { return -6; }\n"
        "        int32[] d4 = [ 1, 2, 3, 4 ];\n"
        "        int64[] sm3 = heap int64[2]; sm3[0] = 2; sm3[1] = 2;\n"
        "        Tensor<int32> m3 = Tensor.of<int32>(d4, sm3);\n"
        "        Tensor<int32> a1 = Tensor.cumsumAxis<int32, int32>(m3, 1);\n" // [[1,3],[3,7]]
        "        if (a1.get2(0, 0) != 1 || a1.get2(0, 1) != 3 || a1.get2(1, 0) != 3 || a1.get2(1, 1) != 7) { return -7; }\n"
        // axis cumprod
        "        int32[] d5 = [ 1, 2, 3, 4 ];\n"
        "        int64[] sm4 = heap int64[2]; sm4[0] = 2; sm4[1] = 2;\n"
        "        Tensor<int32> m4 = Tensor.of<int32>(d5, sm4);\n"
        "        Tensor<int32> p0 = Tensor.cumprodAxis<int32, int32>(m4, 0);\n" // [[1,2],[3,8]]
        "        if (p0.get2(0, 0) != 1 || p0.get2(0, 1) != 2 || p0.get2(1, 0) != 3 || p0.get2(1, 1) != 8) { return -8; }\n"
        "        int32[] d6 = [ 1, 2, 3, 4 ];\n"
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
// valued floats (sum 36 < 2^24) → exact regardless of atomic order. cajeta.xpu CPU
// backend in-process (no GPU required).
TEST(NumpyOpsTests, reductionsCpuGpuAgree) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.math.Ewise;\n"
        "public final class D {\n"
        "    public static #Tensor<float32> mk() {\n"
        "        float32[] d = [ 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f ];\n"
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
        "        int32[] d = [ 1, 2, 3, 4, 5, 6 ];\n"
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
        "        int32[] perm = [ 1, 0 ];\n"
        "        Tensor<int32> ta = a.transposeAxes(perm);\n"             // same as swapaxes(0,1)
        "        if (ta.shapeAt(0) != 3 || ta.get2(0, 1) != 4 || ta.get2(2, 0) != 3) { return -6; }\n"
        "        Tensor<int32> mv = a.moveaxis(0, 1);\n"                   // 2-D move == swap → (3,2)
        "        if (mv.shapeAt(0) != 3 || mv.shapeAt(1) != 2 || mv.get2(0, 1) != 4) { return -7; }\n"
        "        Tensor<int32> fa = a.flipAll();\n"                        // reverse both axes
        "        if (fa.get2(0, 0) != 6 || fa.get2(1, 2) != 1 || fa.get2(0, 2) != 4) { return -8; }\n"
        // 3-D moveaxis(0,2): (1,2,3) -> (2,3,1); verify shape + values via a contiguous copy
        "        int32[] d3 = [ 1, 2, 3, 4, 5, 6 ];\n"
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
        "        int32[] d = [ 1, 2, 3 ];\n"
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
        "        int32[] d4 = [ 1, 2, 3, 4 ];\n"
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
        "        int32[] d = [ 1, 2, 3, 4, 5, 6, 7, 8, 9 ];\n"
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
        "        int32[] d = [ 1, 2, 3, 4, 5, 6 ];\n"
        "        int64[] s32 = heap int64[2]; s32[0] = 3; s32[1] = 2;\n"
        "        Tensor<int32> a = Tensor.of<int32>(d, s32);\n"
        "        boolean[] cb = [ true, false, true ];\n"
        "        int64[] sc = heap int64[1]; sc[0] = 3;\n"
        "        Tensor<boolean> cond = Tensor.of<boolean>(cb, sc);\n"
        "        Tensor<int32> cp = Tensor.compress<int32>(a, cond, 0);\n"   // (2,2)
        "        if (cp.shapeAt(0) != 2 || cp.shapeAt(1) != 2) { return -1; }\n"
        "        if (cp.get2(0, 0) != 1 || cp.get2(0, 1) != 2 || cp.get2(1, 0) != 5 || cp.get2(1, 1) != 6) { return -2; }\n"
        // choose: indices [0,1,0,1] pick between choices c0=[10,11,12,13], c1=[20,21,22,23]
        "        int64[] di = [ 0, 1, 0, 1 ];\n"
        "        int64[] s4 = heap int64[1]; s4[0] = 4;\n"
        "        Tensor<int64> ix = Tensor.of<int64>(di, s4);\n"
        "        int64[] dc0 = [ 10, 11, 12, 13 ];\n"
        "        int64[] s4a = heap int64[1]; s4a[0] = 4;\n"
        "        int64[] dc1 = [ 20, 21, 22, 23 ];\n"
        "        int64[] s4b = heap int64[1]; s4b[0] = 4;\n"
        "        Tensor<int64>[] ch = heap Tensor<int64>[2];\n"
        "        ch[0] = Tensor.of<int64>(dc0, s4a);\n"
        "        ch[1] = Tensor.of<int64>(dc1, s4b);\n"
        "        Tensor<int64> co = Tensor.choose<int64>(ix, ch);\n"        // [10,21,12,23]
        "        if (co.get1(0) != 10 || co.get1(1) != 21 || co.get1(2) != 12 || co.get1(3) != 23) { return -3; }\n"
        // maskedSelect/maskedAssign round-trip
        "        int32[] dm = [ 1, 2, 3, 4 ];\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<int32> m = Tensor.of<int32>(dm, s22);\n"
        "        boolean[] bm = [ true, false, false, true ];\n"
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
// loop — and the two agree. cajeta.xpu CPU backend in-process (no GPU required).
TEST(NumpyOpsTests, gatherCpuGpuAgree) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.math.Ewise;\n"
        "public final class D {\n"
        "    public static #Tensor<float32> data() {\n"
        "        float32[] d = [ 10.0f, 20.0f, 30.0f, 40.0f ];\n"
        "        int64[] s = heap int64[1]; s[0] = 4;\n"
        "        return Tensor.of<float32>(d, s);\n"
        "    }\n"
        "    public static #Tensor<int64> idx() {\n"
        "        int64[] d = [ 3, 1, 0, 2 ];\n"
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
        "        int32[] da = [ 1, 2, 3, 4 ];\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s22);\n"          // [[1,2],[3,4]]
        "        int32[] db = [ 5, 6, 7, 8 ];\n"
        "        int64[] s22b = heap int64[2]; s22b[0] = 2; s22b[1] = 2;\n"
        "        Tensor<int32> b = Tensor.of<int32>(db, s22b);\n"        // [[5,6],[7,8]]
        "        Tensor<int32> c = Tensor.matmul<int32>(a, b);\n"         // [[19,22],[43,50]]
        "        if (c.shapeAt(0) != 2 || c.shapeAt(1) != 2) { return -1; }\n"
        "        if (c.get2(0, 0) != 19 || c.get2(0, 1) != 22 || c.get2(1, 0) != 43 || c.get2(1, 1) != 50) { return -2; }\n"
        // non-square (2,3)·(3,2) → (2,2)
        "        int32[] de = [ 1, 2, 3, 4, 5, 6 ];\n"
        "        int64[] s23 = heap int64[2]; s23[0] = 2; s23[1] = 3;\n"
        "        Tensor<int32> e = Tensor.of<int32>(de, s23);\n"         // [[1,2,3],[4,5,6]]
        "        int32[] df = [ 7, 8, 9, 10, 11, 12 ];\n"
        "        int64[] s32 = heap int64[2]; s32[0] = 3; s32[1] = 2;\n"
        "        Tensor<int32> f = Tensor.of<int32>(df, s32);\n"         // [[7,8],[9,10],[11,12]]
        "        Tensor<int32> g = Tensor.matmul<int32>(e, f);\n"         // [[58,64],[139,154]]
        "        if (g.shapeAt(0) != 2 || g.shapeAt(1) != 2) { return -3; }\n"
        "        if (g.get2(0, 0) != 58 || g.get2(0, 1) != 64 || g.get2(1, 0) != 139 || g.get2(1, 1) != 154) { return -4; }\n"
        // 1-D dot
        "        int32[] dv = [ 1, 2, 3 ];\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<int32> v = Tensor.of<int32>(dv, s3);\n"
        "        int32[] dw = [ 4, 5, 6 ];\n"
        "        int64[] s3b = heap int64[1]; s3b[0] = 3;\n"
        "        Tensor<int32> w = Tensor.of<int32>(dw, s3b);\n"
        "        if (Tensor.dot<int32>(v, w) != 32) { return -5; }\n"     // 4+10+18
        // float matmul exactness
        "        float32[] dp = [ 1.0f, 0.0f, 0.0f, 1.0f ];\n"
        "        int64[] s22c = heap int64[2]; s22c[0] = 2; s22c[1] = 2;\n"
        "        Tensor<float32> id = Tensor.of<float32>(dp, s22c);\n"   // identity
        "        float32[] dq = [ 2.0f, 3.0f, 4.0f, 5.0f ];\n"
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
        "        int32[] da = [ 1, 2, 3, 4 ];\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s22);\n"
        "        int32[] db = [ 5, 6, 7, 8 ];\n"
        "        int64[] s22b = heap int64[2]; s22b[0] = 2; s22b[1] = 2;\n"
        "        Tensor<int32> b = Tensor.of<int32>(db, s22b);\n"
        "        if (Tensor.vdot<int32>(a, b) != 70) { return -1; }\n"
        // outer([1,2,3],[4,5]) → (3,2) [[4,5],[8,10],[12,15]]
        "        int32[] dv = [ 1, 2, 3 ];\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<int32> v = Tensor.of<int32>(dv, s3);\n"
        "        int32[] dw = [ 4, 5 ];\n"
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
        "        int32[] d9 = [ 1, 2, 3, 4, 5, 6, 7, 8, 9 ];\n"
        "        int64[] s33 = heap int64[2]; s33[0] = 3; s33[1] = 3;\n"
        "        Tensor<int32> a = Tensor.of<int32>(d9, s33);\n"
        "        if (Tensor.trace<int32>(a, 0) != 15) { return -1; }\n"     // 1+5+9
        "        if (Tensor.trace<int32>(a, 1) != 8) { return -2; }\n"      // 2+6
        // kron [[1,2],[3,4]] ⊗ [[10,20],[30,40]] → (4,4)
        "        int32[] da = [ 1, 2, 3, 4 ];\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<int32> ka = Tensor.of<int32>(da, s22);\n"
        "        int32[] dbk = [ 10, 20, 30, 40 ];\n"
        "        int64[] s22b = heap int64[2]; s22b[0] = 2; s22b[1] = 2;\n"
        "        Tensor<int32> kb = Tensor.of<int32>(dbk, s22b);\n"
        "        Tensor<int32> kr = Tensor.kron<int32>(ka, kb);\n"
        "        if (kr.shapeAt(0) != 4 || kr.shapeAt(1) != 4) { return -3; }\n"
        "        if (kr.get2(0, 0) != 10 || kr.get2(0, 2) != 20 || kr.get2(2, 0) != 30 || kr.get2(3, 3) != 160) { return -4; }\n"
        "        if (kr.get2(1, 1) != 40 || kr.get2(0, 1) != 20) { return -5; }\n"
        // matrix_power of [[1,1],[0,1]]: ^2 → [[1,2],[0,1]], ^3 → [[1,3],[0,1]], ^0 → I
        "        int32[] dp = [ 1, 1, 0, 1 ];\n"
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
        "        int32[] de = [ 1, 2, 3, 4, 5, 6 ];\n"
        "        int64[] s23 = heap int64[2]; s23[0] = 2; s23[1] = 3;\n"
        "        Tensor<int32> e = Tensor.of<int32>(de, s23);\n"
        "        int32[] df = [ 7, 8, 9, 10, 11, 12 ];\n"
        "        int64[] s32 = heap int64[2]; s32[0] = 3; s32[1] = 2;\n"
        "        Tensor<int32> f = Tensor.of<int32>(df, s32);\n"
        "        Tensor<int32> g = Tensor.tensordot<int32>(e, f, 1);\n"
        "        if (g.ndim() != 2 || g.shapeAt(0) != 2 || g.shapeAt(1) != 2) { return -1; }\n"
        "        if (g.get2(0, 0) != 58 || g.get2(0, 1) != 64 || g.get2(1, 0) != 139 || g.get2(1, 1) != 154) { return -2; }\n"
        // (2,3)·(3,) naxes=1 → (2,) matrix-vector; b=[1,1,1] → [6,15]
        "        int32[] dv = [ 1, 1, 1 ];\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<int32> v = Tensor.of<int32>(dv, s3);\n"
        "        Tensor<int32> mv = Tensor.tensordot<int32>(e, v, 1);\n"
        "        if (mv.ndim() != 1 || mv.shapeAt(0) != 2) { return -3; }\n"
        "        if (mv.get1(0) != 6 || mv.get1(1) != 15) { return -4; }\n"
        // 3-D · 2-D, naxes=1: (2,2,3)·(3,4) → (2,2,4); spot-check via copy/flatGet
        "        int32[] d12 = [ 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1 ];\n"   // (2,2,3)
        "        int64[] s223 = heap int64[3]; s223[0] = 2; s223[1] = 2; s223[2] = 3;\n"
        "        Tensor<int32> a3 = Tensor.of<int32>(d12, s223);\n"
        "        int32[] dB = [ 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 ];\n"  // (3,4)
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
        "        int32[] da = [ 1, 2, 3, 4, 5, 6 ];\n"
        "        int64[] s23 = heap int64[2]; s23[0] = 2; s23[1] = 3;\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s23);\n"           // [[1,2,3],[4,5,6]]
        "        int32[] db = [ 1, 0, 0, 0, 1, 0 ];\n"
        "        int64[] s23b = heap int64[2]; s23b[0] = 2; s23b[1] = 3;\n"
        "        Tensor<int32> b = Tensor.of<int32>(db, s23b);\n"          // [[1,0,0],[0,1,0]]
        "        Tensor<int32> c = Tensor.inner<int32>(a, b);\n"           // [[1,2],[4,5]]
        "        if (c.ndim() != 2 || c.shapeAt(0) != 2 || c.shapeAt(1) != 2) { return -1; }\n"
        "        if (c.get2(0, 0) != 1 || c.get2(0, 1) != 2 || c.get2(1, 0) != 4 || c.get2(1, 1) != 5) { return -2; }\n"
        // 1-D inner → 0-D scalar: [1,2,3]·[4,5,6] = 32
        "        int32[] dv = [ 1, 2, 3 ];\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<int32> v = Tensor.of<int32>(dv, s3);\n"
        "        int32[] dw = [ 4, 5, 6 ];\n"
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

// 6b/6e — matmul GPU lowering: Ewise.matmulF32Op routes on placement, lowering to
// the CooperativeMatrix tiled GEMM (16x16 tiles, one workgroup per output tile) on
// device. Cross-check: the device coop-matrix result == the Tensor.matmul CPU floor,
// bit-exact (small integers exact in float32). 32x32x32 → 2x2 output tiles, 2 K-tiles.
TEST(NumpyOpsTests, matmulCoopCpuGpuAgree) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.math.Ewise;\n"
        "public final class D {\n"
        // 32x32 float32, A[i,p]=(i+p)%3, B[p,j]=(2p+j)%2 — products in {0,1,2}, the
        // 32-term sum <= 64, exact in float32. Tensor.of owns the shape, so each
        // build gets its own shape array.
        "    public static #Tensor<float32> mkA() {\n"
        "        float32[] d = heap float32[1024];\n"
        "        int32 i = 0;\n"
        "        while (i < 32) {\n"
        "            int32 p = 0;\n"
        "            while (p < 32) { d[i * 32 + p] = (float32) ((i + p) % 3); p = p + 1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        int64[] s = heap int64[2]; s[0] = 32; s[1] = 32;\n"
        "        return Tensor.of<float32>(d, s);\n"
        "    }\n"
        "    public static #Tensor<float32> mkB() {\n"
        "        float32[] d = heap float32[1024];\n"
        "        int32 p = 0;\n"
        "        while (p < 32) {\n"
        "            int32 j = 0;\n"
        "            while (j < 32) { d[p * 32 + j] = (float32) ((2 * p + j) % 2); j = j + 1; }\n"
        "            p = p + 1;\n"
        "        }\n"
        "        int64[] s = heap int64[2]; s[0] = 32; s[1] = 32;\n"
        "        return Tensor.of<float32>(d, s);\n"
        "    }\n"
        "    public static int32 run() {\n"
        // CPU floor: the generic Tensor.matmul.
        "        Tensor<float32> aRef = D.mkA();\n"
        "        Tensor<float32> bRef = D.mkB();\n"
        "        Tensor<float32> cpuRef = Tensor.matmul<float32>(aRef, bRef);\n"
        // Ewise CPU path (host operands) must equal the floor.
        "        Tensor<float32> aH = D.mkA();\n"
        "        Tensor<float32> bH = D.mkB();\n"
        "        Tensor<float32> cHost = Ewise.matmulF32Op(aH, bH);\n"
        // Ewise GPU path (device operands) → CooperativeMatrix tiled GEMM.
        "        Tensor<float32> aG = D.mkA();\n"
        "        Tensor<float32> bG = D.mkB();\n"
        "        aG.gpu();\n"
        "        bG.gpu();\n"
        "        Tensor<float32> cGpu = Ewise.matmulF32Op(aG, bG);\n"
        "        cGpu.cpu();\n"
        "        int64 i = 0;\n"
        "        while (i < 32) {\n"
        "            int64 j = 0;\n"
        "            while (j < 32) {\n"
        "                float32 want = cpuRef.get2(i, j);\n"
        "                if (cHost.get2(i, j) != want) { return -1; }\n"          // Ewise CPU == floor
        "                if (cGpu.get2(i, j) != want) { return -2; }\n"           // coop GPU == floor
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        // guard against an all-zero false pass: row 0 of the product has a positive sum.
        "        float32 total = 0.0f;\n"
        "        i = 0;\n"
        "        while (i < 32) { total = total + cpuRef.get2(0, i); i = i + 1; }\n"
        "        if (total <= 0.0f) { return -3; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
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
        "        int32[] da = [ 1, 2, 3, 4, 5, 6 ];\n"
        "        int64[] s23 = heap int64[2]; s23[0] = 2; s23[1] = 3;\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s23);\n"
        "        Tensor<int32>[] o1 = heap Tensor<int32>[1]; o1[0] = a;\n"
        "        Tensor<int32> tr = Tensor.einsum<int32>(\"ij->ji\", o1);\n"
        "        if (tr.ndim() != 2 || tr.shapeAt(0) != 3 || tr.shapeAt(1) != 2) { return -1; }\n"
        "        if (tr.get2(0, 1) != 4 || tr.get2(2, 0) != 3 || tr.get2(1, 1) != 5) { return -2; }\n"
        // trace "ii->" on (3,3) [1..9] diag 1,5,9 → 15 (0-D scalar)
        "        int32[] d9 = [ 1, 2, 3, 4, 5, 6, 7, 8, 9 ];\n"
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
        "        int32[] de = [ 1, 2, 3, 4, 5, 6 ];\n"
        "        int64[] s23e = heap int64[2]; s23e[0] = 2; s23e[1] = 3;\n"
        "        Tensor<int32> e = Tensor.of<int32>(de, s23e);\n"
        "        int32[] df = [ 7, 8, 9, 10, 11, 12 ];\n"
        "        int64[] s32 = heap int64[2]; s32[0] = 3; s32[1] = 2;\n"
        "        Tensor<int32> f = Tensor.of<int32>(df, s32);\n"
        "        Tensor<int32>[] o6 = heap Tensor<int32>[2]; o6[0] = e; o6[1] = f;\n"
        "        Tensor<int32> mm = Tensor.einsum<int32>(\"ij,jk->ik\", o6);\n"
        "        if (mm.shapeAt(0) != 2 || mm.shapeAt(1) != 2) { return -9; }\n"
        "        if (mm.get2(0, 0) != 58 || mm.get2(0, 1) != 64 || mm.get2(1, 0) != 139 || mm.get2(1, 1) != 154) { return -10; }\n"
        // 1-D inner "i,i->" [1,2,3]·[4,5,6] → 32
        "        int32[] dv = [ 1, 2, 3 ];\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<int32> v = Tensor.of<int32>(dv, s3);\n"
        "        int32[] dw = [ 4, 5, 6 ];\n"
        "        int64[] s3b = heap int64[1]; s3b[0] = 3;\n"
        "        Tensor<int32> w = Tensor.of<int32>(dw, s3b);\n"
        "        Tensor<int32>[] o7 = heap Tensor<int32>[2]; o7[0] = v; o7[1] = w;\n"
        "        Tensor<int32> ip = Tensor.einsum<int32>(\"i,i->\", o7);\n"
        "        if (ip.getAt(dummy) != 32) { return -11; }\n"
        // batched matmul "bij,bjk->bik" (2,2,2)·(2,2,2)
        "        int32[] dba = [ 1, 2, 3, 4, 1, 0, 0, 1 ];\n"     // batch0 [[1,2],[3,4]], batch1 I
        "        int64[] s222 = heap int64[3]; s222[0] = 2; s222[1] = 2; s222[2] = 2;\n"
        "        Tensor<int32> ba = Tensor.of<int32>(dba, s222);\n"
        "        int32[] dbb = [ 5, 6, 7, 8, 2, 3, 4, 5 ];\n"     // batch0 [[5,6],[7,8]], batch1 [[2,3],[4,5]]
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

// 7a — sort + argsort along an axis (numpy default ascending). sort returns a fresh
// sorted copy; argsort returns the Tensor<int64> permutation. Both STABLE (ties keep
// original order — numpy `kind='stable'`). Default numpy axis is the last; tested here
// with an explicit axis arg for 1-D, axis 0 and axis 1.
TEST(NumpyOpsTests, sortArgsortMatchNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // 1-D sort [3,1,2] → [1,2,3]; argsort → [1,2,0]
        "        int32[] d3 = [ 3, 1, 2 ];\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<int32> v = Tensor.of<int32>(d3, s3);\n"
        "        Tensor<int32> sv = Tensor.sort<int32>(v, 0);\n"
        "        if (sv.get1(0) != 1 || sv.get1(1) != 2 || sv.get1(2) != 3) { return -1; }\n"
        "        Tensor<int64> av = Tensor.argsort<int32>(v, 0);\n"
        "        if (av.get1(0) != 1 || av.get1(1) != 2 || av.get1(2) != 0) { return -2; }\n"
        // 2-D sort axis=1: [[3,1,2],[6,4,5]] → [[1,2,3],[4,5,6]]
        "        int32[] d6 = [ 3, 1, 2, 6, 4, 5 ];\n"
        "        int64[] s23 = heap int64[2]; s23[0] = 2; s23[1] = 3;\n"
        "        Tensor<int32> m = Tensor.of<int32>(d6, s23);\n"
        "        Tensor<int32> sm1 = Tensor.sort<int32>(m, 1);\n"
        "        if (sm1.get2(0, 0) != 1 || sm1.get2(0, 2) != 3 || sm1.get2(1, 0) != 4 || sm1.get2(1, 2) != 6) { return -3; }\n"
        "        Tensor<int64> am1 = Tensor.argsort<int32>(m, 1);\n"
        "        if (am1.get2(0, 0) != 1 || am1.get2(0, 1) != 2 || am1.get2(0, 2) != 0) { return -4; }\n"
        // 2-D sort axis=0: [[3,1],[1,2]] → [[1,1],[3,2]]; argsort axis=0 → [[1,0],[0,1]]
        "        int32[] d4 = [ 3, 1, 1, 2 ];\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<int32> m2 = Tensor.of<int32>(d4, s22);\n"
        "        Tensor<int32> sm0 = Tensor.sort<int32>(m2, 0);\n"
        "        if (sm0.get2(0, 0) != 1 || sm0.get2(0, 1) != 1 || sm0.get2(1, 0) != 3 || sm0.get2(1, 1) != 2) { return -5; }\n"
        "        Tensor<int64> am0 = Tensor.argsort<int32>(m2, 0);\n"
        "        if (am0.get2(0, 0) != 1 || am0.get2(0, 1) != 0 || am0.get2(1, 0) != 0 || am0.get2(1, 1) != 1) { return -6; }\n"
        // stability with ties: [2,1,2,1] argsort stable → [1,3,0,2]
        "        int32[] dt = [ 2, 1, 2, 1 ];\n"
        "        int64[] s4 = heap int64[1]; s4[0] = 4;\n"
        "        Tensor<int32> vt = Tensor.of<int32>(dt, s4);\n"
        "        Tensor<int64> at = Tensor.argsort<int32>(vt, 0);\n"
        "        if (at.get1(0) != 1 || at.get1(1) != 3 || at.get1(2) != 0 || at.get1(3) != 2) { return -7; }\n"
        // float sort
        "        float32[] df = [ 2.5f, -1.0f, 0.0f ];\n"
        "        int64[] s3f = heap int64[1]; s3f[0] = 3;\n"
        "        Tensor<float32> vf = Tensor.of<float32>(df, s3f);\n"
        "        Tensor<float32> svf = Tensor.sort<float32>(vf, 0);\n"
        "        if (svf.get1(0) != -1.0f || svf.get1(1) != 0.0f || svf.get1(2) != 2.5f) { return -8; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 7a — searchsorted (binary-search insertion indices into a sorted 1-D array; side
// 0=left → first i with a[i] >= v, 1=right → first i with a[i] > v) + partition /
// argpartition (the kth element lands in its sorted slot, smaller before / larger
// after; the CPU floor returns a fully-ordered lane, a valid stronger partition).
TEST(NumpyOpsTests, searchPartitionMatchNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] da = [ 1, 3, 5, 7 ];\n"
        "        int64[] s4 = heap int64[1]; s4[0] = 4;\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s4);\n"
        "        int32[] dv = [ 0, 1, 4, 5, 8 ];\n"
        "        int64[] s5 = heap int64[1]; s5[0] = 5;\n"
        "        Tensor<int32> v = Tensor.of<int32>(dv, s5);\n"
        // left:  [0,0,2,2,4]
        "        Tensor<int64> il = Tensor.searchsorted<int32>(a, v, 0);\n"
        "        if (il.get1(0) != 0 || il.get1(1) != 0 || il.get1(2) != 2 || il.get1(3) != 2 || il.get1(4) != 4) { return -1; }\n"
        // right: [0,1,2,3,4]
        "        int32[] dv2 = [ 0, 1, 4, 5, 8 ];\n"
        "        int64[] s5b = heap int64[1]; s5b[0] = 5;\n"
        "        Tensor<int32> v2 = Tensor.of<int32>(dv2, s5b);\n"
        "        Tensor<int64> ir = Tensor.searchsorted<int32>(a, v2, 1);\n"
        "        if (ir.get1(0) != 0 || ir.get1(1) != 1 || ir.get1(2) != 2 || ir.get1(3) != 3 || ir.get1(4) != 4) { return -2; }\n"
        // partition t=[3,1,2,5,4], kth=2 → out[2] is the 3rd smallest (==3), and the
        // partition property holds around it.
        "        int32[] dt = [ 3, 1, 2, 5, 4 ];\n"
        "        int64[] s5c = heap int64[1]; s5c[0] = 5;\n"
        "        Tensor<int32> t = Tensor.of<int32>(dt, s5c);\n"
        "        Tensor<int32> pt = Tensor.partition<int32>(t, 2, 0);\n"
        "        int32 piv = pt.get1(2);\n"
        "        if (piv != 3) { return -3; }\n"
        "        int64 i = 0;\n"
        "        while (i < 2) { if (pt.get1(i) > piv) { return -4; } i = i + 1; }\n"
        "        i = 3;\n"
        "        while (i < 5) { if (pt.get1(i) < piv) { return -5; } i = i + 1; }\n"
        // argpartition: t[at[2]] is the 3rd smallest (==3)
        "        int32[] dt2 = [ 3, 1, 2, 5, 4 ];\n"
        "        int64[] s5d = heap int64[1]; s5d[0] = 5;\n"
        "        Tensor<int32> t2 = Tensor.of<int32>(dt2, s5d);\n"
        "        Tensor<int64> at = Tensor.argpartition<int32>(t2, 2, 0);\n"
        "        if (t2.get1(at.get1(2)) != 3) { return -6; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 7 (deferred) — real introselect partition/argpartition: element kth is the true kth
// order statistic, smaller before / larger after (sides NOT fully ordered), across every
// kth, plus a reverse-sorted stress input. Values 0..9 ⇒ the kth order statistic == kth.
TEST(NumpyOpsTests, introselectPartitionMatchNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // partition for every kth in [0,10); each lane a scramble of 0..9
        "        int64 kk = 0;\n"
        "        while (kk < 10) {\n"
        "            int32[] dd = [ 9,1,8,2,7,3,6,4,5,0 ];\n"
        "            int64[] sh = heap int64[1]; sh[0] = 10;\n"
        "            Tensor<int32> t = Tensor.of<int32>(dd, sh);\n"
        "            Tensor<int32> pt = Tensor.partition<int32>(t, kk, 0);\n"
        "            int32 piv = pt.get1(kk);\n"
        "            if (piv != (int32) kk) { return -1; }\n"
        "            int64 i = 0;\n"
        "            while (i < kk) { int32 v = pt.get1(i); if (v >= piv) { return -2; } i = i + 1; }\n"
        "            i = kk + 1;\n"
        "            while (i < 10) { int32 v = pt.get1(i); if (v <= piv) { return -3; } i = i + 1; }\n"
        "            kk = kk + 1;\n"
        "        }\n"
        // argpartition, kth=4: t[at[4]] is the 4th smallest; index-space partition property
        "        int32[] da = [ 9,1,8,2,7,3,6,4,5,0 ];\n"
        "        int64[] sa = heap int64[1]; sa[0] = 10;\n"
        "        Tensor<int32> ta = Tensor.of<int32>(da, sa);\n"
        "        Tensor<int64> at = Tensor.argpartition<int32>(ta, 4, 0);\n"
        "        int64 a4 = at.get1(4);\n"
        "        if (ta.get1(a4) != 4) { return -4; }\n"
        "        int64 j = 0;\n"
        "        while (j < 4) { int64 aj = at.get1(j); int32 v = ta.get1(aj); if (v >= 4) { return -5; } j = j + 1; }\n"
        "        j = 5;\n"
        "        while (j < 10) { int64 aj = at.get1(j); int32 v = ta.get1(aj); if (v <= 4) { return -6; } j = j + 1; }\n"
        // reverse-sorted stress input (exercises the quickselect recursion), kth=3
        "        int32[] dr = [ 9,8,7,6,5,4,3,2,1,0 ];\n"
        "        int64[] sr = heap int64[1]; sr[0] = 10;\n"
        "        Tensor<int32> tr = Tensor.of<int32>(dr, sr);\n"
        "        Tensor<int32> pr = Tensor.partition<int32>(tr, 3, 0);\n"
        "        if (pr.get1(3) != 3) { return -7; }\n"
        "        int64 m = 0;\n"
        "        while (m < 3) { int32 v = pr.get1(m); if (v >= 3) { return -8; } m = m + 1; }\n"
        "        m = 4;\n"
        "        while (m < 10) { int32 v = pr.get1(m); if (v <= 3) { return -9; } m = m + 1; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 7a — unique (sorted distinct values over the flattened input), flatnonzero (C-order
// flat indices where != 0), nonzero (per-dimension coordinate arrays, the numpy tuple),
// extract (arr elements where a condition is nonzero). All return fresh tensors.
TEST(NumpyOpsTests, uniqueNonzeroMatchNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // unique 1-D [3,1,2,3,1] → [1,2,3]
        "        int32[] du = [ 3, 1, 2, 3, 1 ];\n"
        "        int64[] s5 = heap int64[1]; s5[0] = 5;\n"
        "        Tensor<int32> u = Tensor.of<int32>(du, s5);\n"
        "        Tensor<int32> uq = Tensor.unique<int32>(u);\n"
        "        if (uq.ndim() != 1 || uq.size() != 3) { return -1; }\n"
        "        if (uq.get1(0) != 1 || uq.get1(1) != 2 || uq.get1(2) != 3) { return -2; }\n"
        // unique 2-D [[3,1],[1,2]] → [1,2,3]
        "        int32[] d4 = [ 3, 1, 1, 2 ];\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<int32> m = Tensor.of<int32>(d4, s22);\n"
        "        Tensor<int32> uq2 = Tensor.unique<int32>(m);\n"
        "        if (uq2.size() != 3 || uq2.get1(0) != 1 || uq2.get1(1) != 2 || uq2.get1(2) != 3) { return -3; }\n"
        // flatnonzero 2-D [[0,3],[4,0]] → C-order flat [1,2]
        "        int32[] dz = [ 0, 3, 4, 0 ];\n"
        "        int64[] s22b = heap int64[2]; s22b[0] = 2; s22b[1] = 2;\n"
        "        Tensor<int32> z = Tensor.of<int32>(dz, s22b);\n"
        "        Tensor<int64> fz = Tensor.flatnonzero<int32>(z);\n"
        "        if (fz.size() != 2 || fz.get1(0) != 1 || fz.get1(1) != 2) { return -4; }\n"
        // nonzero 2-D [[0,3],[4,0]] → ([0,1],[1,0])
        "        int32[] dz2 = [ 0, 3, 4, 0 ];\n"
        "        int64[] s22c = heap int64[2]; s22c[0] = 2; s22c[1] = 2;\n"
        "        Tensor<int32> z2 = Tensor.of<int32>(dz2, s22c);\n"
        "        Tensor<int64>[] nz = Tensor.nonzero<int32>(z2);\n"
        "        Tensor<int64> rows = nz[0];\n"
        "        Tensor<int64> cols = nz[1];\n"
        "        if (rows.size() != 2 || cols.size() != 2) { return -5; }\n"
        "        if (rows.get1(0) != 0 || rows.get1(1) != 1) { return -6; }\n"
        "        if (cols.get1(0) != 1 || cols.get1(1) != 0) { return -7; }\n"
        // extract condition=[1,0,2,0,3], arr=[10,20,30,40,50] → [10,30,50]
        "        int32[] dc = [ 1, 0, 2, 0, 3 ];\n"
        "        int64[] s5b = heap int64[1]; s5b[0] = 5;\n"
        "        Tensor<int32> cond = Tensor.of<int32>(dc, s5b);\n"
        "        int32[] dar = [ 10, 20, 30, 40, 50 ];\n"
        "        int64[] s5c = heap int64[1]; s5c[0] = 5;\n"
        "        Tensor<int32> arr = Tensor.of<int32>(dar, s5c);\n"
        "        Tensor<int32> ex = Tensor.extract<int32>(cond, arr);\n"
        "        if (ex.size() != 3 || ex.get1(0) != 10 || ex.get1(1) != 30 || ex.get1(2) != 50) { return -8; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 7b — GPU sort: Ewise.sortF32Op routes on placement, lowering to the bitonic-sort
// network (host-driven O(log^2 n) compare-exchange stages over the device buffer) on
// device. Cross-check: the device bitonic result == the Tensor.sort CPU floor, exact
// (distinct float32 keys). Length 8 (power of two) → 6 stages.
TEST(NumpyOpsTests, sortCpuGpuAgree) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.math.Ewise;\n"
        "public final class D {\n"
        "    public static #Tensor<float32> mk() {\n"
        "        float32[] d = heap float32[8];\n"
        "        d[0] = 5.0f; d[1] = 2.0f; d[2] = 8.0f; d[3] = 1.0f;\n"
        "        d[4] = 9.0f; d[5] = 3.0f; d[6] = 7.0f; d[7] = 4.0f;\n"
        "        int64[] s = heap int64[1]; s[0] = 8;\n"
        "        return Tensor.of<float32>(d, s);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Tensor<float32> tRef = D.mk();\n"
        "        Tensor<float32> cpuRef = Tensor.sort<float32>(tRef, 0);\n"   // [1,2,3,4,5,7,8,9]
        "        Tensor<float32> tg = D.mk();\n"
        "        tg.gpu();\n"
        "        Tensor<float32> gpuSorted = Ewise.sortF32Op(tg);\n"          // bitonic on device
        "        gpuSorted.cpu();\n"
        "        int64 i = 0;\n"
        "        while (i < 8) {\n"
        "            if (gpuSorted.get1(i) != cpuRef.get1(i)) { return -1; }\n"  // GPU == CPU floor
        "            i = i + 1;\n"
        "        }\n"
        // sanity: the floor is actually ascending and spans the data
        "        if (cpuRef.get1(0) != 1.0f || cpuRef.get1(7) != 9.0f) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 8a — fft/ifft round trip + reference-DFT spot checks (Phase 8, cajeta.math.fft).
// Complex signals are interleaved float32 ([2i]=re,[2i+1]=im). Radix-2 Cooley-Tukey,
// power-of-two length. impulse→flat ones; constant→DC only; ifft(fft(x))≈x. float32
// accumulation, so checked within tolerance.
TEST(NumpyOpsTests, fftRoundTripMatchNumpy) {
    std::string src = std::string(PRE) +
        "import cajeta.math.fft.Fft;\n"
        "public final class D {\n"
        "    public static boolean close(float32 a, float32 b) {\n"
        "        float32 d = a - b; if (d < 0.0f) { d = -d; } return d < 0.01f;\n"
        "    }\n"
        "    public static int32 run() {\n"
        // impulse [1,0,0,0] (N=4) → flat ones spectrum
        "        float32[] di = [ 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f ];\n"
        "        int64[] s8 = heap int64[1]; s8[0] = 8;\n"
        "        Tensor<float32> imp = Tensor.of<float32>(di, s8);\n"
        "        Tensor<float32> fi = Fft.fft(imp);\n"
        "        int64 i = 0;\n"
        "        while (i < 4) {\n"
        "            if (!D.close(fi.get1(2 * i), 1.0f)) { return -1; }\n"
        "            if (!D.close(fi.get1(2 * i + 1), 0.0f)) { return -2; }\n"
        "            i = i + 1;\n"
        "        }\n"
        // constant [1,1,1,1] (N=4) → DC = 4, rest 0
        "        float32[] dc = [ 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f ];\n"
        "        int64[] s8b = heap int64[1]; s8b[0] = 8;\n"
        "        Tensor<float32> con = Tensor.of<float32>(dc, s8b);\n"
        "        Tensor<float32> fc = Fft.fft(con);\n"
        "        if (!D.close(fc.get1(0), 4.0f) || !D.close(fc.get1(1), 0.0f)) { return -3; }\n"
        "        i = 1;\n"
        "        while (i < 4) {\n"
        "            if (!D.close(fc.get1(2 * i), 0.0f) || !D.close(fc.get1(2 * i + 1), 0.0f)) { return -4; }\n"
        "            i = i + 1;\n"
        "        }\n"
        // round trip ifft(fft(x)) ≈ x for an arbitrary complex signal
        "        float32[] dx = [ 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f ];\n"
        "        int64[] s8c = heap int64[1]; s8c[0] = 8;\n"
        "        Tensor<float32> x = Tensor.of<float32>(dx, s8c);\n"
        "        Tensor<float32> fx = Fft.fft(x);\n"
        "        Tensor<float32> rt = Fft.ifft(fx);\n"
        "        float32[] want = [ 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f ];\n"
        "        i = 0;\n"
        "        while (i < 8) {\n"
        "            float32 wv = want[(int32) i];\n"
        "            float32 gv = rt.get1(i);\n"
        "            if (!D.close(gv, wv)) { return -5; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 8a/8c — fftfreq (DFT sample frequencies, numpy order), fftshift (zero-freq to centre),
// rfft (real → half spectrum) + irfft (Hermitian reconstruct → real), real round trip.
TEST(NumpyOpsTests, fftFreqShiftRealMatchNumpy) {
    std::string src = std::string(PRE) +
        "import cajeta.math.fft.Fft;\n"
        "public final class D {\n"
        "    public static boolean close(float32 a, float32 b) {\n"
        "        float32 d = a - b; if (d < 0.0f) { d = -d; } return d < 0.01f;\n"
        "    }\n"
        "    public static int32 run() {\n"
        // fftfreq(4, 1.0) → [0, 0.25, -0.5, -0.25]
        "        Tensor<float32> fr = Fft.fftfreq(4, 1.0f);\n"
        "        if (!D.close(fr.get1(0), 0.0f) || !D.close(fr.get1(1), 0.25f)) { return -1; }\n"
        "        if (!D.close(fr.get1(2), -0.5f) || !D.close(fr.get1(3), -0.25f)) { return -2; }\n"
        // fftshift([0,0.25,-0.5,-0.25]) → [-0.5,-0.25,0,0.25]
        "        Tensor<float32> sh = Fft.fftshift(fr);\n"
        "        if (!D.close(sh.get1(0), -0.5f) || !D.close(sh.get1(1), -0.25f)) { return -3; }\n"
        "        if (!D.close(sh.get1(2), 0.0f) || !D.close(sh.get1(3), 0.25f)) { return -4; }\n"
        // rfft of real [1,1,1,1] (N=4) → 3 bins: DC=4, rest 0
        "        float32[] dr = [ 1.0f, 1.0f, 1.0f, 1.0f ];\n"
        "        int64[] s4 = heap int64[1]; s4[0] = 4;\n"
        "        Tensor<float32> r = Tensor.of<float32>(dr, s4);\n"
        "        Tensor<float32> rf = Fft.rfft(r);\n"
        "        if (rf.size() != 6) { return -5; }\n"                  // 2*(4/2+1)=6
        "        if (!D.close(rf.get1(0), 4.0f) || !D.close(rf.get1(1), 0.0f)) { return -6; }\n"
        "        if (!D.close(rf.get1(2), 0.0f) || !D.close(rf.get1(4), 0.0f)) { return -7; }\n"
        // real round trip irfft(rfft([1,2,3,4]),4) ≈ [1,2,3,4]
        "        float32[] dx = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
        "        int64[] s4b = heap int64[1]; s4b[0] = 4;\n"
        "        Tensor<float32> x = Tensor.of<float32>(dx, s4b);\n"
        "        Tensor<float32> xf = Fft.rfft(x);\n"
        "        Tensor<float32> xb = Fft.irfft(xf, 4);\n"
        "        float32[] want = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
        "        int64 i = 0;\n"
        "        while (i < 4) {\n"
        "            float32 wv = want[(int32) i];\n"
        "            float32 gv = xb.get1(i);\n"
        "            if (!D.close(gv, wv)) { return -8; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 6 (deferred) — batched matmul: a (2,2,2) @ b (2,2,2) per-batch. numpy oracle
// np.matmul([[[1,2],[3,4]],[[5,6],[7,8]]], [[[1,0],[1,1]],[[2,1],[0,3]]]) = [3,2,7,4,10,23,14,31].
TEST(NumpyOpsTests, batchedMatmulMatchNumpy) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] da = [ 1,2, 3,4, 5,6, 7,8 ];\n"
        "        int64[] sa = heap int64[3]; sa[0] = 2; sa[1] = 2; sa[2] = 2;\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, sa);\n"
        "        int32[] db = [ 1,0, 1,1, 2,1, 0,3 ];\n"
        "        int64[] sb = heap int64[3]; sb[0] = 2; sb[1] = 2; sb[2] = 2;\n"
        "        Tensor<int32> b = Tensor.of<int32>(db, sb);\n"
        "        Tensor<int32> c = Tensor.matmulBatched<int32>(a, b);\n"
        "        if (c.ndim() != 3 || c.shapeAt(0) != 2 || c.shapeAt(1) != 2 || c.shapeAt(2) != 2) { return -1; }\n"
        "        int32[] want = [ 3,2, 7,4, 10,23, 14,31 ];\n"
        "        int64 i = 0;\n"
        "        while (i < 8) {\n"
        "            int32 wv = want[(int32) i]; int32 gv = c.flatGet(i);\n"
        "            if (gv != wv) { return -2; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 8 (deferred) — fft2/ifft2 (2-D DFT), fftn (N-D per-axis walk), hfft (Hermitian → real).
// Oracles from numpy: fft2([[1,2],[3,4]]) = [10,-2,-4,0]; hfft([1+2j,3-1j,0.5],4)=[7.5,-1.5,-4.5,2.5].
TEST(NumpyOpsTests, fftMultiDimAndHermitianMatchNumpy) {
    std::string src = std::string(PRE) +
        "import cajeta.math.fft.Fft;\n"
        "public final class D {\n"
        "    public static boolean close(float32 a, float32 b) {\n"
        "        float32 d = a - b; if (d < 0.0f) { d = -d; } return d < 0.01f;\n"
        "    }\n"
        "    public static int32 run() {\n"
        // 2x2 complex [[1,2],[3,4]] interleaved row-major
        "        float32[] dm = [ 1.0f,0.0f, 2.0f,0.0f, 3.0f,0.0f, 4.0f,0.0f ];\n"
        "        int64[] s8 = heap int64[1]; s8[0] = 8;\n"
        "        Tensor<float32> x = Tensor.of<float32>(dm, s8);\n"
        "        Tensor<float32> f = Fft.fft2(x, 2, 2);\n"
        "        float32[] wf = [ 10.0f,0.0f, -2.0f,0.0f, -4.0f,0.0f, 0.0f,0.0f ];\n"
        "        int64 i = 0;\n"
        "        while (i < 8) {\n"
        "            float32 wv = wf[(int32) i]; float32 gv = f.get1(i);\n"
        "            if (!D.close(gv, wv)) { return -1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        // ifft2 round trip → reals [1,2,3,4] at even (real) slots
        "        Tensor<float32> rt = Fft.ifft2(f, 2, 2);\n"
        "        float32[] wr = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
        "        i = 0;\n"
        "        while (i < 4) {\n"
        "            float32 wv = wr[(int32) i]; float32 gv = rt.get1(2 * i);\n"
        "            if (!D.close(gv, wv)) { return -2; }\n"
        "            i = i + 1;\n"
        "        }\n"
        // fftn with dims [2,2] must equal fft2
        "        int64[] dd = heap int64[2]; dd[0] = 2; dd[1] = 2;\n"
        "        Tensor<float32> fn = Fft.fftn(x, dd, 2);\n"
        "        i = 0;\n"
        "        while (i < 8) {\n"
        "            float32 a = fn.get1(i); float32 b = f.get1(i);\n"
        "            if (!D.close(a, b)) { return -3; }\n"
        "            i = i + 1;\n"
        "        }\n"
        // hfft([1+2j,3-1j,0.5], 4) → real [7.5,-1.5,-4.5,2.5]
        "        float32[] dh = [ 1.0f,2.0f, 3.0f,-1.0f, 0.5f,0.0f ];\n"
        "        int64[] s6 = heap int64[1]; s6[0] = 6;\n"
        "        Tensor<float32> hin = Tensor.of<float32>(dh, s6);\n"
        "        Tensor<float32> h = Fft.hfft(hin, 4);\n"
        "        float32[] wh = [ 7.5f, -1.5f, -4.5f, 2.5f ];\n"
        "        i = 0;\n"
        "        while (i < 4) {\n"
        "            float32 wv = wh[(int32) i]; float32 gv = h.get1(i);\n"
        "            if (!D.close(gv, wv)) { return -4; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 9 (deferred) — permutation/shuffle/choice over the Philox stream: valid permutation +
// reproducible; shuffle preserves the multiset; choice(replace=false) is distinct + in-range,
// choice(replace=true) is in-range. Structural (numpy uses a different permutation algorithm).
TEST(NumpyOpsTests, permutationShuffleChoice) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.math.random.Generator;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Generator g1 = heap Generator(7);\n"
        "        Tensor<int64> p = g1.permutation(8);\n"
        "        int64[] seen = heap int64[8];\n"
        "        int64 i = 0;\n"
        "        while (i < 8) { seen[i] = 0; i = i + 1; }\n"
        "        i = 0;\n"
        "        while (i < 8) {\n"
        "            int64 v = p.get1(i);\n"
        "            if (v < 0 || v >= 8) { return -1; }\n"
        "            seen[v] = seen[v] + 1;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        i = 0;\n"
        "        while (i < 8) { if (seen[i] != 1) { return -2; } i = i + 1; }\n"
        // reproducible for the same seed
        "        Generator g2 = heap Generator(7);\n"
        "        Tensor<int64> p2 = g2.permutation(8);\n"
        "        i = 0;\n"
        "        while (i < 8) { if (p.get1(i) != p2.get1(i)) { return -3; } i = i + 1; }\n"
        // shuffle preserves the multiset (sum of 0..7 == 28)
        "        float32[] df = [ 0.0f, 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f ];\n"
        "        int64[] s8 = heap int64[1]; s8[0] = 8;\n"
        "        Tensor<float32> t = Tensor.of<float32>(df, s8);\n"
        "        Generator g3 = heap Generator(99);\n"
        "        g3.shuffle(t);\n"
        "        float32 sum = 0.0f;\n"
        "        i = 0;\n"
        "        while (i < 8) { sum = sum + t.get1(i); i = i + 1; }\n"
        "        if (sum < 27.9f || sum > 28.1f) { return -4; }\n"
        // choice without replacement: distinct + in range
        "        Generator g4 = heap Generator(5);\n"
        "        Tensor<int64> ch = g4.choice(10, 5, false);\n"
        "        int64[] seen2 = heap int64[10];\n"
        "        i = 0;\n"
        "        while (i < 10) { seen2[i] = 0; i = i + 1; }\n"
        "        i = 0;\n"
        "        while (i < 5) {\n"
        "            int64 v = ch.get1(i);\n"
        "            if (v < 0 || v >= 10) { return -5; }\n"
        "            if (seen2[v] != 0) { return -6; }\n"
        "            seen2[v] = 1;\n"
        "            i = i + 1;\n"
        "        }\n"
        // choice with replacement: in range
        "        Tensor<int64> ch2 = g4.choice(3, 20, true);\n"
        "        i = 0;\n"
        "        while (i < 20) {\n"
        "            int64 v = ch2.get1(i);\n"
        "            if (v < 0 || v >= 3) { return -7; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 11 (deferred) — GPU linalg (representative): Ewise.normFroF32 routes on placement; the
// device atomic sum-of-squares + sqrt agrees with the CPU floor. [2,3,6] → sqrt(49)=7.
TEST(NumpyOpsTests, normFroCpuGpuAgree) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.math.Ewise;\n"
        "public final class D {\n"
        "    public static #Tensor<float32> data() {\n"
        "        float32[] d = [ 2.0f, 3.0f, 6.0f ];\n"
        "        int64[] s = heap int64[1]; s[0] = 3;\n"
        "        return Tensor.of<float32>(d, s);\n"
        "    }\n"
        "    public static boolean close(float32 a, float32 b) {\n"
        "        float32 d = a - b; if (d < 0.0f) { d = -d; } return d < 0.001f;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Tensor<float32> tc = D.data();\n"
        "        float32 cpu = Ewise.normFroF32(tc);\n"
        "        if (!D.close(cpu, 7.0f)) { return -1; }\n"
        "        Tensor<float32> tg = D.data();\n"
        "        tg.gpu();\n"
        "        float32 gpu = Ewise.normFroF32(tg);\n"
        "        if (!D.close(gpu, cpu)) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 6.6 (deferred) — coop GEMM non-f32: Ewise.matmulF64Op runs the CooperativeMatrix tiled
// GEMM for float64 (the software coop tier is dtype-generic); the device result equals the
// generic Tensor.matmul<float64> floor bit-for-bit (32x32x32, integer-valued so exact).
TEST(NumpyOpsTests, matmulCoopF64CpuGpuAgree) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.math.Ewise;\n"
        "public final class D {\n"
        "    public static #Tensor<float64> mkA() {\n"
        "        float64[] d = heap float64[1024];\n"
        "        int32 i = 0;\n"
        "        while (i < 32) {\n"
        "            int32 p = 0;\n"
        "            while (p < 32) { d[i * 32 + p] = (float64) ((i + p) % 3); p = p + 1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        int64[] s = heap int64[2]; s[0] = 32; s[1] = 32;\n"
        "        return Tensor.of<float64>(d, s);\n"
        "    }\n"
        "    public static #Tensor<float64> mkB() {\n"
        "        float64[] d = heap float64[1024];\n"
        "        int32 p = 0;\n"
        "        while (p < 32) {\n"
        "            int32 j = 0;\n"
        "            while (j < 32) { d[p * 32 + j] = (float64) ((2 * p + j) % 2); j = j + 1; }\n"
        "            p = p + 1;\n"
        "        }\n"
        "        int64[] s = heap int64[2]; s[0] = 32; s[1] = 32;\n"
        "        return Tensor.of<float64>(d, s);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Tensor<float64> aRef = D.mkA();\n"
        "        Tensor<float64> bRef = D.mkB();\n"
        "        Tensor<float64> cpuRef = Tensor.matmul<float64>(aRef, bRef);\n"
        "        Tensor<float64> aG = D.mkA();\n"
        "        Tensor<float64> bG = D.mkB();\n"
        "        aG.gpu();\n"
        "        bG.gpu();\n"
        "        Tensor<float64> cGpu = Ewise.matmulF64Op(aG, bG);\n"
        "        cGpu.cpu();\n"
        "        int64 i = 0;\n"
        "        while (i < 32) {\n"
        "            int64 j = 0;\n"
        "            while (j < 32) {\n"
        "                float64 want = cpuRef.get2(i, j);\n"
        "                if (cGpu.get2(i, j) != want) { return -1; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        float64 total = 0.0;\n"
        "        i = 0;\n"
        "        while (i < 32) { total = total + cpuRef.get2(0, i); i = i + 1; }\n"
        "        if (total <= 0.0) { return -3; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 5c (deferred) — GPU structural ops: Ewise.concatF32Op / sliceF32Op route on placement;
// the device range-copies agree with the CPU loop. concat([1,2,3],[4,5,6,7])→[1..7];
// slice(that,2,3)→[3,4,5].
TEST(NumpyOpsTests, concatSplitCpuGpuAgree) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.math.Ewise;\n"
        "public final class D {\n"
        "    public static #Tensor<float32> a() {\n"
        "        float32[] d = [ 1.0f, 2.0f, 3.0f ];\n"
        "        int64[] s = heap int64[1]; s[0] = 3;\n"
        "        return Tensor.of<float32>(d, s);\n"
        "    }\n"
        "    public static #Tensor<float32> b() {\n"
        "        float32[] d = [ 4.0f, 5.0f, 6.0f, 7.0f ];\n"
        "        int64[] s = heap int64[1]; s[0] = 4;\n"
        "        return Tensor.of<float32>(d, s);\n"
        "    }\n"
        "    public static int32 run() {\n"
        // CPU concat then slice
        "        Tensor<float32> ac = D.a();\n"
        "        Tensor<float32> bc = D.b();\n"
        "        Tensor<float32> cc = Ewise.concatF32Op(ac, bc);\n"          // [1,2,3,4,5,6,7]
        "        if (cc.size() != 7 || cc.get1(0) != 1.0f || cc.get1(6) != 7.0f) { return -1; }\n"
        "        Tensor<float32> sc = Ewise.sliceF32Op(cc, 2, 3);\n"        // [3,4,5]
        "        if (sc.get1(0) != 3.0f || sc.get1(1) != 4.0f || sc.get1(2) != 5.0f) { return -2; }\n"
        // GPU concat then slice, compare to CPU
        "        Tensor<float32> ag = D.a();\n"
        "        Tensor<float32> bg = D.b();\n"
        "        ag.gpu();\n"
        "        bg.gpu();\n"
        "        Tensor<float32> cg = Ewise.concatF32Op(ag, bg);\n"
        "        Tensor<float32> sg = Ewise.sliceF32Op(cg, 2, 3);\n"
        "        cg.cpu();\n"
        "        sg.cpu();\n"
        "        int64 i = 0;\n"
        "        while (i < 7) { if (cg.get1(i) != cc.get1(i)) { return -3; } i = i + 1; }\n"
        "        i = 0;\n"
        "        while (i < 3) { if (sg.get1(i) != sc.get1(i)) { return -4; } i = i + 1; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 4 (deferred) — GPU prefix scan: Ewise.cumsumF32Op routes on placement; the device
// Hillis-Steele inclusive scan agrees with the CPU running sum. [1..8] → [1,3,6,10,15,21,28,36].
TEST(NumpyOpsTests, prefixScanCpuGpuAgree) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.math.Ewise;\n"
        "public final class D {\n"
        "    public static #Tensor<float32> data() {\n"
        "        float32[] d = [ 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f ];\n"
        "        int64[] s = heap int64[1]; s[0] = 8;\n"
        "        return Tensor.of<float32>(d, s);\n"
        "    }\n"
        "    public static boolean close(float32 a, float32 b) {\n"
        "        float32 d = a - b; if (d < 0.0f) { d = -d; } return d < 0.001f;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Tensor<float32> tc = D.data();\n"
        "        Tensor<float32> cpu = Ewise.cumsumF32Op(tc);\n"
        "        float32[] want = [ 1.0f, 3.0f, 6.0f, 10.0f, 15.0f, 21.0f, 28.0f, 36.0f ];\n"
        "        int64 i = 0;\n"
        "        while (i < 8) {\n"
        "            float32 wv = want[(int32) i]; float32 gv = cpu.get1(i);\n"
        "            if (!D.close(gv, wv)) { return -1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        Tensor<float32> tg = D.data();\n"
        "        tg.gpu();\n"
        "        Tensor<float32> gpu = Ewise.cumsumF32Op(tg);\n"
        "        gpu.cpu();\n"
        "        i = 0;\n"
        "        while (i < 8) {\n"
        "            if (!D.close(gpu.get1(i), cpu.get1(i))) { return -2; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 4 (deferred) — GPU min/max reduction: Ewise.minF32/maxF32 route on placement; the device
// atomic-min/max agrees with the CPU loop. data=[3,-1,4,1,-5,9,2,6] → min=-5, max=9.
TEST(NumpyOpsTests, minMaxReduceCpuGpuAgree) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.math.Ewise;\n"
        "public final class D {\n"
        "    public static #Tensor<float32> data() {\n"
        "        float32[] d = [ 3.0f, -1.0f, 4.0f, 1.0f, -5.0f, 9.0f, 2.0f, 6.0f ];\n"
        "        int64[] s = heap int64[1]; s[0] = 8;\n"
        "        return Tensor.of<float32>(d, s);\n"
        "    }\n"
        "    public static boolean close(float32 a, float32 b) {\n"
        "        float32 d = a - b; if (d < 0.0f) { d = -d; } return d < 0.001f;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Tensor<float32> tc = D.data();\n"
        "        float32 cpuMin = Ewise.minF32(tc);\n"
        "        Tensor<float32> tc2 = D.data();\n"
        "        float32 cpuMax = Ewise.maxF32(tc2);\n"
        "        if (!D.close(cpuMin, -5.0f) || !D.close(cpuMax, 9.0f)) { return -1; }\n"
        "        Tensor<float32> tg = D.data();\n"
        "        tg.gpu();\n"
        "        float32 gpuMin = Ewise.minF32(tg);\n"
        "        Tensor<float32> tg2 = D.data();\n"
        "        tg2.gpu();\n"
        "        float32 gpuMax = Ewise.maxF32(tg2);\n"
        "        if (!D.close(gpuMin, cpuMin) || !D.close(gpuMax, cpuMax)) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 5e (deferred) — GPU scatter: Ewise.scatterF32Op routes on placement; the device scatter
// (out[idx[i]]=in[i]) agrees with the CPU loop. values=[10,20,30,40] idx=[3,1,0,2] → [30,20,40,10].
TEST(NumpyOpsTests, scatterCpuGpuAgree) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.math.Ewise;\n"
        "public final class D {\n"
        "    public static #Tensor<float32> data() {\n"
        "        float32[] d = [ 10.0f, 20.0f, 30.0f, 40.0f ];\n"
        "        int64[] s = heap int64[1]; s[0] = 4;\n"
        "        return Tensor.of<float32>(d, s);\n"
        "    }\n"
        "    public static #Tensor<int64> idx() {\n"
        "        int64[] d = [ 3, 1, 0, 2 ];\n"
        "        int64[] s = heap int64[1]; s[0] = 4;\n"
        "        return Tensor.of<int64>(d, s);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Tensor<float32> vc = D.data();\n"
        "        Tensor<int64> ic = D.idx();\n"
        "        Tensor<float32> cpu = Ewise.scatterF32Op(vc, ic, 4);\n"       // host → [30,20,40,10]
        "        if (cpu.get1(0) != 30.0f || cpu.get1(1) != 20.0f || cpu.get1(2) != 40.0f || cpu.get1(3) != 10.0f) { return -1; }\n"
        "        Tensor<float32> vg = D.data();\n"
        "        Tensor<int64> ig = D.idx();\n"
        "        vg.gpu();\n"
        "        ig.gpu();\n"
        "        Tensor<float32> gpu = Ewise.scatterF32Op(vg, ig, 4);\n"       // device → scatter kernel
        "        gpu.cpu();\n"
        "        int64 i = 0;\n"
        "        while (i < 4) {\n"
        "            if (gpu.get1(i) != cpu.get1(i)) { return -2; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 10 (deferred) — GPU atomic-scatter bincount: Ewise.bincountI64Op routes on placement;
// the device atomic scatter agrees with the CPU loop. vals=[0,1,1,2,2,2,3] → counts=[1,2,3,1].
TEST(NumpyOpsTests, bincountCpuGpuAgree) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.math.Ewise;\n"
        "public final class D {\n"
        "    public static #Tensor<int64> data() {\n"
        "        int64[] d = [ 0, 1, 1, 2, 2, 2, 3 ];\n"
        "        int64[] s = heap int64[1]; s[0] = 7;\n"
        "        return Tensor.of<int64>(d, s);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Tensor<int64> tc = D.data();\n"
        "        Tensor<int32> cpu = Ewise.bincountI32Op(tc, 4);\n"           // host → [1,2,3,1]
        "        if (cpu.get1(0) != 1 || cpu.get1(1) != 2 || cpu.get1(2) != 3 || cpu.get1(3) != 1) { return -1; }\n"
        "        Tensor<int64> tg = D.data();\n"
        "        tg.gpu();\n"
        "        Tensor<int32> gpu = Ewise.bincountI32Op(tg, 4);\n"           // device → atomic scatter
        "        gpu.cpu();\n"
        "        int64 i = 0;\n"
        "        while (i < 4) {\n"
        "            if (gpu.get1(i) != cpu.get1(i)) { return -2; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 8b — GPU FFT: FftGpu.fftF32Op routes on placement, lowering to the butterfly-stage
// network (host-driven log2(N) stages over the device buffer). Cross-check: the device
// FFT == the Fft.fft CPU floor within a float tolerance. N=8 (interleaved length 16).
TEST(NumpyOpsTests, fftCpuGpuAgree) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.math.fft.Fft;\n"
        "import cajeta.math.fft.FftGpu;\n"
        "public final class D {\n"
        "    public static boolean close(float32 a, float32 b) {\n"
        "        float32 d = a - b; if (d < 0.0f) { d = -d; } return d < 0.01f;\n"
        "    }\n"
        "    public static #Tensor<float32> mk() {\n"
        "        float32[] d = heap float32[16];\n"
        "        int32 i = 0;\n"
        "        while (i < 16) { d[i] = (float32) ((i * 7 + 3) % 11) - 5.0f; i = i + 1; }\n"
        "        int64[] s = heap int64[1]; s[0] = 16;\n"
        "        return Tensor.of<float32>(d, s);\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Tensor<float32> xRef = D.mk();\n"
        "        Tensor<float32> cpuF = Fft.fft(xRef);\n"           // CPU floor
        "        Tensor<float32> xg = D.mk();\n"
        "        xg.gpu();\n"
        "        Tensor<float32> gpuF = FftGpu.fftF32Op(xg);\n"     // butterfly network on device
        "        gpuF.cpu();\n"
        "        int64 i = 0;\n"
        "        while (i < 16) {\n"
        "            if (!D.close(gpuF.get1(i), cpuF.get1(i))) { return -1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        // sanity: spectrum isn't trivially all-equal (guard a degenerate false pass)
        "        if (D.close(cpuF.get1(0), cpuF.get1(2)) && D.close(cpuF.get1(2), cpuF.get1(4))) { return -2; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 9a — rngReproducible: the Philox4x32-10 counter-based Generator yields the SAME stream
// for the same seed, a DIFFERENT stream for a different seed, values in [0,1), and a
// roughly-uniform mean. Counter-based ⇒ deterministic + element-independent.
TEST(NumpyOpsTests, rngReproducibleMatchNumpy) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.math.random.Generator;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Generator g1 = heap Generator(42);\n"
        "        Tensor<float32> a = g1.uniform(64);\n"
        "        Generator g2 = heap Generator(42);\n"
        "        Tensor<float32> b = g2.uniform(64);\n"
        // same seed → identical stream
        "        int64 i = 0;\n"
        "        while (i < 64) {\n"
        "            if (a.get1(i) != b.get1(i)) { return -1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        // different seed → at least one differs
        "        Generator g3 = heap Generator(43);\n"
        "        Tensor<float32> c = g3.uniform(64);\n"
        "        boolean anyDiff = false;\n"
        "        i = 0;\n"
        "        while (i < 64) {\n"
        "            if (a.get1(i) != c.get1(i)) { anyDiff = true; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (!anyDiff) { return -2; }\n"
        // range [0,1) + rough uniformity (mean of 64 well within 0.3..0.7)
        "        float32 sum = 0.0f;\n"
        "        i = 0;\n"
        "        while (i < 64) {\n"
        "            float32 v = a.get1(i);\n"
        "            if (v < 0.0f || v >= 1.0f) { return -3; }\n"
        "            sum = sum + v;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        float32 mean = sum / 64.0f;\n"
        "        if (mean < 0.3f || mean > 0.7f) { return -4; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 9b — distributionMoments: sampled mean/variance match each distribution's theory.
// uniform → mean 0.5, var 1/12; normal → mean 0, var 1; integers[0,10) → mean ~4.5,
// all in range. 4096 samples → tight tolerances.
TEST(NumpyOpsTests, distributionMomentsMatchNumpy) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.math.random.Generator;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Generator g = heap Generator(12345);\n"
        // uniform: mean ~0.5, var ~0.0833
        "        Tensor<float32> u = g.uniform(4096);\n"
        "        float32 su = 0.0f;\n"
        "        int64 i = 0;\n"
        "        while (i < 4096) { su = su + u.get1(i); i = i + 1; }\n"
        "        float32 mu = su / 4096.0f;\n"
        "        if (mu < 0.47f || mu > 0.53f) { return -1; }\n"
        "        float32 vu = 0.0f;\n"
        "        i = 0;\n"
        "        while (i < 4096) { float32 d = u.get1(i) - mu; vu = vu + d * d; i = i + 1; }\n"
        "        vu = vu / 4096.0f;\n"
        "        if (vu < 0.07f || vu > 0.097f) { return -2; }\n"
        // normal: mean ~0, var ~1
        "        Generator g2 = heap Generator(777);\n"
        "        Tensor<float32> z = g2.normal(4096);\n"
        "        float32 sz = 0.0f;\n"
        "        i = 0;\n"
        "        while (i < 4096) { sz = sz + z.get1(i); i = i + 1; }\n"
        "        float32 mz = sz / 4096.0f;\n"
        "        if (mz < -0.1f || mz > 0.1f) { return -3; }\n"
        "        float32 vz = 0.0f;\n"
        "        i = 0;\n"
        "        while (i < 4096) { float32 d = z.get1(i) - mz; vz = vz + d * d; i = i + 1; }\n"
        "        vz = vz / 4096.0f;\n"
        "        if (vz < 0.85f || vz > 1.15f) { return -4; }\n"
        // integers [0,10): all in range, mean ~4.5
        "        Generator g3 = heap Generator(999);\n"
        "        Tensor<int64> ii = g3.integers(0, 10, 4096);\n"
        "        int64 si = 0;\n"
        "        i = 0;\n"
        "        while (i < 4096) {\n"
        "            int64 v = ii.get1(i);\n"
        "            if (v < 0 || v >= 10) { return -5; }\n"
        "            si = si + v;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        float32 mi = (float32) si / 4096.0f;\n"
        "        if (mi < 4.0f || mi > 5.0f) { return -6; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 9c — rngCpuGpuParity: the counter-based Philox stream is bit-identical on CPU and GPU
// for the same seed (per-element kernel, no sequential state). GeneratorGpu.uniformF32
// (device) == Generator.uniform (CPU floor) exactly.
TEST(NumpyOpsTests, rngCpuGpuParity) {
    std::string src =
        "package test;\n"
        "import cajeta.math.Tensor;\n"
        "import cajeta.math.random.Generator;\n"
        "import cajeta.math.random.GeneratorGpu;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Generator g = heap Generator(2024);\n"
        "        Tensor<float32> cpu = g.uniform(256);\n"               // CPU floor
        "        Tensor<float32> gpu = GeneratorGpu.uniformF32(2024, 256);\n"  // device kernel
        "        gpu.cpu();\n"
        "        int64 i = 0;\n"
        "        boolean anyNonzero = false;\n"
        "        while (i < 256) {\n"
        "            if (cpu.get1(i) != gpu.get1(i)) { return -1; }\n"  // bit-identical parity
        "            if (cpu.get1(i) > 0.0f) { anyNonzero = true; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (!anyNonzero) { return -2; }\n"                     // guard degenerate all-zero
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32Xpu(src), 1);
}

// 10a — binning stats: histogram (equal-width counts over [lo,hi]), bincount (count of
// each non-negative int), digitize (bin index vs increasing edges, right=False).
TEST(NumpyOpsTests, binningStatsMatchNumpy) {
    std::string src = std::string(PRE) +
        "import cajeta.math.stats.Stats;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // histogram: [0.5,1.5,1.7,3.9] into 4 bins over [0,4] → [1,2,0,1]
        "        float32[] dh = [ 0.5f, 1.5f, 1.7f, 3.9f ];\n"
        "        int64[] s4 = heap int64[1]; s4[0] = 4;\n"
        "        Tensor<float32> h = Tensor.of<float32>(dh, s4);\n"
        "        Tensor<int64> hc = Stats.histogram<float32>(h, 4, 0.0f, 4.0f);\n"
        "        if (hc.size() != 4) { return -1; }\n"
        "        if (hc.get1(0) != 1 || hc.get1(1) != 2 || hc.get1(2) != 0 || hc.get1(3) != 1) { return -2; }\n"
        // bincount: [0,1,1,3,3,3] → [1,2,0,3]
        "        int64[] db = [ 0, 1, 1, 3, 3, 3 ];\n"
        "        int64[] s6 = heap int64[1]; s6[0] = 6;\n"
        "        Tensor<int64> b = Tensor.of<int64>(db, s6);\n"
        "        Tensor<int64> bc = Stats.bincount(b);\n"
        "        if (bc.size() != 4) { return -3; }\n"
        "        if (bc.get1(0) != 1 || bc.get1(1) != 2 || bc.get1(2) != 0 || bc.get1(3) != 3) { return -4; }\n"
        // digitize: [0.2,6.4,3.0] vs edges [0,1,2.5,4,10] → [1,4,3]
        "        float32[] dt = [ 0.2f, 6.4f, 3.0f ];\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<float32> dv = Tensor.of<float32>(dt, s3);\n"
        "        float32[] de = [ 0.0f, 1.0f, 2.5f, 4.0f, 10.0f ];\n"
        "        int64[] s5 = heap int64[1]; s5[0] = 5;\n"
        "        Tensor<float32> ed = Tensor.of<float32>(de, s5);\n"
        "        Tensor<int64> dg = Stats.digitize<float32>(dv, ed);\n"
        "        if (dg.get1(0) != 1 || dg.get1(1) != 4 || dg.get1(2) != 3) { return -5; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 10a — cov (covariance matrix, ddof=1, rowvar) + corrcoef (Pearson correlation).
TEST(NumpyOpsTests, covCorrMatchNumpy) {
    std::string src = std::string(PRE) +
        "import cajeta.math.stats.Stats;\n"
        "public final class D {\n"
        "    public static boolean close(float32 a, float32 b) {\n"
        "        float32 d = a - b; if (d < 0.0f) { d = -d; } return d < 0.001f;\n"
        "    }\n"
        "    public static int32 run() {\n"
        // m = [[1,2,3],[6,5,4]] (2 vars, 3 obs): cov=[[1,-1],[-1,1]], corr=[[1,-1],[-1,1]]
        "        float32[] dm = [ 1.0f, 2.0f, 3.0f, 6.0f, 5.0f, 4.0f ];\n"
        "        int64[] s23 = heap int64[2]; s23[0] = 2; s23[1] = 3;\n"
        "        Tensor<float32> m = Tensor.of<float32>(dm, s23);\n"
        "        Tensor<float32> c = Stats.cov<float32>(m);\n"
        "        if (c.ndim() != 2 || c.shapeAt(0) != 2 || c.shapeAt(1) != 2) { return -1; }\n"
        "        if (!D.close(c.get2(0, 0), 1.0f) || !D.close(c.get2(1, 1), 1.0f)) { return -2; }\n"
        "        if (!D.close(c.get2(0, 1), -1.0f) || !D.close(c.get2(1, 0), -1.0f)) { return -3; }\n"
        "        Tensor<float32> r = Stats.corrcoef<float32>(m);\n"
        "        if (!D.close(r.get2(0, 0), 1.0f) || !D.close(r.get2(1, 1), 1.0f)) { return -4; }\n"
        "        if (!D.close(r.get2(0, 1), -1.0f) || !D.close(r.get2(1, 0), -1.0f)) { return -5; }\n"
        // a positively-correlated pair: [[1,2,3],[2,4,6]] → cov[0,1]>0, corr[0,1]=1
        "        float32[] dp = [ 1.0f, 2.0f, 3.0f, 2.0f, 4.0f, 6.0f ];\n"
        "        int64[] s23b = heap int64[2]; s23b[0] = 2; s23b[1] = 3;\n"
        "        Tensor<float32> mp = Tensor.of<float32>(dp, s23b);\n"
        "        Tensor<float32> rp = Stats.corrcoef<float32>(mp);\n"
        "        if (!D.close(rp.get2(0, 1), 1.0f)) { return -6; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 10a — quantile (linear interp), percentile, median (incl. even-length + unsorted input).
TEST(NumpyOpsTests, quantileMedianMatchNumpy) {
    std::string src = std::string(PRE) +
        "import cajeta.math.stats.Stats;\n"
        "public final class D {\n"
        "    public static boolean close(float32 a, float32 b) {\n"
        "        float32 d = a - b; if (d < 0.0f) { d = -d; } return d < 0.001f;\n"
        "    }\n"
        "    public static int32 run() {\n"
        // [1,2,3,4,5]: median 3, q0.25=2, q0.75=4
        "        float32[] d5 = [ 1.0f, 2.0f, 3.0f, 4.0f, 5.0f ];\n"
        "        int64[] s5 = heap int64[1]; s5[0] = 5;\n"
        "        Tensor<float32> a = Tensor.of<float32>(d5, s5);\n"
        "        if (!D.close(Stats.median<float32>(a), 3.0f)) { return -1; }\n"
        "        if (!D.close(Stats.quantile<float32>(a, 0.25f), 2.0f)) { return -2; }\n"
        "        if (!D.close(Stats.quantile<float32>(a, 0.75f), 4.0f)) { return -3; }\n"
        "        if (!D.close(Stats.percentile<float32>(a, 50.0f), 3.0f)) { return -4; }\n"
        // even-length [1,2,3,4]: median 2.5
        "        float32[] d4 = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
        "        int64[] s4 = heap int64[1]; s4[0] = 4;\n"
        "        Tensor<float32> b = Tensor.of<float32>(d4, s4);\n"
        "        if (!D.close(Stats.median<float32>(b), 2.5f)) { return -5; }\n"
        // unsorted [3,1,4,1,5,9,2,6]: sorted [1,1,2,3,4,5,6,9], median 3.5
        "        float32[] d8 = [ 3.0f, 1.0f, 4.0f, 1.0f, 5.0f, 9.0f, 2.0f, 6.0f ];\n"
        "        int64[] s8 = heap int64[1]; s8[0] = 8;\n"
        "        Tensor<float32> c = Tensor.of<float32>(d8, s8);\n"
        "        if (!D.close(Stats.median<float32>(c), 3.5f)) { return -6; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 10c — poly: polyval (Horner), polyadd (degree-aligned), polymul (convolution).
// Coefficients highest-degree-first (numpy order).
TEST(NumpyOpsTests, polyMatchNumpy) {
    std::string src = std::string(PRE) +
        "import cajeta.math.poly.Poly;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        // polyval([1,2,3], 2) = x^2+2x+3 at 2 = 11
        "        int32[] dp = [ 1, 2, 3 ];\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<int32> p = Tensor.of<int32>(dp, s3);\n"
        "        if (Poly.polyval<int32>(p, 2) != 11) { return -1; }\n"
        // polyadd([1,2,3],[4,5]) → [1,6,8]
        "        int32[] da = [ 1, 2, 3 ];\n"
        "        int64[] s3b = heap int64[1]; s3b[0] = 3;\n"
        "        Tensor<int32> a = Tensor.of<int32>(da, s3b);\n"
        "        int32[] db = [ 4, 5 ];\n"
        "        int64[] s2 = heap int64[1]; s2[0] = 2;\n"
        "        Tensor<int32> b = Tensor.of<int32>(db, s2);\n"
        "        Tensor<int32> sum = Poly.polyadd<int32>(a, b);\n"
        "        if (sum.size() != 3 || sum.get1(0) != 1 || sum.get1(1) != 6 || sum.get1(2) != 8) { return -2; }\n"
        // polymul([1,1],[1,1]) → [1,2,1]
        "        int32[] dc = [ 1, 1 ];\n"
        "        int64[] s2b = heap int64[1]; s2b[0] = 2;\n"
        "        Tensor<int32> c = Tensor.of<int32>(dc, s2b);\n"
        "        int32[] dd = [ 1, 1 ];\n"
        "        int64[] s2c = heap int64[1]; s2c[0] = 2;\n"
        "        Tensor<int32> d = Tensor.of<int32>(dd, s2c);\n"
        "        Tensor<int32> mul = Poly.polymul<int32>(c, d);\n"
        "        if (mul.size() != 3 || mul.get1(0) != 1 || mul.get1(1) != 2 || mul.get1(2) != 1) { return -3; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 11b — native linalg: solve (Gaussian elimination + partial pivoting), det, inv.
TEST(NumpyOpsTests, linalgSolveDetInvMatchNumpy) {
    std::string src = std::string(PRE) +
        "import cajeta.math.linalg.LinAlg;\n"
        "public final class D {\n"
        "    public static boolean close(float32 a, float32 b) {\n"
        "        float32 d = a - b; if (d < 0.0f) { d = -d; } return d < 0.001f;\n"
        "    }\n"
        "    public static int32 run() {\n"
        // A=[[2,1],[1,3]], b=[3,4] → x=[1,1]; det=5
        "        float32[] da = [ 2.0f, 1.0f, 1.0f, 3.0f ];\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<float32> a = Tensor.of<float32>(da, s22);\n"
        "        float32[] db = [ 3.0f, 4.0f ];\n"
        "        int64[] s2 = heap int64[1]; s2[0] = 2;\n"
        "        Tensor<float32> b = Tensor.of<float32>(db, s2);\n"
        "        Tensor<float32> x = LinAlg.solve<float32>(a, b);\n"
        "        if (!D.close(x.get1(0), 1.0f) || !D.close(x.get1(1), 1.0f)) { return -1; }\n"
        "        if (!D.close(LinAlg.det<float32>(a), 5.0f)) { return -2; }\n"
        // A2=[[1,2],[3,4]], b2=[5,11] → x=[1,2]; det=-2
        "        float32[] dc = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
        "        int64[] s22b = heap int64[2]; s22b[0] = 2; s22b[1] = 2;\n"
        "        Tensor<float32> a2 = Tensor.of<float32>(dc, s22b);\n"
        "        float32[] dd = [ 5.0f, 11.0f ];\n"
        "        int64[] s2b = heap int64[1]; s2b[0] = 2;\n"
        "        Tensor<float32> b2 = Tensor.of<float32>(dd, s2b);\n"
        "        Tensor<float32> x2 = LinAlg.solve<float32>(a2, b2);\n"
        "        if (!D.close(x2.get1(0), 1.0f) || !D.close(x2.get1(1), 2.0f)) { return -3; }\n"
        "        if (!D.close(LinAlg.det<float32>(a2), -2.0f)) { return -4; }\n"
        // inv(a2) = [[-2,1],[1.5,-0.5]]
        "        Tensor<float32> inv = LinAlg.inv<float32>(a2);\n"
        "        if (!D.close(inv.get2(0, 0), -2.0f) || !D.close(inv.get2(0, 1), 1.0f)) { return -5; }\n"
        "        if (!D.close(inv.get2(1, 0), 1.5f) || !D.close(inv.get2(1, 1), -0.5f)) { return -6; }\n"
        // 3x3 needing a pivot: A=[[0,1,1],[1,0,1],[1,1,0]], b=[2,2,2] → x=[1,1,1]; det=2
        "        float32[] de = [ 0.0f, 1.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f ];\n"
        "        int64[] s33 = heap int64[2]; s33[0] = 3; s33[1] = 3;\n"
        "        Tensor<float32> a3 = Tensor.of<float32>(de, s33);\n"
        "        float32[] df = [ 2.0f, 2.0f, 2.0f ];\n"
        "        int64[] s3 = heap int64[1]; s3[0] = 3;\n"
        "        Tensor<float32> b3 = Tensor.of<float32>(df, s3);\n"
        "        Tensor<float32> x3 = LinAlg.solve<float32>(a3, b3);\n"
        "        if (!D.close(x3.get1(0), 1.0f) || !D.close(x3.get1(1), 1.0f) || !D.close(x3.get1(2), 1.0f)) { return -7; }\n"
        "        if (!D.close(LinAlg.det<float32>(a3), 2.0f)) { return -8; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 11b — native LU (A=P·L·U, unit-lower L, upper U) + QR (A=Q·R, orthonormal Q).
// Verified by reconstruction (matmul) + structure; pivoting/sign are free per numpy.
TEST(NumpyOpsTests, linalgLuQrMatchNumpy) {
    std::string src = std::string(PRE) +
        "import cajeta.math.linalg.LinAlg;\n"
        "public final class D {\n"
        "    public static boolean close(float32 a, float32 b) {\n"
        "        float32 d = a - b; if (d < 0.0f) { d = -d; } return d < 0.002f;\n"
        "    }\n"
        "    public static int32 run() {\n"
        // ---- LU: A=[[4,3],[6,3]] (needs a pivot since |6|>|4|) ----
        "        float32[] da = [ 4.0f, 3.0f, 6.0f, 3.0f ];\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<float32> a = Tensor.of<float32>(da, s22);\n"
        "        Tensor<float32>[] plu = LinAlg.lu<float32>(a);\n"
        "        Tensor<float32> p = plu[0];\n"
        "        Tensor<float32> l = plu[1];\n"
        "        Tensor<float32> u = plu[2];\n"
        // L unit lower-triangular; U upper-triangular
        "        if (!D.close(l.get2(0, 0), 1.0f) || !D.close(l.get2(1, 1), 1.0f) || !D.close(l.get2(0, 1), 0.0f)) { return -1; }\n"
        "        if (!D.close(u.get2(1, 0), 0.0f)) { return -2; }\n"
        // reconstruct P·L·U == A
        "        Tensor<float32> pl = Tensor.matmul<float32>(p, l);\n"
        "        Tensor<float32> recon = Tensor.matmul<float32>(pl, u);\n"
        "        if (!D.close(recon.get2(0, 0), 4.0f) || !D.close(recon.get2(0, 1), 3.0f)) { return -3; }\n"
        "        if (!D.close(recon.get2(1, 0), 6.0f) || !D.close(recon.get2(1, 1), 3.0f)) { return -4; }\n"
        // ---- LU 3x3 needing pivoting: A=[[2,1,1],[4,3,3],[8,7,9]] ----
        "        float32[] db = [ 2.0f, 1.0f, 1.0f, 4.0f, 3.0f, 3.0f, 8.0f, 7.0f, 9.0f ];\n"
        "        int64[] s33 = heap int64[2]; s33[0] = 3; s33[1] = 3;\n"
        "        Tensor<float32> a3 = Tensor.of<float32>(db, s33);\n"
        "        Tensor<float32>[] plu3 = LinAlg.lu<float32>(a3);\n"
        "        Tensor<float32> p3 = plu3[0];\n"
        "        Tensor<float32> l3 = plu3[1];\n"
        "        Tensor<float32> u3 = plu3[2];\n"
        "        Tensor<float32> pl3 = Tensor.matmul<float32>(p3, l3);\n"
        "        Tensor<float32> recon3 = Tensor.matmul<float32>(pl3, u3);\n"
        "        int64 i = 0;\n"
        "        while (i < 3) {\n"
        "            int64 j = 0;\n"
        "            while (j < 3) {\n"
        "                float32 orig = a3.get2(i, j);\n"
        "                float32 rc = recon3.get2(i, j);\n"
        "                if (!D.close(rc, orig)) { return -5; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        // ---- QR: A=[[1,2],[3,4]] → A=Q·R, Q^T·Q=I, R upper ----
        "        float32[] dc = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
        "        int64[] s22b = heap int64[2]; s22b[0] = 2; s22b[1] = 2;\n"
        "        Tensor<float32> aq = Tensor.of<float32>(dc, s22b);\n"
        "        Tensor<float32>[] qr = LinAlg.qr<float32>(aq);\n"
        "        Tensor<float32> q = qr[0];\n"
        "        Tensor<float32> r = qr[1];\n"
        // R upper-triangular
        "        if (!D.close(r.get2(1, 0), 0.0f)) { return -6; }\n"
        // reconstruct Q·R == A
        "        Tensor<float32> qrec = Tensor.matmul<float32>(q, r);\n"
        "        if (!D.close(qrec.get2(0, 0), 1.0f) || !D.close(qrec.get2(0, 1), 2.0f)) { return -7; }\n"
        "        if (!D.close(qrec.get2(1, 0), 3.0f) || !D.close(qrec.get2(1, 1), 4.0f)) { return -8; }\n"
        // orthonormal columns: Q^T·Q == I
        "        Tensor<float32> qt = q.transpose();\n"
        "        Tensor<float32> qtq = Tensor.matmul<float32>(qt, q);\n"
        "        if (!D.close(qtq.get2(0, 0), 1.0f) || !D.close(qtq.get2(1, 1), 1.0f)) { return -9; }\n"
        "        if (!D.close(qtq.get2(0, 1), 0.0f) || !D.close(qtq.get2(1, 0), 0.0f)) { return -10; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 11c — conditioning / edge cases: pinv, lstsq, matrix_rank, cond, Frobenius norm
// (svd-derived), incl. a singular matrix (rank-deficient) handled gracefully.
TEST(NumpyOpsTests, linalgConditioningAndEdgeCases) {
    std::string src = std::string(PRE) +
        "import cajeta.math.linalg.LinAlg;\n"
        "public final class D {\n"
        "    public static boolean close(float32 a, float32 b) {\n"
        "        float32 d = a - b; if (d < 0.0f) { d = -d; } return d < 0.01f;\n"
        "    }\n"
        "    public static int32 run() {\n"
        // ---- full-rank A=[[1,2],[3,4]] ----
        "        float32[] da = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<float32> a = Tensor.of<float32>(da, s22);\n"
        "        if (LinAlg.matrixRank<float32>(a) != 2) { return -1; }\n"
        // Frobenius norm = sqrt(30) ≈ 5.477226
        "        if (!D.close(LinAlg.normFro<float32>(a), 5.477226f)) { return -2; }\n"
        // cond = σmax/σmin ≈ 5.4649858/0.3659662 ≈ 14.9330 (loose: σmin sensitive in f32)
        "        float32 cnd = LinAlg.cond<float32>(a);\n"
        "        float32 cdiff = cnd - 14.9330f; if (cdiff < 0.0f) { cdiff = -cdiff; }\n"
        "        if (cdiff > 0.5f) { return -3; }\n"
        // pinv == inv for nonsingular: A·pinv == I
        "        Tensor<float32> pa = LinAlg.pinv<float32>(a);\n"
        "        Tensor<float32> ident = Tensor.matmul<float32>(a, pa);\n"
        "        if (!D.close(ident.get2(0, 0), 1.0f) || !D.close(ident.get2(1, 1), 1.0f)) { return -4; }\n"
        "        if (!D.close(ident.get2(0, 1), 0.0f) || !D.close(ident.get2(1, 0), 0.0f)) { return -5; }\n"
        // lstsq on a nonsingular system A·x=b, b=[5,11] → x=[1,2]
        "        float32[] db = [ 5.0f, 11.0f ];\n"
        "        int64[] s2 = heap int64[1]; s2[0] = 2;\n"
        "        Tensor<float32> b = Tensor.of<float32>(db, s2);\n"
        "        Tensor<float32> x = LinAlg.lstsq<float32>(a, b);\n"
        "        if (!D.close(x.get1(0), 1.0f) || !D.close(x.get1(1), 2.0f)) { return -6; }\n"
        // ---- singular A2=[[1,2],[2,4]] (rank 1): graceful pseudo-inverse ----
        "        float32[] dc = [ 1.0f, 2.0f, 2.0f, 4.0f ];\n"
        "        int64[] s22b = heap int64[2]; s22b[0] = 2; s22b[1] = 2;\n"
        "        Tensor<float32> a2 = Tensor.of<float32>(dc, s22b);\n"
        "        if (LinAlg.matrixRank<float32>(a2) != 1) { return -7; }\n"
        // pseudo-inverse property: A·A⁺·A == A
        "        Tensor<float32> p2 = LinAlg.pinv<float32>(a2);\n"
        "        Tensor<float32> ap = Tensor.matmul<float32>(a2, p2);\n"
        "        Tensor<float32> apa = Tensor.matmul<float32>(ap, a2);\n"
        "        int64 i = 0;\n"
        "        while (i < 2) {\n"
        "            int64 j = 0;\n"
        "            while (j < 2) {\n"
        "                float32 orig = a2.get2(i, j);\n"
        "                float32 rc = apa.get2(i, j);\n"
        "                if (!D.close(rc, orig)) { return -8; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 10.5 — npio numpy-interop harness: numpy writes a .npy → cajeta reads + verifies →
// cajeta writes a .npy → real numpy reads + verifies. Skips if python/numpy is absent.
TEST(NumpyOpsTests, npyNumpyInteropF32) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path();
    std::string script = (dir / "cajeta_npy_harness.py").string();
    std::string fromNp = (dir / "cajeta_interop_from_np.npy").string();
    std::string toCj   = (dir / "cajeta_interop_to_cj.npy").string();
    // shared oracle script: `write <path>` saves the known array; `verify <path>`
    // loads it and exits 0 iff dtype/shape/values match.
    {
        std::ofstream f(script);
        f << "import numpy as np, sys\n"
             "mode, path = sys.argv[1], sys.argv[2]\n"
             "exp = np.array([[10.0, 20.5], [-3.5, 4.0], [7.0, 8.0]], dtype=np.float32)\n"
             "if mode == 'write':\n"
             "    np.save(path, exp)\n"
             "else:\n"
             "    a = np.load(path)\n"
             "    ok = a.dtype == np.float32 and a.shape == (3, 2) and np.array_equal(a, exp)\n"
             "    sys.exit(0 if ok else 1)\n";
    }
    // Prefer an explicit interpreter via $CAJETA_PYTHON (the one with numpy); default to
    // `python3` (Linux-standard). Probe numpy availability FIRST and skip cleanly if absent —
    // a failed `system()` exec inside this threaded process can leave the heap corrupted and
    // SIGABRT the next test, so we must not reach the harness fork when numpy is missing.
    const char* pyEnv = std::getenv("CAJETA_PYTHON");
    std::string py = pyEnv ? std::string(pyEnv) : std::string("python3");
    if (std::system((py + " -c \"import numpy\"").c_str()) != 0) {
        GTEST_SKIP() << "python/numpy unavailable — skipping npio interop harness";
    }
    auto run = [&](const std::string& mode, const std::string& path) {
        // Plain, POSIX-correct invocation (no `cmd /c` outer-quote wrap — that only parses
        // on Windows and makes /bin/sh treat the whole string as one command name).
        std::string cmd = py + " \"" + script + "\" " + mode + " \"" + path + "\"";
        return std::system(cmd.c_str());
    };
    // 1. numpy writes the array.
    ASSERT_EQ(run("write", fromNp), 0) << "numpy harness failed to write the .npy";
    std::string fromNpC = fromNp, toCjC = toCj;
    std::replace(fromNpC.begin(), fromNpC.end(), '\\', '/');
    std::replace(toCjC.begin(), toCjC.end(), '\\', '/');
    // 2. cajeta reads numpy's file, verifies values, and writes its own .npy.
    std::string src = std::string(PRE) +
        "import cajeta.math.npio.Npy;\n"
        "public final class D {\n"
        "    public static boolean close(float32 a, float32 b) {\n"
        "        float32 d = a - b; if (d < 0.0f) { d = -d; } return d < 0.0001f;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Tensor<float32> a = Npy.loadF32(\"" + fromNpC + "\");\n"
        "        if (a.ndim() != 2 || a.shapeAt(0) != 3 || a.shapeAt(1) != 2) { return -1; }\n"
        "        if (!D.close(a.get2(0, 0), 10.0f) || !D.close(a.get2(0, 1), 20.5f)) { return -2; }\n"
        "        if (!D.close(a.get2(1, 0), -3.5f) || !D.close(a.get2(1, 1), 4.0f)) { return -3; }\n"
        "        if (!D.close(a.get2(2, 0), 7.0f) || !D.close(a.get2(2, 1), 8.0f)) { return -4; }\n"
        "        Npy.saveF32(\"" + toCjC + "\", a);\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
    // 3. real numpy reads cajeta's output and asserts equality.
    EXPECT_EQ(run("verify", toCj), 0);
}

// 10.5 — npio: cajeta round-trip of a float32 .npy (write then read back in cajeta).
TEST(NumpyOpsTests, npyFloat32RoundTrip) {
    std::string path = (std::filesystem::temp_directory_path() / "cajeta_npy_rt.npy").string();
    std::replace(path.begin(), path.end(), '\\', '/');
    std::string src = std::string(PRE) +
        "import cajeta.math.npio.Npy;\n"
        "public final class D {\n"
        "    public static boolean close(float32 a, float32 b) {\n"
        "        float32 d = a - b; if (d < 0.0f) { d = -d; } return d < 0.0001f;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        float32[] da = [ 1.5f, -2.25f, 3.0f, 4.75f, 5.0f, 6.5f ];\n"
        "        int64[] s23 = heap int64[2]; s23[0] = 2; s23[1] = 3;\n"
        "        Tensor<float32> t = Tensor.of<float32>(da, s23);\n"
        "        Npy.saveF32(\"" + path + "\", t);\n"
        "        Tensor<float32> r = Npy.loadF32(\"" + path + "\");\n"
        "        if (r.ndim() != 2) { return -1; }\n"
        "        if (r.shapeAt(0) != 2 || r.shapeAt(1) != 3) { return -2; }\n"
        "        if (!D.close(r.get2(0, 0), 1.5f) || !D.close(r.get2(0, 1), -2.25f)) { return -3; }\n"
        "        if (!D.close(r.get2(0, 2), 3.0f) || !D.close(r.get2(1, 0), 4.75f)) { return -4; }\n"
        "        if (!D.close(r.get2(1, 1), 5.0f) || !D.close(r.get2(1, 2), 6.5f)) { return -5; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 10.5 — npio: cajeta round-trip of a float64 .npy ('<f8', 8-byte little-endian path).
TEST(NumpyOpsTests, npyFloat64RoundTrip) {
    std::string path = (std::filesystem::temp_directory_path() / "cajeta_npy_rt_f64.npy").string();
    std::replace(path.begin(), path.end(), '\\', '/');
    std::string src = std::string(PRE) +
        "import cajeta.math.npio.Npy;\n"
        "public final class D {\n"
        "    public static boolean close(float64 a, float64 b) {\n"
        "        float64 d = a - b; if (d < 0.0) { d = -d; } return d < 0.000000001;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        float64[] da = [ 1.5, -2.25, 3.0, 4.75, 5.0, 6.5 ];\n"
        "        int64[] s23 = heap int64[2]; s23[0] = 2; s23[1] = 3;\n"
        "        Tensor<float64> t = Tensor.of<float64>(da, s23);\n"
        "        Npy.saveF64(\"" + path + "\", t);\n"
        "        Tensor<float64> r = Npy.loadF64(\"" + path + "\");\n"
        "        if (r.ndim() != 2) { return -1; }\n"
        "        if (r.shapeAt(0) != 2 || r.shapeAt(1) != 3) { return -2; }\n"
        "        if (!D.close(r.get2(0, 0), 1.5) || !D.close(r.get2(0, 1), -2.25)) { return -3; }\n"
        "        if (!D.close(r.get2(0, 2), 3.0) || !D.close(r.get2(1, 0), 4.75)) { return -4; }\n"
        "        if (!D.close(r.get2(1, 1), 5.0) || !D.close(r.get2(1, 2), 6.5)) { return -5; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 10.5 — npio: cajeta round-trip of an int32 .npy ('<i4', exact integer equality).
TEST(NumpyOpsTests, npyInt32RoundTrip) {
    std::string path = (std::filesystem::temp_directory_path() / "cajeta_npy_rt_i32.npy").string();
    std::replace(path.begin(), path.end(), '\\', '/');
    std::string src = std::string(PRE) +
        "import cajeta.math.npio.Npy;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] da = [ 7, -3, 0, 2147483647, -2147483648, 42 ];\n"
        "        int64[] s23 = heap int64[2]; s23[0] = 2; s23[1] = 3;\n"
        "        Tensor<int32> t = Tensor.of<int32>(da, s23);\n"
        "        Npy.saveI32(\"" + path + "\", t);\n"
        "        Tensor<int32> r = Npy.loadI32(\"" + path + "\");\n"
        "        if (r.ndim() != 2) { return -1; }\n"
        "        if (r.shapeAt(0) != 2 || r.shapeAt(1) != 3) { return -2; }\n"
        "        if (r.get2(0, 0) != 7 || r.get2(0, 1) != -3) { return -3; }\n"
        "        if (r.get2(0, 2) != 0 || r.get2(1, 0) != 2147483647) { return -4; }\n"
        "        if (r.get2(1, 1) != -2147483648 || r.get2(1, 2) != 42) { return -5; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 10.5 — npio: cajeta round-trip of an int64 .npy ('<i8', 8-byte little-endian path).
TEST(NumpyOpsTests, npyInt64RoundTrip) {
    std::string path = (std::filesystem::temp_directory_path() / "cajeta_npy_rt_i64.npy").string();
    std::replace(path.begin(), path.end(), '\\', '/');
    std::string src = std::string(PRE) +
        "import cajeta.math.npio.Npy;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 big = 9223372036854775807;\n"        // int64 max
        "        int64 small = 0 - big;\n"                     // -(max) = int64 min + 1 (avoid min-literal overflow)
        "        int64[] da = [ 7, -3, 0, big, small, 42 ];\n"
        "        int64[] s23 = heap int64[2]; s23[0] = 2; s23[1] = 3;\n"
        "        Tensor<int64> t = Tensor.of<int64>(da, s23);\n"
        "        Npy.saveI64(\"" + path + "\", t);\n"
        "        Tensor<int64> r = Npy.loadI64(\"" + path + "\");\n"
        "        if (r.ndim() != 2) { return -1; }\n"
        "        if (r.shapeAt(0) != 2 || r.shapeAt(1) != 3) { return -2; }\n"
        "        if (r.get2(0, 0) != 7 || r.get2(0, 1) != -3) { return -3; }\n"
        "        if (r.get2(0, 2) != 0 || r.get2(1, 0) != big) { return -4; }\n"
        "        if (r.get2(1, 1) != small || r.get2(1, 2) != 42) { return -5; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 10.5 — npio numpy-interop for the non-f32 dtypes: numpy writes each .npy → cajeta loads,
// verifies, and re-saves → real numpy re-verifies. Skips if python/numpy is absent.
TEST(NumpyOpsTests, npyNumpyInteropDtypes) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path();
    std::string script = (dir / "cajeta_npy_dtypes_harness.py").string();
    {
        std::ofstream f(script);
        f << "import numpy as np, sys\n"
             "mode, path, dt = sys.argv[1], sys.argv[2], sys.argv[3]\n"
             "dtypes = {'f8': np.float64, 'i4': np.int32, 'i8': np.int64}\n"
             "npdt = dtypes[dt]\n"
             "exp = np.array([[10, 20], [-3, 4], [7, 8]], dtype=npdt)\n"
             "if mode == 'write':\n"
             "    np.save(path, exp)\n"
             "else:\n"
             "    a = np.load(path)\n"
             "    ok = a.dtype == npdt and a.shape == (3, 2) and np.array_equal(a, exp)\n"
             "    sys.exit(0 if ok else 1)\n";
    }
    // Prefer $CAJETA_PYTHON, else `python3`. Probe numpy up front and skip cleanly if absent —
    // never reach the harness fork on a numpy-less box (a failed exec can corrupt the heap and
    // SIGABRT the following test).
    const char* pyEnv = std::getenv("CAJETA_PYTHON");
    std::string py = pyEnv ? std::string(pyEnv) : std::string("python3");
    if (std::system((py + " -c \"import numpy\"").c_str()) != 0) {
        GTEST_SKIP() << "python/numpy unavailable — skipping npio dtype interop";
    }
    auto run = [&](const std::string& mode, const std::string& path, const std::string& dt) {
        // POSIX-correct invocation (no `cmd /c` outer-quote wrap).
        std::string cmd = py + " \"" + script + "\" " + mode + " \"" + path + "\" " + dt;
        return std::system(cmd.c_str());
    };
    // dt tag → (loader, saver, cajeta element type). Same array [[10,20],[-3,4],[7,8]].
    struct Case { const char* tag; const char* loader; const char* saver; const char* ety; };
    std::vector<Case> cases = {
        {"f8", "loadF64", "saveF64", "float64"},
        {"i4", "loadI32", "saveI32", "int32"},
        {"i8", "loadI64", "saveI64", "int64"},
    };
    for (const Case& c : cases) {
        std::string fromNp = (dir / (std::string("cajeta_interop_from_np_") + c.tag + ".npy")).string();
        std::string toCj   = (dir / (std::string("cajeta_interop_to_cj_") + c.tag + ".npy")).string();
        ASSERT_EQ(run("write", fromNp, c.tag), 0) << "numpy harness failed to write dtype " << c.tag;
        std::string fromNpC = fromNp, toCjC = toCj;
        std::replace(fromNpC.begin(), fromNpC.end(), '\\', '/');
        std::replace(toCjC.begin(), toCjC.end(), '\\', '/');
        std::string src = std::string(PRE) +
            "import cajeta.math.npio.Npy;\n"
            "public final class D {\n"
            "    public static int32 run() {\n"
            "        Tensor<" + c.ety + "> a = Npy." + c.loader + "(\"" + fromNpC + "\");\n"
            "        if (a.ndim() != 2 || a.shapeAt(0) != 3 || a.shapeAt(1) != 2) { return -1; }\n"
            "        if (a.get2(0, 0) != 10 || a.get2(0, 1) != 20) { return -2; }\n"
            "        if (a.get2(1, 0) != -3 || a.get2(1, 1) != 4) { return -3; }\n"
            "        if (a.get2(2, 0) != 7 || a.get2(2, 1) != 8) { return -4; }\n"
            "        Npy." + c.saver + "(\"" + toCjC + "\", a);\n"
            "        return 1;\n"
            "    }\n"
            "}\n";
        EXPECT_EQ(runI32(src), 1) << "cajeta round-trip failed for dtype " << c.tag;
        EXPECT_EQ(run("verify", toCj, c.tag), 0) << "numpy re-verify failed for dtype " << c.tag;
    }
}

// 10.5 — npz: cajeta round-trips a multi-array, mixed-dtype `.npz` (write with NpzWriter,
// read each named member back with the typed Npz loaders).
TEST(NumpyOpsTests, npzRoundTrip) {
    std::string path = (std::filesystem::temp_directory_path() / "cajeta_npz_rt.npz").string();
    std::replace(path.begin(), path.end(), '\\', '/');
    std::string src = std::string(PRE) +
        "import cajeta.math.npio.Npz;\n"
        "import cajeta.math.npio.NpzWriter;\n"
        "public final class D {\n"
        "    public static boolean close(float32 a, float32 b) {\n"
        "        float32 d = a - b; if (d < 0.0f) { d = -d; } return d < 0.0001f;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        float32[] fa = [ 1.5f, -2.5f, 3.0f, 4.0f ];\n"
        "        int64[] sa = heap int64[2]; sa[0] = 2; sa[1] = 2;\n"
        "        Tensor<float32> ta = Tensor.of<float32>(fa, sa);\n"
        "        int32[] ib = [ 7, -3, 0, 42 ];\n"
        "        int64[] sb = heap int64[2]; sb[0] = 2; sb[1] = 2;\n"
        "        Tensor<int32> tb = Tensor.of<int32>(ib, sb);\n"
        "        int64[] ic = [ 100, 200, 300, 400 ];\n"
        "        int64[] sc = heap int64[2]; sc[0] = 2; sc[1] = 2;\n"
        "        Tensor<int64> tc = Tensor.of<int64>(ic, sc);\n"
        "        NpzWriter w = heap NpzWriter();\n"
        "        w.addF32(\"a\", ta);\n"
        "        w.addI32(\"b\", tb);\n"
        "        w.addI64(\"c\", tc);\n"
        "        w.save(\"" + path + "\");\n"
        "        Tensor<float32> ra = Npz.loadF32(\"" + path + "\", \"a\");\n"
        "        if (ra.ndim() != 2 || ra.shapeAt(0) != 2 || ra.shapeAt(1) != 2) { return -1; }\n"
        "        if (!D.close(ra.get2(0, 0), 1.5f) || !D.close(ra.get2(0, 1), -2.5f)) { return -2; }\n"
        "        if (!D.close(ra.get2(1, 0), 3.0f) || !D.close(ra.get2(1, 1), 4.0f)) { return -3; }\n"
        "        Tensor<int32> rb = Npz.loadI32(\"" + path + "\", \"b\");\n"
        "        if (rb.get2(0, 0) != 7 || rb.get2(0, 1) != -3) { return -4; }\n"
        "        if (rb.get2(1, 0) != 0 || rb.get2(1, 1) != 42) { return -5; }\n"
        "        Tensor<int64> rc = Npz.loadI64(\"" + path + "\", \"c\");\n"
        "        if (rc.get2(0, 0) != 100 || rc.get2(0, 1) != 200) { return -6; }\n"
        "        if (rc.get2(1, 0) != 300 || rc.get2(1, 1) != 400) { return -7; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 10.5 — npz numpy-interop: cajeta writes a mixed-dtype `.npz`, then REAL numpy `np.load`s
// it and verifies every member (dtype/shape/values). Proves the ZIP+CRC framing is valid,
// not merely self-consistent. Uses a plain (Linux-friendly) python invocation — unlike the
// older npy interop harness whose `cmd /c` outer-quote wrapping only works on Windows.
TEST(NumpyOpsTests, npzNumpyInterop) {
    namespace fs = std::filesystem;
    fs::path dir = fs::temp_directory_path();
    std::string npz    = (dir / "cajeta_npz_interop.npz").string();
    std::string script = (dir / "cajeta_npz_verify.py").string();
    {
        std::ofstream f(script);
        f << "import numpy as np, sys\n"
             "z = np.load(sys.argv[1])\n"
             "a, b, c = z['a'], z['b'], z['c']\n"
             "ok = (a.dtype == np.float32 and a.shape == (2, 2)\n"
             "      and np.allclose(a, [[1.5, -2.5], [3.0, 4.0]])\n"
             "      and b.dtype == np.int32 and np.array_equal(b, [[7, -3], [0, 42]])\n"
             "      and c.dtype == np.int64 and np.array_equal(c, [[100, 200], [300, 400]]))\n"
             "sys.exit(0 if ok else 1)\n";
    }
    const char* pyEnv = std::getenv("CAJETA_PYTHON");
    std::string py = pyEnv ? std::string(pyEnv) : std::string("python3");
    // Probe numpy availability; skip cleanly if absent.
    if (std::system((py + " -c \"import numpy\"").c_str()) != 0) {
        GTEST_SKIP() << "python/numpy unavailable — skipping npz interop";
    }
    std::string npzC = npz;
    std::replace(npzC.begin(), npzC.end(), '\\', '/');
    // 1. cajeta writes the .npz.
    std::string src = std::string(PRE) +
        "import cajeta.math.npio.NpzWriter;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float32[] fa = [ 1.5f, -2.5f, 3.0f, 4.0f ];\n"
        "        int64[] sa = heap int64[2]; sa[0] = 2; sa[1] = 2;\n"
        "        Tensor<float32> ta = Tensor.of<float32>(fa, sa);\n"
        "        int32[] ib = [ 7, -3, 0, 42 ];\n"
        "        int64[] sb = heap int64[2]; sb[0] = 2; sb[1] = 2;\n"
        "        Tensor<int32> tb = Tensor.of<int32>(ib, sb);\n"
        "        int64[] ic = [ 100, 200, 300, 400 ];\n"
        "        int64[] sc = heap int64[2]; sc[0] = 2; sc[1] = 2;\n"
        "        Tensor<int64> tc = Tensor.of<int64>(ic, sc);\n"
        "        NpzWriter w = heap NpzWriter();\n"
        "        w.addF32(\"a\", ta);\n"
        "        w.addI32(\"b\", tb);\n"
        "        w.addI64(\"c\", tc);\n"
        "        w.save(\"" + npzC + "\");\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
    // 2. real numpy reads cajeta's archive and asserts every member.
    std::string verify = py + " \"" + script + "\" \"" + npz + "\"";
    EXPECT_EQ(std::system(verify.c_str()), 0);
}

// 10.5 precursor — float<->raw-bits reinterpret intrinsics (Cajeta.f32ToBits/bitsToF32,
// f64ToBits/bitsToF64): LLVM bitcast, NOT a value conversion. Unblocks binary float .npy.
TEST(NumpyOpsTests, floatBitsIntrinsicRoundTrip) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // 1.0f IEEE-754 bits == 0x3F800000 == 1065353216 (a reinterpret, not (int32)1)
        "        int32 b = Cajeta.f32ToBits(1.0f);\n"
        "        if (b != 1065353216) { return -1; }\n"
        // bitsToF32 is the exact inverse
        "        float32 x = Cajeta.bitsToF32(b);\n"
        "        if (x != 1.0f) { return -2; }\n"
        // arbitrary value round-trips bit-exactly
        "        float32 y = 3.14159f;\n"
        "        int32 yb = Cajeta.f32ToBits(y);\n"
        "        float32 y2 = Cajeta.bitsToF32(yb);\n"
        "        if (y2 != y) { return -3; }\n"
        // negative value (sign bit set) round-trips
        "        float32 z = -42.5f;\n"
        "        float32 z2 = Cajeta.bitsToF32(Cajeta.f32ToBits(z));\n"
        "        if (z2 != z) { return -4; }\n"
        // f64 round-trip
        "        float64 dv = 2.5;\n"
        "        int64 db = Cajeta.f64ToBits(dv);\n"
        "        float64 dx = Cajeta.bitsToF64(db);\n"
        "        if (dx != dv) { return -5; }\n"
        // a (int32) cast CONVERTS (truncates) — must differ from the bit reinterpret
        "        int32 conv = (int32) 1.0f;\n"
        "        if (conv == 1065353216) { return -6; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 11 (deferred) — general nonsymmetric eigenvalues via Francis double-shift QR
// (elmhes + hqr). Complex eigenvalues emerge as conjugate pairs with real arithmetic only.
// [[2,-1,0],[1,2,0],[0,0,5]] → {2-i, 2+i, 5} (numpy eigvals, compared as a sorted set).
TEST(NumpyOpsTests, eigvalsNonsymmetricMatchNumpy) {
    std::string src = std::string(PRE) +
        "import cajeta.math.linalg.LinAlg;\n"
        "public final class D {\n"
        "    public static boolean close(float64 a, float64 b) {\n"
        "        float64 d = a - b; if (d < 0.0) { d = -d; } return d < 0.0001;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        float64[] dm = [ 2.0, -1.0, 0.0,  1.0, 2.0, 0.0,  0.0, 0.0, 5.0 ];\n"
        "        int64[] s33 = heap int64[2]; s33[0] = 3; s33[1] = 3;\n"
        "        Tensor<float64> a = Tensor.of<float64>(dm, s33);\n"
        "        Tensor<float64> ev = LinAlg.eigvals(a);\n"          // length 6 interleaved (re,im)
        "        if (ev.size() != 6) { return -9; }\n"
        "        float64[] re = heap float64[3];\n"
        "        float64[] im = heap float64[3];\n"
        "        int64 i = 0;\n"
        "        while (i < 3) { re[i] = ev.get1(2 * i); im[i] = ev.get1(2 * i + 1); i = i + 1; }\n"
        // insertion sort the (re, im) pairs by re then im
        "        int64 aa = 1;\n"
        "        while (aa < 3) {\n"
        "            float64 kr = re[aa]; float64 ki = im[aa];\n"
        "            int64 b = aa - 1; boolean placed = false;\n"
        "            while (b >= 0 && !placed) {\n"
        "                float64 br = re[b]; float64 bi = im[b];\n"
        "                boolean gt = (br > kr) || (br == kr && bi > ki);\n"
        "                if (gt) { re[b + 1] = re[b]; im[b + 1] = im[b]; b = b - 1; }\n"
        "                else { placed = true; }\n"
        "            }\n"
        "            re[b + 1] = kr; im[b + 1] = ki; aa = aa + 1;\n"
        "        }\n"
        // expected sorted: (2,-1), (2,+1), (5,0)
        "        float64 r0 = re[0]; float64 m0 = im[0];\n"
        "        if (!D.close(r0, 2.0) || !D.close(m0, -1.0)) { return -1; }\n"
        "        float64 r1 = re[1]; float64 m1 = im[1];\n"
        "        if (!D.close(r1, 2.0) || !D.close(m1, 1.0)) { return -2; }\n"
        "        float64 r2 = re[2]; float64 m2 = im[2];\n"
        "        if (!D.close(r2, 5.0) || !D.close(m2, 0.0)) { return -3; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 11 (deferred) — nonsymmetric eig on a DENSE matrix needing real QR convergence (not a
// trivial block read-off). [[1,2,3],[4,5,6],[7,8,10]] → {-0.9057, 0.1982, 16.7075} (all real).
TEST(NumpyOpsTests, eigvalsDenseRealMatchNumpy) {
    std::string src = std::string(PRE) +
        "import cajeta.math.linalg.LinAlg;\n"
        "public final class D {\n"
        "    public static boolean close(float64 a, float64 b) {\n"
        "        float64 d = a - b; if (d < 0.0) { d = -d; } return d < 0.001;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        float64[] dm = [ 1.0, 2.0, 3.0,  4.0, 5.0, 6.0,  7.0, 8.0, 10.0 ];\n"
        "        int64[] s33 = heap int64[2]; s33[0] = 3; s33[1] = 3;\n"
        "        Tensor<float64> a = Tensor.of<float64>(dm, s33);\n"
        "        Tensor<float64> ev = LinAlg.eigvals(a);\n"
        "        float64[] re = heap float64[3];\n"
        "        float64[] im = heap float64[3];\n"
        "        int64 i = 0;\n"
        "        while (i < 3) { re[i] = ev.get1(2 * i); im[i] = ev.get1(2 * i + 1); i = i + 1; }\n"
        "        int64 aa = 1;\n"
        "        while (aa < 3) {\n"
        "            float64 kr = re[aa]; float64 ki = im[aa];\n"
        "            int64 b = aa - 1; boolean placed = false;\n"
        "            while (b >= 0 && !placed) {\n"
        "                float64 br = re[b]; float64 bi = im[b];\n"
        "                boolean gt = (br > kr) || (br == kr && bi > ki);\n"
        "                if (gt) { re[b + 1] = re[b]; im[b + 1] = im[b]; b = b - 1; }\n"
        "                else { placed = true; }\n"
        "            }\n"
        "            re[b + 1] = kr; im[b + 1] = ki; aa = aa + 1;\n"
        "        }\n"
        "        float64 r0 = re[0]; float64 m0 = im[0];\n"
        "        if (!D.close(r0, -0.9057) || !D.close(m0, 0.0)) { return -1; }\n"
        "        float64 r1 = re[1]; float64 m1 = im[1];\n"
        "        if (!D.close(r1, 0.1982) || !D.close(m1, 0.0)) { return -2; }\n"
        "        float64 r2 = re[2]; float64 m2 = im[2];\n"
        "        if (!D.close(r2, 16.7075) || !D.close(m2, 0.0)) { return -3; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 11b — native symmetric eig (Jacobi) + SVD (via eigh of A^T A). Eigenvalues ascending,
// singular values descending; verified by spectrum + A·v=λ·v and U·diag(S)·Vt==A.
TEST(NumpyOpsTests, linalgEighSvdMatchNumpy) {
    std::string src = std::string(PRE) +
        "import cajeta.math.linalg.LinAlg;\n"
        "public final class D {\n"
        "    public static boolean close(float32 a, float32 b) {\n"
        "        float32 d = a - b; if (d < 0.0f) { d = -d; } return d < 0.003f;\n"
        "    }\n"
        "    public static int32 run() {\n"
        // ---- eigh: A=[[2,1],[1,2]] → eigenvalues [1,3] (ascending) ----
        "        float32[] da = [ 2.0f, 1.0f, 1.0f, 2.0f ];\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<float32> a = Tensor.of<float32>(da, s22);\n"
        "        Tensor<float32>[] eg = LinAlg.eigh<float32>(a);\n"
        "        Tensor<float32> w = eg[0];\n"
        "        Tensor<float32> v = eg[1];\n"
        "        if (!D.close(w.get1(0), 1.0f) || !D.close(w.get1(1), 3.0f)) { return -1; }\n"
        // A·v_k == λ_k·v_k for each eigenpair (column k of V)
        "        int64 k = 0;\n"
        "        while (k < 2) {\n"
        "            float32 lam = w.get1(k);\n"
        "            int64 r = 0;\n"
        "            while (r < 2) {\n"
        "                float32 av = 0.0f;\n"
        "                int64 c = 0;\n"
        "                while (c < 2) {\n"
        "                    av = av + a.get2(r, c) * v.get2(c, k);\n"
        "                    c = c + 1;\n"
        "                }\n"
        "                float32 lv = lam * v.get2(r, k);\n"
        "                if (!D.close(av, lv)) { return -2; }\n"
        "                r = r + 1;\n"
        "            }\n"
        "            k = k + 1;\n"
        "        }\n"
        // ---- svd: A2=[[1,2],[3,4]] → S≈[5.4650, 0.3660] (descending), U·diag(S)·Vt==A2 ----
        "        float32[] db = [ 1.0f, 2.0f, 3.0f, 4.0f ];\n"
        "        int64[] s22b = heap int64[2]; s22b[0] = 2; s22b[1] = 2;\n"
        "        Tensor<float32> a2 = Tensor.of<float32>(db, s22b);\n"
        "        Tensor<float32>[] sv = LinAlg.svd<float32>(a2);\n"
        "        Tensor<float32> u = sv[0];\n"
        "        Tensor<float32> sg = sv[1];\n"
        "        Tensor<float32> vt = sv[2];\n"
        "        if (!D.close(sg.get1(0), 5.4649858f) || !D.close(sg.get1(1), 0.3659662f)) { return -3; }\n"
        // reconstruct A2 = U·diag(S)·Vt
        "        int64 i = 0;\n"
        "        while (i < 2) {\n"
        "            int64 j = 0;\n"
        "            while (j < 2) {\n"
        "                float32 acc = 0.0f;\n"
        "                int64 t = 0;\n"
        "                while (t < 2) {\n"
        "                    acc = acc + u.get2(i, t) * sg.get1(t) * vt.get2(t, j);\n"
        "                    t = t + 1;\n"
        "                }\n"
        "                float32 orig = a2.get2(i, j);\n"
        "                if (!D.close(acc, orig)) { return -4; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 11b — native Cholesky: L·L^T = A for symmetric positive-definite A (lower triangular).
TEST(NumpyOpsTests, linalgCholeskyMatchNumpy) {
    std::string src = std::string(PRE) +
        "import cajeta.math.linalg.LinAlg;\n"
        "public final class D {\n"
        "    public static boolean close(float32 a, float32 b) {\n"
        "        float32 d = a - b; if (d < 0.0f) { d = -d; } return d < 0.002f;\n"
        "    }\n"
        "    public static int32 run() {\n"
        // A=[[4,2],[2,3]] → L=[[2,0],[1,sqrt(2)]]
        "        float32[] da = [ 4.0f, 2.0f, 2.0f, 3.0f ];\n"
        "        int64[] s22 = heap int64[2]; s22[0] = 2; s22[1] = 2;\n"
        "        Tensor<float32> a = Tensor.of<float32>(da, s22);\n"
        "        Tensor<float32> l = LinAlg.cholesky<float32>(a);\n"
        "        if (!D.close(l.get2(0, 0), 2.0f) || !D.close(l.get2(0, 1), 0.0f)) { return -1; }\n"
        "        if (!D.close(l.get2(1, 0), 1.0f) || !D.close(l.get2(1, 1), 1.4142136f)) { return -2; }\n"
        // 3x3 classic: A=[[4,12,-16],[12,37,-43],[-16,-43,98]] → L=[[2,0,0],[6,1,0],[-8,5,3]]
        "        float32[] db = [ 4.0f, 12.0f, -16.0f, 12.0f, 37.0f, -43.0f, -16.0f, -43.0f, 98.0f ];\n"
        "        int64[] s33 = heap int64[2]; s33[0] = 3; s33[1] = 3;\n"
        "        Tensor<float32> a3 = Tensor.of<float32>(db, s33);\n"
        "        Tensor<float32> l3 = LinAlg.cholesky<float32>(a3);\n"
        "        if (!D.close(l3.get2(0, 0), 2.0f) || !D.close(l3.get2(1, 0), 6.0f) || !D.close(l3.get2(1, 1), 1.0f)) { return -3; }\n"
        "        if (!D.close(l3.get2(2, 0), -8.0f) || !D.close(l3.get2(2, 1), 5.0f) || !D.close(l3.get2(2, 2), 3.0f)) { return -4; }\n"
        // upper triangle is zero
        "        if (!D.close(l3.get2(0, 1), 0.0f) || !D.close(l3.get2(0, 2), 0.0f) || !D.close(l3.get2(1, 2), 0.0f)) { return -5; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
