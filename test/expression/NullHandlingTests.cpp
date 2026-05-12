//
// Tests for null literal handling — assignment, comparisons, null-safe
// behavior of String runtime helpers.
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
        "public final class N {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.N");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

TEST(NullHandlingTests, assignNullToString) {
    EXPECT_EQ(runI32(
        "String s = null;\n"
        "return 0;"), 0);
}

TEST(NullHandlingTests, nullEqEqNull) {
    EXPECT_EQ(runI32(
        "String s = null;\n"
        "if (s == null) return 1;\n"
        "return 0;"), 1);
}

TEST(NullHandlingTests, stringNotNullAfterAssignment) {
    EXPECT_EQ(runI32(
        "String s = \"hello\";\n"
        "if (s != null) return 1;\n"
        "return 0;"), 1);
}

TEST(NullHandlingTests, stringLengthOnNullReturnsZero) {
    // __cajeta_str_len handles null with a 0 fallback so length probes are safe.
    EXPECT_EQ(runI32(
        "String s = null;\n"
        "if (s.size() == 0) return 1;\n"
        "return 0;"), 1);
}

TEST(NullHandlingTests, equalsOnNullReturnsFalse) {
    EXPECT_EQ(runI32(
        "String s = null;\n"
        "if (s.equals(\"x\")) return 1;\n"
        "return 0;"), 0);
}

TEST(NullHandlingTests, isEmptyOnNullIsTrue) {
    // An empty string and null both register as "empty" today — they're both
    // length-zero observable byte sequences. Revisit when Java's NPE semantics
    // are wired in.
    EXPECT_EQ(runI32(
        "String s = null;\n"
        "if (s.isEmpty()) return 1;\n"
        "return 0;"), 1);
}
