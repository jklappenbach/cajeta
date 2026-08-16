//
// cajeta-ml-v2 U7 (7.1.x) — `cajeta.nucleo.sparse.CsrMatrix`: the float64
// CSR type whose first consumer is the sparse coordinate-descent path in
// dev.cajeta.ml (table-fit doctrine: types + primitives in the stdlib,
// algorithms external). Round-trips, SpMV/SpMVT against dense arithmetic,
// and the CSC mirror invariants the cd solver leans on.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kPrelude =
    "package test;\n"
    "import cajeta.math.Tensor;\n"
    "import cajeta.nucleo.sparse.CsrMatrix;\n";

// 7.1.1 — fromDense → toDense identity; fromCoo with unsorted, duplicate
// entries (summed, scipy's rule) equals the same dense matrix.
TEST(CsrMatrixTests, roundTripsAndCooSemantics) {
    std::string src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float64[] dd = [ 1.0, 0.0, 2.0,\n"
        "                         0.0, 0.0, 0.0,\n"
        "                         3.0, 4.0, 0.0,\n"
        "                         0.0, 5.0, 6.0 ];\n"
        "        int64[] sh = heap int64[2];\n"
        "        sh[0] = 4;\n"
        "        sh[1] = 3;\n"
        "        Tensor<float64> dm #= Tensor.of<float64>(dd, sh);\n"
        "        CsrMatrix a #= CsrMatrix.fromDense(dm);\n"
        "        if (a.rowCount() != 4 || a.colCount() != 3) { return 1; }\n"
        "        if (a.nnz() != 6) { return 2; }\n"
        "        Tensor<float64> back #= a.toDense();\n"
        "        int64 i = 0;\n"
        "        while (i < 4) {\n"
        "            int64 j = 0;\n"
        "            while (j < 3) {\n"
        "                if (back.get2(i, j) != dm.get2(i, j)) { return 3; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        // COO: unsorted, with (2,0) split into two duplicates 1+2.\n"
        "        int64[] rr = [ 3, 0, 2, 0, 2, 3, 2 ];\n"
        "        int64[] cc = [ 2, 2, 1, 0, 0, 1, 0 ];\n"
        "        float64[] vv = [ 6.0, 2.0, 4.0, 1.0, 1.0, 5.0, 2.0 ];\n"
        "        CsrMatrix b #= CsrMatrix.fromCoo(rr, cc, vv, 7, 4, 3);\n"
        "        if (b.nnz() != 6) { return 4; }\n"
        "        Tensor<float64> bd #= b.toDense();\n"
        "        i = 0;\n"
        "        while (i < 4) {\n"
        "            int64 j = 0;\n"
        "            while (j < 3) {\n"
        "                if (bd.get2(i, j) != dm.get2(i, j)) { return 5; }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 42;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 42);
}

// 7.1.2 — SpMV and SpMVT match dense matmul on a deterministic pattern.
TEST(CsrMatrixTests, spmvMatchesDense) {
    std::string src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 n = 6;\n"
        "        int64 p = 5;\n"
        "        int64[] sh = heap int64[2];\n"
        "        sh[0] = n;\n"
        "        sh[1] = p;\n"
        "        Tensor<float64> dm #= Tensor.zeros<float64>(sh);\n"
        "        int64 i = 0;\n"
        "        while (i < n) {\n"
        "            int64 j = 0;\n"
        "            while (j < p) {\n"
        "                if ((i * 3 + j * 2) % 4 == 0) {\n"
        "                    dm.flatSet(i * p + j, (float64) (i + 1) * 0.5\n"
        "                        - (float64) j * 0.25);\n"
        "                }\n"
        "                j = j + 1;\n"
        "            }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        CsrMatrix a #= CsrMatrix.fromDense(dm);\n"
        "        float64[] xv = heap float64[p];\n"
        "        int64 j2 = 0;\n"
        "        while (j2 < p) { xv[j2] = (float64) j2 - 1.5; j2 = j2 + 1; }\n"
        "        int64[] ps = heap int64[1];\n"
        "        ps[0] = p;\n"
        "        Tensor<float64> x #= Tensor.of<float64>(xv, ps);\n"
        "        Tensor<float64> yv #= a.matVec(x);\n"
        "        i = 0;\n"
        "        while (i < n) {\n"
        "            float64 want = 0.0;\n"
        "            int64 j3 = 0;\n"
        "            while (j3 < p) {\n"
        "                want = want + dm.get2(i, j3) * x.get1(j3);\n"
        "                j3 = j3 + 1;\n"
        "            }\n"
        "            if (yv.get1(i) != want) { return 1; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        float64[] uv = heap float64[n];\n"
        "        i = 0;\n"
        "        while (i < n) { uv[i] = (float64) i * 0.3 + 0.1; i = i + 1; }\n"
        "        int64[] ns = heap int64[1];\n"
        "        ns[0] = n;\n"
        "        Tensor<float64> u #= Tensor.of<float64>(uv, ns);\n"
        "        Tensor<float64> tv #= a.matVecT(u);\n"
        "        int64 j4 = 0;\n"
        "        while (j4 < p) {\n"
        "            float64 want = 0.0;\n"
        "            i = 0;\n"
        "            while (i < n) {\n"
        "                want = want + dm.get2(i, j4) * u.get1(i);\n"
        "                i = i + 1;\n"
        "            }\n"
        "            if (tv.get1(j4) != want) { return 2; }\n"
        "            j4 = j4 + 1;\n"
        "        }\n"
        "        return 42;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 42);
}

// 7.1.3 — CSC mirror: per-column walks cover exactly the nonzeros, in
// ascending row order; colNorm2 matches the dense column norm.
TEST(CsrMatrixTests, cscMirrorInvariants) {
    std::string src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float64[] dd = [ 1.0, 0.0, 2.0,\n"
        "                         0.0, 0.0, 0.0,\n"
        "                         3.0, 4.0, 0.0,\n"
        "                         0.0, 5.0, 6.0 ];\n"
        "        int64[] sh = heap int64[2];\n"
        "        sh[0] = 4;\n"
        "        sh[1] = 3;\n"
        "        Tensor<float64> dm #= Tensor.of<float64>(dd, sh);\n"
        "        CsrMatrix a #= CsrMatrix.fromDense(dm);\n"
        "        a.buildCsc();\n"
        "        int64 j = 0;\n"
        "        while (j < 3) {\n"
        "            float64 norm = 0.0;\n"
        "            int64 prev = -1;\n"
        "            int64 t = a.colStart(j);\n"
        "            while (t < a.colEnd(j)) {\n"
        "                int64 r = a.cscRowAt(t);\n"
        "                float64 v = a.cscValAt(t);\n"
        "                if (r <= prev) { return 1; }\n"
        "                prev = r;\n"
        "                if (v != dm.get2(r, j)) { return 2; }\n"
        "                norm = norm + v * v;\n"
        "                t = t + 1;\n"
        "            }\n"
        "            float64 want = 0.0;\n"
        "            int64 i = 0;\n"
        "            while (i < 4) {\n"
        "                want = want + dm.get2(i, j) * dm.get2(i, j);\n"
        "                i = i + 1;\n"
        "            }\n"
        "            if (a.colNorm2(j) != want || norm != want) { return 3; }\n"
        "            j = j + 1;\n"
        "        }\n"
        "        return 42;\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 42);
}

} // namespace
