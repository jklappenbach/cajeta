// `char` is now a 32-bit Unicode codepoint (was i8). Verify:
//   - Character literals evaluate to int32 values.
//   - ASCII literals retain their ASCII codepoint value.
//   - Multi-byte UTF-8 source characters decode to the Unicode codepoint.
//   - Escape sequences (\uXXXX) decode as written.
//   - `char` typed locals hold int32 widths (assignment + return).
//
// See docs/stdlib/lang/String.md § `char` is a 32-bit Unicode codepoint.

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {
int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}
} // namespace

// ASCII char literal evaluates to int32 99 ('c').
TEST(CharCodepointTests, asciiLiteralIsCodepoint) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        char c = 'c';\n"
        "        return (int32) c;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 99);
}

// char width is 32-bit — can hold values above 255 (was capped at i8 = -128..127 before).
TEST(CharCodepointTests, charHolds16BitValue) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        char c = (char) 1000;\n"   // U+03E8 (greek small letter sampi)
        "        return (int32) c;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1000);
}

// char holds full 32-bit codepoint range — emoji U+1F600 = 128512.
TEST(CharCodepointTests, charHoldsFullCodepointRange) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        char c = (char) 128512;\n"  // U+1F600 grinning face
        "        return (int32) c;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 128512);
}

// Multi-byte UTF-8 source char 'é' (U+00E9 = 233) decodes to its codepoint, not its
// first UTF-8 byte (0xC3 = 195).
TEST(CharCodepointTests, multiByteSourceCharDecodesToCodepoint) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        char c = 'é';\n"            // U+00E9 = 233, UTF-8: 0xC3 0xA9
        "        return (int32) c;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 233);
}

// Three-byte UTF-8 source ('☃' = U+2603 snowman = 9731, UTF-8: 0xE2 0x98 0x83)
// decodes to its codepoint.
TEST(CharCodepointTests, threeByteUtf8SourceDecodes) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        char c = '\xE2\x98\x83';\n"  // raw UTF-8 bytes for snowman
        "        return (int32) c;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 9731);
}

// \uXXXX escapes still work (Java-compatible) and decode to int32.
TEST(CharCodepointTests, unicodeEscapeDecodes) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        char c = '\\u00E9';\n"
        "        return (int32) c;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 233);
}

// Standard ASCII escapes (\n, \t, etc.) keep their values.
TEST(CharCodepointTests, asciiEscapesUnchanged) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        char nl = '\\n';\n"
        "        char tab = '\\t';\n"
        "        return ((int32) nl) * 100 + (int32) tab;\n"
        "    }\n"
        "}\n";
    // '\n' = 10, '\t' = 9 → 10*100 + 9 = 1009
    EXPECT_EQ(runI32(src), 1009);
}
