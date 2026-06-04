//
// Array allocation, indexing, and size() tests. Verifies the Java-style nested
// model: `T[]` is a pointer to a `{ i64 size, [0 x T] data }` header, `T[][]` is
// the same shape with element type being another header pointer.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "../DeathTestUtil.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& returnType, const std::string& body) {
    return "package test;\n"
           "public final class A {\n"
           "    public static " + returnType + " run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

// --- new + index + assignment --------------------------------------------------

TEST(ArrayTests, allocateAndReadDefault) {
    // calloc-backed new means the buffer starts zeroed.
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32[] arr = new int32[5];\n"
        "return arr[0];"), "test.A");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 0);
}

TEST(ArrayTests, writeThenRead) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32[] arr = new int32[5];\n"
        "arr[2] = 42;\n"
        "return arr[2];"), "test.A");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

TEST(ArrayTests, multipleSlotsIndependent) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32[] arr = new int32[3];\n"
        "arr[0] = 10;\n"
        "arr[1] = 20;\n"
        "arr[2] = 30;\n"
        "return arr[0] + arr[1] + arr[2];"), "test.A");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 60);
}

TEST(ArrayTests, indexFromVariable) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32[] arr = new int32[10];\n"
        "int32 idx = 7;\n"
        "arr[idx] = 99;\n"
        "return arr[idx];"), "test.A");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 99);
}

TEST(ArrayTests, doubleElement) {
    auto jit = CajetaJit::compile(makeSource("float64",
        "float64[] arr = new float64[3];\n"
        "arr[0] = 1.5;\n"
        "arr[1] = 2.5;\n"
        "arr[2] = 4.25;\n"
        "return arr[0] + arr[1] + arr[2];"), "test.A");
    auto fn = jit->lookup<double (*)()>("run");
    EXPECT_DOUBLE_EQ(fn(), 8.25);
}

// --- count() -------------------------------------------------------------------

TEST(ArrayTests, sizeMatchesAllocation) {
    auto jit = CajetaJit::compile(makeSource("int64",
        "int32[] arr = new int32[42];\n"
        "return arr.count();"), "test.A");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 42);
}

TEST(ArrayTests, sizeOfDifferentElementType) {
    auto jit = CajetaJit::compile(makeSource("int64",
        "float64[] arr = new float64[7];\n"
        "return arr.count();"), "test.A");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 7);
}

TEST(ArrayTests, sizeUsedInComparison) {
    // WhileStatement codegen is still a stub (separate from array work); verify
    // count() in expression position without a loop.
    auto jit = CajetaJit::compile(makeSource("boolean",
        "int32[] arr = new int32[5];\n"
        "int64 expected = 5;\n"
        "return arr.count() == expected;"), "test.A");
    auto fn = jit->lookup<bool (*)()>("run");
    EXPECT_TRUE(fn());
}

// --- bounds check --------------------------------------------------------------
//
// The bounds-fail helper aborts the process. Use death-test pattern so gtest
// runs the body in a forked child and verifies it died.

TEST(ArrayTests, boundsCheckFiresOnOutOfRange) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32[] arr = new int32[3];\n"
        "return arr[5];"), "test.A");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    // Aborts via __cajeta_array_bounds_fail.
    EXPECT_EXIT(fn(), CAJETA_DIED_BY_ABORT, "out of bounds");
}

TEST(ArrayTests, boundsCheckFiresOnNegativeIndex) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32[] arr = new int32[3];\n"
        "int32 idx = -1;\n"
        "return arr[idx];"), "test.A");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EXIT(fn(), CAJETA_DIED_BY_ABORT, "out of bounds");
}

// --- nested arrays (multi-dim) ------------------------------------------------
//
// `int32[][]` is `array-of-array-references`. `new int32[2][3]` allocates the
// outer of 2 inner-array refs, then each of 2 inners of 3 ints each.

TEST(ArrayTests, twoDimAllocateAndRead) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32[][] arr = new int32[2][3];\n"
        "arr[1][2] = 77;\n"
        "return arr[1][2];"), "test.A");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 77);
}

TEST(ArrayTests, twoDimRowsIndependent) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32[][] arr = new int32[2][3];\n"
        "arr[0][0] = 1;\n"
        "arr[0][1] = 2;\n"
        "arr[1][0] = 10;\n"
        "arr[1][1] = 20;\n"
        "return arr[0][0] + arr[0][1] + arr[1][0] + arr[1][1];"), "test.A");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 33);
}

TEST(ArrayTests, twoDimSizeOfOuter) {
    auto jit = CajetaJit::compile(makeSource("int64",
        "int32[][] arr = new int32[5][3];\n"
        "return arr.count();"), "test.A");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 5);
}

TEST(ArrayTests, twoDimSizeOfInner) {
    auto jit = CajetaJit::compile(makeSource("int64",
        "int32[][] arr = new int32[5][3];\n"
        "return arr[0].count();"), "test.A");
    auto fn = jit->lookup<int64_t (*)()>("run");
    EXPECT_EQ(fn(), 3);
}

// --- field-read as array dimension (l-value → r-value coercion) ----------------
// Regression: `new T[Klass.STATIC]` / `new T[this.field]` mis-lowered the
// dimension. A static-final field read returns the GlobalVariable, an instance
// field read returns a struct GEP — neither is an AllocaInst, so the old
// alloca-only load in ArrayCreatorRest left the dimension a `ptr`, and the
// CreateIntCast below it sext'd a pointer → "SExt only operates on integer"
// IR-verify failure. Now routed through loadIfLValue, mirroring the same fix
// ArrayIndexExpression needed (Expression.cpp). Surfaced by AsyncWriter's
// `new int8[AsyncWriter.DEFAULT_BUFFER]` and AsyncReader's `new int8[this.chunkSize]`.

TEST(ArrayTests, staticFinalFieldAsArrayDimension) {
    auto src =
        "package test;\n"
        "public final class Cfg {\n"
        "    public static final int32 CAP = 6;\n"
        "}\n"
        "public final class A {\n"
        "    public static int64 run() {\n"
        "        int32[] arr = new int32[Cfg.CAP];\n"
        "        return arr.count();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.A");
    auto fn = jit->lookup<int64_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 6);
}

TEST(ArrayTests, instanceFieldAsArrayDimension) {
    auto src =
        "package test;\n"
        "public final class Buf {\n"
        "    public int32 chunkSize;\n"
        "    public Buf(int32 n) { this.chunkSize = n; }\n"
        "    public int32 make() {\n"
        "        int32[] arr = new int32[this.chunkSize];\n"
        "        arr[this.chunkSize - 1] = 7;\n"
        "        return arr[this.chunkSize - 1];\n"
        "    }\n"
        "}\n"
        "public final class A {\n"
        "    public static int32 run() {\n"
        "        Buf b = new Buf(4);\n"
        "        return b.make();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.A");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 7);
}
