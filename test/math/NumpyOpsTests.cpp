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
