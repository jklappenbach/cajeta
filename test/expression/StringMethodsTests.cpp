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















// SIMD memmem path: haystack > 32 bytes so the 32-wide scan runs; match lands
// well past the first vector block.

// Match at the very tail (last full window) — exercises the block/tail boundary.

// Absent long needle over a > 32-byte haystack whose first byte recurs (the
// two-byte prefilter must reject every candidate; result -1).

// Single-byte needle on a long haystack (nlen==1 path: last==first).














TEST(StringMethodsTests, trimAllWhitespace) {
    EXPECT_EQ(runJit(
        "String s = \" \\t \\n \";\n"
        "if (s.trim().isEmpty()) return 1;\n"
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

// --- Cajeta.allocBytes (uninitialized int8[]) ---------------------------

TEST(StringMethodsTests, allocBytesWriteReadRoundTrip) {
    // allocBytes returns an owned, indexable int8[] of the given length;
    // we fully overwrite then read back (data is uninitialized on alloc).
    EXPECT_EQ(runJit(
        "int8[] b = Cajeta.allocBytes(10);\n"
        "int32 i = 0;\n"
        "while (i < 10) { b[i] = (int8) i; i = i + 1; }\n"
        "int32 sum = 0;\n"
        "i = 0;\n"
        "while (i < 10) { sum = sum + (int32) b[i]; i = i + 1; }\n"
        "return sum;"), 45);
}
