//
// Tests for char literals — single-quoted character forms that lower to i8.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& body) {
    std::string src =
        "package test;\n"
        "public final class C {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.C");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

TEST(CharLiteralTests, asciiLetter) {
    EXPECT_EQ(runI32("return (int32) 'A';"), 'A');
}

TEST(CharLiteralTests, asciiDigit) {
    EXPECT_EQ(runI32("return (int32) '7';"), '7');
}

TEST(CharLiteralTests, escapeNewline) {
    EXPECT_EQ(runI32("return (int32) '\\n';"), 10);
}

TEST(CharLiteralTests, escapeTab) {
    EXPECT_EQ(runI32("return (int32) '\\t';"), 9);
}

TEST(CharLiteralTests, escapeBackslash) {
    EXPECT_EQ(runI32("return (int32) '\\\\';"), 92);
}

TEST(CharLiteralTests, escapeSingleQuote) {
    EXPECT_EQ(runI32("return (int32) '\\'';"), 39);
}

TEST(CharLiteralTests, escapeNull) {
    EXPECT_EQ(runI32("return (int32) '\\0';"), 0);
}

TEST(CharLiteralTests, unicodeAscii) {
    EXPECT_EQ(runI32("return (int32) '\\u0041';"), 'A');
}

TEST(CharLiteralTests, charComparedToInt) {
    EXPECT_EQ(runI32(
        "if ('z' == 122) return 1;\n"
        "return 0;"), 1);
}

TEST(CharLiteralTests, storedInCharLocal) {
    EXPECT_EQ(runI32(
        "char c = 'k';\n"
        "return (int32) c;"), 'k');
}
