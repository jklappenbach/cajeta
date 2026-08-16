// cajeta.lang.Guid — a 128-bit UUID value-type over uint128. Each test compiles
// a small program whose test.G.run() returns an int32 the C++ side asserts on
// (0 = all in-cajeta checks held, a positive code identifies the first failure).

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string prog(const std::string& body) {
    return
        "package test;\n"
        "import cajeta.lang.Guid;\n"
        "import cajeta.lang.GuidFormatException;\n"
        "import cajeta.lang.String;\n"
        "public final class G {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
}

int32_t runI32(const std::string& body) {
    auto jit = CajetaJit::compile(prog(body), "test.G");
    return jit->lookup<int32_t (*)()>("run")();
}

}  // namespace

// parse → toString is the identity on a canonical lowercase GUID.
TEST(GuidTests, parseToStringRoundTrip) {
    EXPECT_EQ(runI32(
        "Guid g #= Guid.parse(\"01234567-89ab-cdef-fedc-ba9876543210\");\n"
        "if (g.toString().equals(\"01234567-89ab-cdef-fedc-ba9876543210\")) { return 0; }\n"
        "return 1;\n"), 0);
}

// parse accepts uppercase hex and normalizes to lowercase on toString.
TEST(GuidTests, parseUppercaseNormalizes) {
    EXPECT_EQ(runI32(
        "Guid g #= Guid.parse(\"01234567-89AB-CDEF-FEDC-BA9876543210\");\n"
        "if (g.toString().equals(\"01234567-89ab-cdef-fedc-ba9876543210\")) { return 0; }\n"
        "return 1;\n"), 0);
}

// fromHalves + high()/low() round-trip, and the byte layout (high half is the
// most significant) renders in canonical order.
TEST(GuidTests, fromHalvesAccessorsAndLayout) {
    EXPECT_EQ(runI32(
        "Guid g #= Guid.fromHalves((uint64) 255L, (uint64) 1L);\n"
        "if (g.high() != (uint64) 255L) { return 1; }\n"
        "if (g.low() != (uint64) 1L) { return 2; }\n"
        "if (!g.toString().equals(\"00000000-0000-00ff-0000-000000000001\")) { return 3; }\n"
        "return 0;\n"), 0);
}

// equals() is exact; equal text → equal GUIDs, different text → not.
TEST(GuidTests, equalsIsExact) {
    EXPECT_EQ(runI32(
        "Guid a #= Guid.parse(\"01234567-89ab-cdef-fedc-ba9876543210\");\n"
        "Guid b #= Guid.parse(\"01234567-89ab-cdef-fedc-ba9876543210\");\n"
        "Guid c #= Guid.parse(\"01234567-89ab-cdef-fedc-ba9876543211\");\n"  // last nibble differs
        "if (!a.equals(b)) { return 1; }\n"
        "if (a.equals(c)) { return 2; }\n"
        "return 0;\n"), 0);
}

// Malformed input raises GuidFormatException (wrong length, bad separator, and
// a non-hex digit each rejected).
TEST(GuidTests, parseRejectsMalformed) {
    EXPECT_EQ(runI32(
        "int32 caught = 0;\n"
        "try { Guid.parse(\"too-short\"); } catch (GuidFormatException e) { caught = caught + 1; }\n"
        "try { Guid.parse(\"01234567x89ab-cdef-fedc-ba9876543210\"); } catch (GuidFormatException e) { caught = caught + 1; }\n"
        "try { Guid.parse(\"0123456g-89ab-cdef-fedc-ba9876543210\"); } catch (GuidFormatException e) { caught = caught + 1; }\n"
        "if (caught == 3) { return 0; }\n"
        "return caught;\n"), 0);
}

// random() yields a well-formed version-4 UUID: 36 chars, '4' at the version
// position (index 14), and a 10xx variant (index 19 in {8,9,a,b}).
TEST(GuidTests, randomIsVersion4) {
    EXPECT_EQ(runI32(
        "String s #= Guid.random().toString();\n"
        "if (s.byteLength() != 36) { return 1; }\n"
        "if (s.byteAt(14) != 52) { return 2; }\n"               // '4'
        "int32 var = (int32) s.byteAt(19);\n"
        "if (var != 56 && var != 57 && var != 97 && var != 98) { return 3; }\n"  // 8 9 a b
        "return 0;\n"), 0);
}

// Two random GUIDs are (overwhelmingly) distinct.
TEST(GuidTests, randomDistinct) {
    EXPECT_EQ(runI32(
        "Guid a #= Guid.random();\n"
        "Guid b #= Guid.random();\n"
        "if (a.equals(b)) { return 1; }\n"
        "return 0;\n"), 0);
}
