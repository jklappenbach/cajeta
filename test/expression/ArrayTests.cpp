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






// --- count() -------------------------------------------------------------------




// --- bounds check --------------------------------------------------------------
//
// The bounds-fail helper aborts the process. Use death-test pattern so gtest
// runs the body in a forked child and verifies it died.



// --- nested arrays (multi-dim) ------------------------------------------------
//
// `int32[][]` is `array-of-array-references`. `heap int32[2][3]` allocates the
// outer of 2 inner-array refs, then each of 2 inners of 3 ints each.


TEST(ArrayTests, twoDimRowsIndependent) {
    auto jit = CajetaJit::compile(makeSource("int32",
        "int32[][] arr = heap int32[2][3];\n"
        "arr[0][0] = 1;\n"
        "arr[0][1] = 2;\n"
        "arr[1][0] = 10;\n"
        "arr[1][1] = 20;\n"
        "return arr[0][0] + arr[0][1] + arr[1][0] + arr[1][1];"), "test.A");
    auto fn = jit->lookup<int32_t (*)()>("run");
    EXPECT_EQ(fn(), 33);
}



// --- field-read as array dimension (l-value → r-value coercion) ----------------
// Regression: `heap T[Klass.STATIC]` / `heap T[this.field]` mis-lowered the
// dimension. A static-final field read returns the GlobalVariable, an instance
// field read returns a struct GEP — neither is an AllocaInst, so the old
// alloca-only load in ArrayCreatorRest left the dimension a `ptr`, and the
// CreateIntCast below it sext'd a pointer → "SExt only operates on integer"
// IR-verify failure. Now routed through loadIfLValue, mirroring the same fix
// ArrayIndexExpression needed (Expression.cpp). Surfaced by AsyncWriter's
// `heap int8[AsyncWriter.DEFAULT_BUFFER]` and AsyncReader's `heap int8[this.chunkSize]`.

TEST(ArrayTests, staticFinalFieldAsArrayDimension) {
    auto src =
        "package test;\n"
        "public final class Cfg {\n"
        "    public static final int32 CAP = 6;\n"
        "}\n"
        "public final class A {\n"
        "    public static int64 run() {\n"
        "        int32[] arr = heap int32[Cfg.CAP];\n"
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
        "        int32[] arr = heap int32[this.chunkSize];\n"
        "        arr[this.chunkSize - 1] = 7;\n"
        "        return arr[this.chunkSize - 1];\n"
        "    }\n"
        "}\n"
        "public final class A {\n"
        "    public static int32 run() {\n"
        "        Buf b = heap Buf(4);\n"
        "        return b.make();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.A");
    auto fn = jit->lookup<int32_t (*)()>("run");
    ASSERT_NE(fn, nullptr);
    EXPECT_EQ(fn(), 7);
}
