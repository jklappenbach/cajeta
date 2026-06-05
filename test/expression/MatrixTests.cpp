//
// B1 — Matrix<T, R, C> as a hybrid operator-overloaded value type
// (plans/fluttering-sparking-lantern.md). Host JIT coverage.
//
// S1 (this stage): TYPE RESOLUTION + LLVM-type layout. A `Matrix<T, R, C>`
// reference resolves to the flat row-major CajetaMatrix representation
// (`<R*C x T>`), distinct per (T, R, C), admitted only for non-bool numeric T
// and positive constant R, C. Resolution + layout are forced the same way the
// Vector tests do it — wrapping the matrix as a Buffer<T> element so
// Buffer<Matrix<...>> monomorphizes and lays out the matrix's LLVM type; the
// 2-arg Buffer ctor stores a handle/count without touching a device, so it runs
// under the host JIT. Construction / m[r][c] / arithmetic / matmul land in the
// later stages (S2+).
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"
#include "../../src/cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

float runF32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<float (*)()>("run");
    return fn();
}

const char* IMPORTS =
    "import cajeta.xpu.core.Buffer;\n";

} // namespace

// Matrix<float32,2,3> resolves and a Buffer over it instantiates + lays out
// (the flat <6 x float> representation is materialized as the element type).
TEST(MatrixTests, typeResolvesAsBufferElement) {
    std::string src = std::string("package test;\n") + IMPORTS +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Buffer<Matrix<float32,2,3>> b = heap Buffer<Matrix<float32,2,3>>(0, 5);\n"
        "        return (int32) b.length();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// Distinct (T,R,C) shapes are distinct types and all lay out — f32 2x3,
// i32 3x2, and a repeat of f32 2x3 (cache hit on the same canonical key).
TEST(MatrixTests, typeMultipleShapesResolveDistinctly) {
    std::string src = std::string("package test;\n") + IMPORTS +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Buffer<Matrix<float32,2,3>> a = heap Buffer<Matrix<float32,2,3>>(0, 2);\n"
        "        Buffer<Matrix<int32,3,2>>   b = heap Buffer<Matrix<int32,3,2>>(0, 3);\n"
        "        Buffer<Matrix<float32,2,3>> c = heap Buffer<Matrix<float32,2,3>>(0, 5);\n"
        "        return (int32)(a.length() + b.length() + c.length());\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 10);
}

// A square shape resolves too (R == C), exercised separately because identity/
// transpose specialize on it later.
TEST(MatrixTests, typeSquareShapeResolves) {
    std::string src = std::string("package test;\n") + IMPORTS +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Buffer<Matrix<float32,4,4>> b = heap Buffer<Matrix<float32,4,4>>(0, 7);\n"
        "        return (int32) b.length();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// Non-numeric element type (a user class) is rejected.
TEST(MatrixTests, typeNonNumericElementRejected) {
    std::string src = std::string("package test;\n") + IMPORTS +
        "public class Thing { public Thing() { return; } }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Buffer<Matrix<Thing,2,2>> b = heap Buffer<Matrix<Thing,2,2>>(0, 1);\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_MATRIX_ELEMENT_TYPE";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_MATRIX_ELEMENT_TYPE");
    }
}

// A bool element type is rejected (matrices are numeric only).
TEST(MatrixTests, typeBooleanElementRejected) {
    std::string src = std::string("package test;\n") + IMPORTS +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Buffer<Matrix<boolean,2,2>> b = heap Buffer<Matrix<boolean,2,2>>(0, 1);\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_MATRIX_ELEMENT_TYPE";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_MATRIX_ELEMENT_TYPE");
    }
}

// ---- S2/S3: construction + 2D element access (m[r][c] read/write) ----------

// Construct a 2x3 matrix row-major and read elements back with m[r][c]. The
// row-major lane mapping element (r,c) = lane r*C+c is the contract.
TEST(MatrixTests, constructAndIndexReadRowMajor) {
    // [ 1 2 3 ]
    // [ 4 5 6 ]
    std::string src = std::string("package test;\n") +
        "public final class D {\n"
        "    public static float32 run() {\n"
        "        Matrix<float32,2,3> m = stack Matrix<float32,2,3>(\n"
        "            1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f);\n"
        "        return m[0][0] + m[0][2] * 10.0f + m[1][1] * 100.0f;\n"
        "    }\n"
        "}\n";
    // 1 + 3*10 + 5*100 = 531
    EXPECT_FLOAT_EQ(runF32(src), 531.0f);
}

// m[r][c] write updates only the addressed element and persists in the slot.
TEST(MatrixTests, indexWriteUpdatesElement) {
    std::string src = std::string("package test;\n") +
        "public final class D {\n"
        "    public static float32 run() {\n"
        "        Matrix<float32,2,3> m = stack Matrix<float32,2,3>(\n"
        "            1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f);\n"
        "        m[1][2] = 60.0f;\n"
        "        m[0][0] = 9.0f;\n"
        "        return m[0][0] + m[1][2] + m[0][1];\n"
        "    }\n"
        "}\n";
    // 9 + 60 + 2 = 71  (m[0][1] untouched = 2)
    EXPECT_FLOAT_EQ(runF32(src), 71.0f);
}

// A row m[r] is a Vector<T,C>; indexing it once more reads the element, and the
// dynamic-index form works (runtime r, c).
TEST(MatrixTests, dynamicIndexRead) {
    std::string src = std::string("package test;\n") +
        "public final class D {\n"
        "    public static float32 run() {\n"
        "        Matrix<float32,3,2> m = stack Matrix<float32,3,2>(\n"
        "            1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f);\n"
        "        int32 r = 2;\n"
        "        int32 c = 1;\n"
        "        return m[r][c];\n"   // element (2,1) = lane 2*2+1 = 5 -> 6.0
        "    }\n"
        "}\n";
    EXPECT_FLOAT_EQ(runF32(src), 6.0f);
}

// Wrong constructor argument count is a clean diagnostic.
TEST(MatrixTests, constructWrongArgCountRejected) {
    std::string src = std::string("package test;\n") +
        "public final class D {\n"
        "    public static float32 run() {\n"
        "        Matrix<float32,2,2> m = stack Matrix<float32,2,2>(1.0f, 2.0f);\n"
        "        return m[0][0];\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_MATRIX_CONSTRUCT";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_MATRIX_CONSTRUCT");
    }
}

// ---- S4: element-wise + - /, scalar scale, == --------------------------------

// Element-wise add of two same-shape matrices.
TEST(MatrixTests, elementwiseAdd) {
    std::string src = std::string("package test;\n") +
        "public final class D {\n"
        "    public static float32 run() {\n"
        "        Matrix<float32,2,2> a = stack Matrix<float32,2,2>(1.0f, 2.0f, 3.0f, 4.0f);\n"
        "        Matrix<float32,2,2> b = stack Matrix<float32,2,2>(10.0f, 20.0f, 30.0f, 40.0f);\n"
        "        Matrix<float32,2,2> c = a + b;\n"
        "        return c[0][0] + c[1][1];\n"   // 11 + 44 = 55
        "    }\n"
        "}\n";
    EXPECT_FLOAT_EQ(runF32(src), 55.0f);
}

// Element-wise subtract and divide.
TEST(MatrixTests, elementwiseSubDiv) {
    std::string src = std::string("package test;\n") +
        "public final class D {\n"
        "    public static float32 run() {\n"
        "        Matrix<float32,2,2> a = stack Matrix<float32,2,2>(10.0f, 20.0f, 30.0f, 40.0f);\n"
        "        Matrix<float32,2,2> b = stack Matrix<float32,2,2>(2.0f, 4.0f, 5.0f, 8.0f);\n"
        "        Matrix<float32,2,2> d = a - b;\n"   // [8 16 25 32]
        "        Matrix<float32,2,2> q = a / b;\n"   // [5 5 6 5]
        "        return d[0][1] + q[1][0];\n"        // 16 + 6 = 22
        "    }\n"
        "}\n";
    EXPECT_FLOAT_EQ(runF32(src), 22.0f);
}

// Scalar scale via `m * s`.
TEST(MatrixTests, scalarScale) {
    std::string src = std::string("package test;\n") +
        "public final class D {\n"
        "    public static float32 run() {\n"
        "        Matrix<float32,2,2> a = stack Matrix<float32,2,2>(1.0f, 2.0f, 3.0f, 4.0f);\n"
        "        Matrix<float32,2,2> s = a * 3.0f;\n"
        "        return s[0][0] + s[1][1];\n"   // 3 + 12 = 15
        "    }\n"
        "}\n";
    EXPECT_FLOAT_EQ(runF32(src), 15.0f);
}

// `==` is a per-lane mask (value-type comparison rule); whole-matrix equality
// is `(a == b).all()`. `any()` is the existence dual.
TEST(MatrixTests, equalsMaskAllAny) {
    std::string src = std::string("package test;\n") +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Matrix<float32,2,2> a = stack Matrix<float32,2,2>(1.0f, 2.0f, 3.0f, 4.0f);\n"
        "        Matrix<float32,2,2> b = stack Matrix<float32,2,2>(1.0f, 2.0f, 3.0f, 4.0f);\n"
        "        Matrix<float32,2,2> c = stack Matrix<float32,2,2>(1.0f, 2.0f, 3.0f, 9.0f);\n"
        "        int32 eqAll = (a == b).all() ? 1 : 0;\n"   // all 4 lanes equal -> 1
        "        int32 neAll = (a == c).all() ? 1 : 0;\n"   // lane (1,1) differs -> 0
        "        int32 neAny = (a == c).any() ? 1 : 0;\n"   // 3 lanes equal -> 1
        "        return eqAll * 100 + neAll * 10 + neAny;\n"   // 100 + 0 + 1 = 101
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 101);
}

// Ordering comparison with a broadcast scalar -> mask; select prunes small
// weights. w > 1: (F,T,F,F) -> select(w, 0) keeps only the large weight = 2.
TEST(MatrixTests, thresholdScalarMaskSelect) {
    std::string src = std::string("package test;\n") +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Matrix<float32,2,2> w = stack Matrix<float32,2,2>(0.1f, 2.0f, -3.0f, 0.05f);\n"
        "        Matrix<float32,2,2> z = stack Matrix<float32,2,2>(0.0f, 0.0f, 0.0f, 0.0f);\n"
        "        Matrix<float32,2,2> pruned = (w > 1.0f).select(w, z);\n"
        "        return (int32)(pruned[0][0] + pruned[0][1] + pruned[1][0] + pruned[1][1]);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 2);   // (0,2,0,0)
}

// select on a matrix mask: pick element-wise between two matrices.
//   mask = (a == b) (lanes: 1,1,1,0);  m = mask.select(a, c)
//   -> a where equal, c where not = [1 2; 3 9] -> m[1][1] = 9.
TEST(MatrixTests, maskSelect) {
    std::string src = std::string("package test;\n") +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Matrix<float32,2,2> a = stack Matrix<float32,2,2>(1.0f, 2.0f, 3.0f, 4.0f);\n"
        "        Matrix<float32,2,2> b = stack Matrix<float32,2,2>(1.0f, 2.0f, 3.0f, 4.0f);\n"
        "        Matrix<float32,2,2> c = stack Matrix<float32,2,2>(5.0f, 6.0f, 7.0f, 9.0f);\n"
        "        Matrix<float32,2,2> m = (a == b).select(a, c);\n"
        "        return (int32) m[1][1];\n"   // lane equal -> a's 4? no: a==b all equal -> a -> 4
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 4);
}

// ---- S5: * = matrix multiply + matrix-vector ---------------------------------

// 2x3 * 3x2 -> 2x2 matrix multiply.
TEST(MatrixTests, matrixMultiply) {
    std::string src = std::string("package test;\n") +
        "public final class D {\n"
        "    public static float32 run() {\n"
        // A = [1 2 3; 4 5 6]  B = [7 8; 9 10; 11 12]
        // AB = [58 64; 139 154]
        "        Matrix<float32,2,3> a = stack Matrix<float32,2,3>(1.0f,2.0f,3.0f,4.0f,5.0f,6.0f);\n"
        "        Matrix<float32,3,2> b = stack Matrix<float32,3,2>(7.0f,8.0f,9.0f,10.0f,11.0f,12.0f);\n"
        "        Matrix<float32,2,2> c = a * b;\n"
        "        return c[0][0] + c[0][1] * 1000.0f + c[1][0] + c[1][1] * 1000.0f;\n"
        // 58 + 64000 + 139 + 154000 = 218197
        "    }\n"
        "}\n";
    EXPECT_FLOAT_EQ(runF32(src), 218197.0f);
}

// Matrix * Vector -> Vector. A(2x3) * v(3) -> (2).
TEST(MatrixTests, matrixVectorMultiply) {
    std::string src = std::string("package test;\n") +
        "public final class D {\n"
        "    public static float32 run() {\n"
        // A = [1 2 3; 4 5 6]  v = [1 2 3]  Av = [14 32]
        "        Matrix<float32,2,3> a = stack Matrix<float32,2,3>(1.0f,2.0f,3.0f,4.0f,5.0f,6.0f);\n"
        "        Vector<float32,3> v = stack Vector<float32,3>(1.0f,2.0f,3.0f);\n"
        "        Vector<float32,2> r = a * v;\n"
        "        return r[0] + r[1] * 1000.0f;\n"   // 14 + 32000 = 32014
        "    }\n"
        "}\n";
    EXPECT_FLOAT_EQ(runF32(src), 32014.0f);
}

// Matmul shape mismatch (inner dims differ) is a clean diagnostic.
TEST(MatrixTests, matmulShapeMismatchRejected) {
    std::string src = std::string("package test;\n") +
        "public final class D {\n"
        "    public static float32 run() {\n"
        "        Matrix<float32,2,3> a = stack Matrix<float32,2,3>(1.0f,2.0f,3.0f,4.0f,5.0f,6.0f);\n"
        "        Matrix<float32,2,2> b = stack Matrix<float32,2,2>(1.0f,2.0f,3.0f,4.0f);\n"
        "        Matrix<float32,2,2> c = a * b;\n"   // 3 != 2
        "        return c[0][0];\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_MATRIX_SHAPE";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_MATRIX_SHAPE");
    }
}

// ---- S4: methods transpose / identity / row / col / hadamard -----------------

// transpose() of a 2x3 -> 3x2: element (i,j) becomes (j,i).
TEST(MatrixTests, transpose) {
    std::string src = std::string("package test;\n") +
        "public final class D {\n"
        "    public static float32 run() {\n"
        // A = [1 2 3; 4 5 6]  A^T = [1 4; 2 5; 3 6]
        "        Matrix<float32,2,3> a = stack Matrix<float32,2,3>(1.0f,2.0f,3.0f,4.0f,5.0f,6.0f);\n"
        "        Matrix<float32,3,2> t = a.transpose();\n"
        "        return t[0][1] + t[2][0] * 100.0f;\n"   // 4 + 3*100 = 304
        "    }\n"
        "}\n";
    EXPECT_FLOAT_EQ(runF32(src), 304.0f);
}

// identity() of a square matrix is 1 on the diagonal, 0 elsewhere.
TEST(MatrixTests, identity) {
    std::string src = std::string("package test;\n") +
        "public final class D {\n"
        "    public static float32 run() {\n"
        "        Matrix<float32,3,3> a = stack Matrix<float32,3,3>(\n"
        "            9.0f,9.0f,9.0f,9.0f,9.0f,9.0f,9.0f,9.0f,9.0f);\n"
        "        Matrix<float32,3,3> i = a.identity();\n"
        "        return i[0][0] + i[1][1] + i[2][2] + i[0][1] + i[2][0];\n"  // 3 + 0 = 3
        "    }\n"
        "}\n";
    EXPECT_FLOAT_EQ(runF32(src), 3.0f);
}

// row(r) and col(c) extract a Vector view.
TEST(MatrixTests, rowAndCol) {
    std::string src = std::string("package test;\n") +
        "public final class D {\n"
        "    public static float32 run() {\n"
        // A = [1 2 3; 4 5 6]
        "        Matrix<float32,2,3> a = stack Matrix<float32,2,3>(1.0f,2.0f,3.0f,4.0f,5.0f,6.0f);\n"
        "        Vector<float32,3> r1 = a.row(1);\n"   // [4 5 6]
        "        Vector<float32,2> c2 = a.col(2);\n"   // [3 6]
        "        return r1[0] + c2[1] * 100.0f;\n"     // 4 + 6*100 = 604
        "    }\n"
        "}\n";
    EXPECT_FLOAT_EQ(runF32(src), 604.0f);
}

// hadamard(b) is the element-wise product (since * is matmul).
TEST(MatrixTests, hadamard) {
    std::string src = std::string("package test;\n") +
        "public final class D {\n"
        "    public static float32 run() {\n"
        "        Matrix<float32,2,2> a = stack Matrix<float32,2,2>(1.0f,2.0f,3.0f,4.0f);\n"
        "        Matrix<float32,2,2> b = stack Matrix<float32,2,2>(10.0f,20.0f,30.0f,40.0f);\n"
        "        Matrix<float32,2,2> h = a.hadamard(b);\n"
        "        return h[0][0] + h[1][1];\n"   // 10 + 160 = 170
        "    }\n"
        "}\n";
    EXPECT_FLOAT_EQ(runF32(src), 170.0f);
}

// ---- S7: diagnostics ---------------------------------------------------------

// A zero dimension is rejected at construction.
TEST(MatrixTests, zeroDimensionRejected) {
    std::string src = std::string("package test;\n") +
        "public final class D {\n"
        "    public static float32 run() {\n"
        "        Matrix<float32,0,2> m = stack Matrix<float32,0,2>();\n"
        "        return 0.0f;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_MATRIX_DIMENSIONS";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_MATRIX_DIMENSIONS");
    }
}

// An unknown method on a matrix is a clean diagnostic.
TEST(MatrixTests, unknownMethodRejected) {
    std::string src = std::string("package test;\n") +
        "public final class D {\n"
        "    public static float32 run() {\n"
        "        Matrix<float32,2,2> m = stack Matrix<float32,2,2>(1.0f,2.0f,3.0f,4.0f);\n"
        "        return m.inverse();\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_MATRIX_METHOD";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_MATRIX_METHOD");
    }
}

// identity() on a non-square matrix is rejected.
TEST(MatrixTests, identityNonSquareRejected) {
    std::string src = std::string("package test;\n") +
        "public final class D {\n"
        "    public static float32 run() {\n"
        "        Matrix<float32,2,3> m = stack Matrix<float32,2,3>(1.0f,2.0f,3.0f,4.0f,5.0f,6.0f);\n"
        "        Matrix<float32,2,3> i = m.identity();\n"
        "        return i[0][0];\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_MATRIX_METHOD";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_MATRIX_METHOD");
    }
}

// Element-wise op on mismatched shapes is rejected.
TEST(MatrixTests, elementwiseShapeMismatchRejected) {
    std::string src = std::string("package test;\n") +
        "public final class D {\n"
        "    public static float32 run() {\n"
        "        Matrix<float32,2,2> a = stack Matrix<float32,2,2>(1.0f,2.0f,3.0f,4.0f);\n"
        "        Matrix<float32,2,3> b = stack Matrix<float32,2,3>(1.0f,2.0f,3.0f,4.0f,5.0f,6.0f);\n"
        "        Matrix<float32,2,2> c = a + b;\n"
        "        return c[0][0];\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected CAJETA_ERROR_MATRIX_SHAPE";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_MATRIX_SHAPE");
    }
}
