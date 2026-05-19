// String basic predicates + indexing:
//   - isEmpty()
//   - byteAt(int32)       — raw byte at byte index (byte-indexed)
//   - codepointAt(int32)  — codepoint at codepoint index (O(N) walk)
//
// Per cajeta-docs/stdlib/lang/String.md § Indexing.

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

constexpr const char* PRELUDE =
    "package test;\n"
    "import cajeta.lang.String;\n";
} // namespace

// NOTE: `isEmpty()` is intentionally NOT a class method yet — the
// legacy intrinsic dispatch in MethodCallExpression unconditionally
// routes `s.isEmpty()` to `__cajeta_str_isEmpty(receiver)` when
// receiverType's typeName is "String", which interprets the receiver
// pointer as a C-string. For class instances that misreads the
// vtable byte and returns the wrong answer. The class-method form
// lands once the intrinsic gate distinguishes class-typed receivers
// from the primitive-alias path (blocked on Phase 2b-β's literal
// codegen flip — see cajeta-docs/stdlib/lang/String.md).

// byteAt returns the raw byte at the given byte index.
TEST(StringIndexingTests, byteAtAsciiIndex) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] b = new int8[3];\n"
        "        b[0] = (int8) 'a';\n"   // 0x61
        "        b[1] = (int8) 'b';\n"   // 0x62
        "        b[2] = (int8) 'c';\n"   // 0x63
        "        String s = heap String(b, 3);\n"
        "        int8 c = s.byteAt(1);\n"
        "        return (int32) c;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0x62);
}

// codepointAt walks the UTF-8 bytes and returns the codepoint at the
// given codepoint index. ASCII case: byte index == codepoint index.
TEST(StringIndexingTests, codepointAtAscii) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] b = new int8[3];\n"
        "        b[0] = (int8) 'a';\n"
        "        b[1] = (int8) 'b';\n"
        "        b[2] = (int8) 'c';\n"
        "        String s = heap String(b, 3);\n"
        "        char c = s.codepointAt(1);\n"
        "        return (int32) c;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), (int32_t) 'b');
}

// codepointAt over a 2-byte UTF-8 sequence: cpIdx 0 → first codepoint
// (the multi-byte one), cpIdx 1 → second codepoint (ASCII).
// Bytes: [0xC3, 0xA9, 0x21] = 'é!' → 2 codepoints (U+00E9, U+0021).
TEST(StringIndexingTests, codepointAtMultibyte) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] b = new int8[3];\n"
        "        b[0] = (int8) 0xC3;\n"
        "        b[1] = (int8) 0xA9;\n"
        "        b[2] = (int8) 0x21;\n"   // '!'
        "        String s = heap String(b, 3);\n"
        "        char cp0 = s.codepointAt(0);\n"
        "        char cp1 = s.codepointAt(1);\n"
        "        return (int32) cp0 * 1000 + (int32) cp1;\n"
        "    }\n"
        "}\n";
    // U+00E9 = 233; '!' = 33. 233 * 1000 + 33 = 233033
    EXPECT_EQ(runI32(src), 233033);
}

// 4-byte UTF-8: '😀' = U+1F600 = 0xF0 0x9F 0x98 0x80
TEST(StringIndexingTests, codepointAtFourByte) {
    auto src = std::string(PRELUDE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] b = new int8[4];\n"
        "        b[0] = (int8) 0xF0;\n"
        "        b[1] = (int8) 0x9F;\n"
        "        b[2] = (int8) 0x98;\n"
        "        b[3] = (int8) 0x80;\n"
        "        String s = heap String(b, 4);\n"
        "        char cp = s.codepointAt(0);\n"
        "        return (int32) cp;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0x1F600);
}
