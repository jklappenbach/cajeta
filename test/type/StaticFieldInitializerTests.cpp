// A static field with a non-constant initializer is stored by the class's clinit.
// Array initializers must store the array pointer, the static keeps the array
// alive across calls, and a genuine mismatch names two distinguishable types.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string diagnosticFor(const std::string& src, std::string* message = nullptr) {
    try {
        auto jit = CajetaJit::compile(src, "test.S");
        return "<compiled>";
    } catch (cajeta::Exception& e) {
        if (message) *message = e.getMessage();
        return e.getErrorId();
    }
}

const char* kArrays =
    "package test;\n"
    "public final class S {\n"
    "    static int8[] EMPTY = heap int8[0];\n"
    "    static int32[] TABLE = heap int32[4];\n"
    "    static int32[] NEXT = heap int32[2];\n"
    "    static int32[] PRIMES = [2, 3, 5, 7, 11];\n"
    "    static #int32[] make(int32 n) {\n"
    "        int32[] a = heap int32[n];\n"
    "        a[0] = 41;\n"
    "        return #a;\n"
    "    }\n"
    "    static int32[] MADE = S.make(3);\n"
    "    public static int32 emptyCount() { return (int32) S.EMPTY.count(); }\n"
    "    public static int32 primeSum() {\n"
    "        int32 s = 0;\n"
    "        int32 i = 0;\n"
    "        while (i < (int32) PRIMES.count()) { s = s + PRIMES[i]; i = i + 1; }\n"
    "        return s;\n"
    "    }\n"
    "    public static int32 made() { return MADE[0] + (int32) MADE.count(); }\n"
    "    public static int32 fill() {\n"
    "        int32 i = 0;\n"
    "        while (i < 4) { TABLE[i] = 100 + i; i = i + 1; }\n"
    "        return (int32) TABLE.count();\n"
    "    }\n"
    "    public static int32 churn() {\n"
    "        int32 total = 0;\n"
    "        int32 k = 0;\n"
    "        while (k < 256) {\n"
    "            int32[] junk = heap int32[4];\n"
    "            junk[0] = -1; junk[1] = -1; junk[2] = -1; junk[3] = -1;\n"
    "            int8[] bytes = heap int8[0];\n"
    "            total = total + junk[0] + (int32) bytes.count();\n"
    "            k = k + 1;\n"
    "        }\n"
    "        return total;\n"
    "    }\n"
    "    public static int32 readBack() {\n"
    "        return S.TABLE[0] + TABLE[1] + S.TABLE[2] + TABLE[3] + (int32) NEXT.count();\n"
    "    }\n"
    "}\n";

}  // namespace

TEST(StaticFieldInitializerTests, emptyArrayStaticIsReadable) {
    auto jit = CajetaJit::compile(kArrays, "test.S");
    EXPECT_EQ(jit->lookup<int32_t (*)()>("emptyCount")(), 0);
}

TEST(StaticFieldInitializerTests, arrayLiteralStaticHoldsItsElements) {
    auto jit = CajetaJit::compile(kArrays, "test.S");
    EXPECT_EQ(jit->lookup<int32_t (*)()>("primeSum")(), 28);
}

TEST(StaticFieldInitializerTests, arrayFromAnOwningFactoryIsKept) {
    auto jit = CajetaJit::compile(kArrays, "test.S");
    EXPECT_EQ(jit->lookup<int32_t (*)()>("made")(), 44);
}

TEST(StaticFieldInitializerTests, arrayStaticSurvivesAcrossCallsAndChurn) {
    auto jit = CajetaJit::compile(kArrays, "test.S");
    EXPECT_EQ(jit->lookup<int32_t (*)()>("fill")(), 4);
    EXPECT_EQ(jit->lookup<int32_t (*)()>("churn")(), -256);
    EXPECT_EQ(jit->lookup<int32_t (*)()>("readBack")(), 408);
    EXPECT_EQ(jit->lookup<int32_t (*)()>("churn")(), -256);
    EXPECT_EQ(jit->lookup<int32_t (*)()>("readBack")(), 408);
    EXPECT_EQ(jit->lookup<int32_t (*)()>("emptyCount")(), 0);
}

TEST(StaticFieldInitializerTests, classAndStringStaticsStillInitialize) {
    std::string src =
        "package test;\n"
        "import cajeta.lang.String;\n"
        "class Cell {\n"
        "    public int32 n;\n"
        "    public Cell(int32 n) { this.n = n; }\n"
        "}\n"
        "public final class S {\n"
        "    static Cell C = heap Cell(5);\n"
        "    static String NAME = \"abc\";\n"
        "    static int64 WIDE = 7;\n"
        "    public static int32 run() { return C.n + NAME.byteLength() + (int32) WIDE; }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.S");
    EXPECT_EQ(jit->lookup<int32_t (*)()>("run")(), 15);
}

TEST(StaticFieldInitializerTests, scalarFromStringIsStillRejected) {
    std::string src =
        "package test;\n"
        "public final class S {\n"
        "    static int32 X = \"abc\";\n"
        "    public static int32 run() { return X; }\n"
        "}\n";
    std::string msg;
    EXPECT_EQ(diagnosticFor(src, &msg), "CAJETA_ERROR_INITIALIZER_TYPE_MISMATCH");
    EXPECT_NE(msg.find("'int32'"), std::string::npos) << msg;
}

TEST(StaticFieldInitializerTests, arrayOfAnotherElementTypeIsRejectedWithDistinctNames) {
    std::string src =
        "package test;\n"
        "public final class S {\n"
        "    static int32[] A = heap int8[3];\n"
        "    public static int32 run() { return (int32) A.count(); }\n"
        "}\n";
    std::string msg;
    EXPECT_EQ(diagnosticFor(src, &msg), "CAJETA_ERROR_INITIALIZER_TYPE_MISMATCH");
    EXPECT_NE(msg.find("'int8[]'"), std::string::npos) << msg;
    EXPECT_NE(msg.find("'int32[]'"), std::string::npos) << msg;
}

TEST(StaticFieldInitializerTests, unqualifiedElementAccessReachesTheArray) {
    std::string src =
        "package test;\n"
        "public final class S {\n"
        "    static int8[] BUF;\n"
        "    static int32[] AFTER = heap int32[3];\n"
        "    public static int32 run() {\n"
        "        S.BUF = heap int8[5];\n"
        "        BUF[1] = 7;\n"
        "        S.BUF[2] = 9;\n"
        "        return (int32) S.BUF[1] * 100 + (int32) BUF[2] * 10 + (int32) AFTER.count();\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.S");
    EXPECT_EQ(jit->lookup<int32_t (*)()>("run")(), 793);
}
