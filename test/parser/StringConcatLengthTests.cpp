//
// String concat must respect byteLength — docs-refactor 11.2.3.
//
// The `+` lowering unwraps class String operands to bytes.data and
// calls the strlen-based __cajeta_str_concat. Only literal codegen
// NUL-terminates; a String wrapped over a bare byte array (e.g.
// Sha256.hashHex's heap int8[64]) has no terminator, so strlen reads
// past the logical end — the tour's trailing-Q digests. Concat must
// take the class String's byteLength, not scan.
//
// The fixtures place sentinel bytes INSIDE the owned buffer beyond
// byteLength, so the pre-fix over-read is deterministic and in-bounds.
//

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

// "abcd" + 4 sentinel 'Q' bytes in one owned buffer, byteLength 4.
const char* kMakeS =
    "        int8[] raw = heap int8[8];\n"
    "        raw[0] = 97; raw[1] = 98; raw[2] = 99; raw[3] = 100;\n"
    "        raw[4] = 81; raw[5] = 81; raw[6] = 81; raw[7] = 81;\n"
    "        String s = heap String(#raw, 4);\n";
} // namespace

// literal + S: 1 + 4 bytes, not 1 + strlen("abcdQQQQ…").
TEST(StringConcatLengthTests, literalPlusBytesBackedString) {
    std::string src = std::string(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n")
        + kMakeS +
        "        String j = \"x\" + s;\n"
        "        return j.byteLength;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// S + literal — rhs unwrap path.
TEST(StringConcatLengthTests, bytesBackedStringPlusLiteral) {
    std::string src = std::string(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n")
        + kMakeS +
        "        String j = s + \"!\";\n"
        "        return j.byteLength;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// S + S — both sides unwrapped.
TEST(StringConcatLengthTests, bytesBackedBothSides) {
    std::string src = std::string(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n")
        + kMakeS +
        "        String j = s + s;\n"
        "        return j.byteLength;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 8);
}

// Primitive auto-stringify still works (strlen path for the
// stringified side, byteLength for the String side).
TEST(StringConcatLengthTests, primitiveStringifyUnaffected) {
    std::string src = std::string(
        "package test;\n"
        "import cajeta.lang.String;\n"
        "public final class D {\n"
        "    public static int32 run() {\n")
        + kMakeS +
        "        String j = s + 42;\n"
        "        return j.byteLength;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 6);
}
