//
// Tests for built-in String methods: size/length, equals.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "public final class Smt {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

int32_t runJit(const std::string& body) {
    auto jit = CajetaJit::compile(makeSource(body), "test.Smt");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

TEST(StringMethodsTests, declOnlyDoesNotCrash) {
    EXPECT_EQ(runJit(
        "String s = \"hello\";\n"
        "return 42;"), 42);
}

TEST(StringMethodsTests, sizeOfHello) {
    EXPECT_EQ(runJit(
        "String s = \"hello\";\n"
        "if (s.size() == 5) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, sizeOfEmpty) {
    EXPECT_EQ(runJit(
        "String s = \"\";\n"
        "if (s.size() == 0) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, countOfTenAsciiChars) {
    // `count()` is the universal element-count API across String and
    // Collections (2026-05-18 naming convention); for ASCII this
    // coincides with `size()` (byte length), for multibyte UTF-8 the
    // two diverge. Test pins ASCII parity.
    EXPECT_EQ(runJit(
        "String s = \"abcdefghij\";\n"
        "if (s.count() == 10) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, equalsTrue) {
    EXPECT_EQ(runJit(
        "String a = \"foo\";\n"
        "String b = \"foo\";\n"
        "if (a.equals(b)) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, equalsFalseDifferentContent) {
    EXPECT_EQ(runJit(
        "String a = \"foo\";\n"
        "String b = \"bar\";\n"
        "if (a.equals(b)) return 1;\n"
        "return 0;"), 0);
}

TEST(StringMethodsTests, equalsLiteralRhs) {
    EXPECT_EQ(runJit(
        "String a = \"hello\";\n"
        "if (a.equals(\"hello\")) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, equalsAfterConcat) {
    EXPECT_EQ(runJit(
        "String a = \"foo\" + \"bar\";\n"
        "if (a.equals(\"foobar\")) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, isEmptyEmpty) {
    EXPECT_EQ(runJit(
        "String s = \"\";\n"
        "if (s.isEmpty()) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, isEmptyNonEmpty) {
    EXPECT_EQ(runJit(
        "String s = \"x\";\n"
        "if (s.isEmpty()) return 1;\n"
        "return 0;"), 0);
}

TEST(StringMethodsTests, charAtFirst) {
    EXPECT_EQ(runJit(
        "String s = \"hello\";\n"
        "return (int32) s.charAt(0);"), 'h');
}

TEST(StringMethodsTests, charAtMiddle) {
    EXPECT_EQ(runJit(
        "String s = \"abcdef\";\n"
        "return (int32) s.charAt(3);"), 'd');
}

TEST(StringMethodsTests, charAtOutOfRangeReturnsZero) {
    EXPECT_EQ(runJit(
        "String s = \"abc\";\n"
        "return (int32) s.charAt(99);"), 0);
}

TEST(StringMethodsTests, indexOfFound) {
    EXPECT_EQ(runJit(
        "String s = \"hello world\";\n"
        "if (s.indexOf(\"world\") == 6) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, indexOfNotFound) {
    EXPECT_EQ(runJit(
        "String s = \"hello\";\n"
        "if (s.indexOf(\"xyz\") == -1) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, startsWithTrue) {
    EXPECT_EQ(runJit(
        "String s = \"hello world\";\n"
        "if (s.startsWith(\"hello\")) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, startsWithFalse) {
    EXPECT_EQ(runJit(
        "String s = \"hello world\";\n"
        "if (s.startsWith(\"world\")) return 1;\n"
        "return 0;"), 0);
}

TEST(StringMethodsTests, endsWithTrue) {
    EXPECT_EQ(runJit(
        "String s = \"hello world\";\n"
        "if (s.endsWith(\"world\")) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, endsWithFalse) {
    EXPECT_EQ(runJit(
        "String s = \"hello world\";\n"
        "if (s.endsWith(\"hello\")) return 1;\n"
        "return 0;"), 0);
}

TEST(StringMethodsTests, containsTrue) {
    EXPECT_EQ(runJit(
        "String s = \"abcdefg\";\n"
        "if (s.contains(\"cde\")) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, containsFalse) {
    EXPECT_EQ(runJit(
        "String s = \"abcdefg\";\n"
        "if (s.contains(\"xyz\")) return 1;\n"
        "return 0;"), 0);
}

TEST(StringMethodsTests, substringSliceMatches) {
    EXPECT_EQ(runJit(
        "String s = \"hello world\";\n"
        "String sub = s.substring(6, 11);\n"
        "if (sub.equals(\"world\")) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, substringEmptyWindow) {
    EXPECT_EQ(runJit(
        "String s = \"abc\";\n"
        "String sub = s.substring(2, 2);\n"
        "if (sub.isEmpty()) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, toUpperCaseAscii) {
    EXPECT_EQ(runJit(
        "String s = \"Hello, World!\";\n"
        "String u = s.toUpperCase();\n"
        "if (u.equals(\"HELLO, WORLD!\")) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, toLowerCaseAscii) {
    EXPECT_EQ(runJit(
        "String s = \"Hello, World!\";\n"
        "String l = s.toLowerCase();\n"
        "if (l.equals(\"hello, world!\")) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, toUpperCaseLong) {
    // 46 bytes: exercises the 32-byte SWAR path + 8-byte word + scalar tail.
    EXPECT_EQ(runJit(
        "String s = \"the quick brown fox jumps over the lazy dog 123\";\n"
        "String u = s.toUpperCase();\n"
        "if (u.equals(\"THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 123\")) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, toLowerCaseLong) {
    EXPECT_EQ(runJit(
        "String s = \"THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 123\";\n"
        "String l = s.toLowerCase();\n"
        "if (l.equals(\"the quick brown fox jumps over the lazy dog 123\")) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, trimBothSides) {
    EXPECT_EQ(runJit(
        "String s = \"   spaced   \";\n"
        "String t = s.trim();\n"
        "if (t.equals(\"spaced\")) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, trimAllWhitespace) {
    EXPECT_EQ(runJit(
        "String s = \" \\t \\n \";\n"
        "if (s.trim().isEmpty()) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, replaceSingleMatch) {
    EXPECT_EQ(runJit(
        "String s = \"foo bar\";\n"
        "String r = s.replace(\"bar\", \"baz\");\n"
        "if (r.equals(\"foo baz\")) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, replaceMultipleMatches) {
    EXPECT_EQ(runJit(
        "String s = \"a-b-c-d\";\n"
        "String r = s.replace(\"-\", \"::\");\n"
        "if (r.equals(\"a::b::c::d\")) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, replaceNoMatch) {
    EXPECT_EQ(runJit(
        "String s = \"hello\";\n"
        "String r = s.replace(\"x\", \"y\");\n"
        "if (r.equals(\"hello\")) return 1;\n"
        "return 0;"), 1);
}

// --- hash() (XXH3) -------------------------------------------------------

TEST(StringMethodsTests, hashEqualContentEqual) {
    EXPECT_EQ(runJit(
        "String a = \"foo\";\n"
        "String b = \"foo\";\n"
        "if (a.hash() == b.hash()) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, hashConcatEqualsLiteral) {
    EXPECT_EQ(runJit(
        "String a = \"foo\" + \"bar\";\n"
        "if (a.hash() == \"foobar\".hash()) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, hashEmptyEqualsDefault) {
    EXPECT_EQ(runJit(
        "String a = \"\";\n"
        "String b = \"\";\n"
        "if (a.hash() == b.hash()) return 1;\n"
        "return 0;"), 1);
}

TEST(StringMethodsTests, hashDifferentContentDiffers) {
    EXPECT_EQ(runJit(
        "String a = \"foo\";\n"
        "String b = \"bar\";\n"
        "if (a.hash() != b.hash()) return 1;\n"
        "return 0;"), 1);
}
