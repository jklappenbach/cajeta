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
