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

// --- bare null literal as a call argument (compiler-gap repros) ----------
// A bare `null` passed directly in arg position (vs assigned to a typed local
// first) must lower to a typed null matching the parameter. These pin the
// "bare-null-literal-argument lowering" gap.

namespace {
int32_t runFull(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.N");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}
} // namespace

// class-ref parameter receiving a bare null
TEST(NullHandlingTests, bareNullAsClassRefArg) {
    EXPECT_EQ(runFull(
        "package test;\n"
        "public final class Box {\n"
        "    public static int32 take(String s) {\n"
        "        if (s == null) return 7;\n"
        "        return 0;\n"
        "    }\n"
        "}\n"
        "public final class N {\n"
        "    public static int32 run() {\n"
        "        return Box.take(null);\n"
        "    }\n"
        "}\n"), 7);
}

// `pointer`-typed parameter receiving a bare null (the Reactor.register case)
TEST(NullHandlingTests, bareNullAsPointerArg) {
    EXPECT_EQ(runFull(
        "package test;\n"
        "public final class Box {\n"
        "    public static int32 take(pointer p) {\n"
        "        return 5;\n"
        "    }\n"
        "}\n"
        "public final class N {\n"
        "    public static int32 run() {\n"
        "        return Box.take(null);\n"
        "    }\n"
        "}\n"), 5);
}

// instance-method class-ref parameter receiving a bare null
TEST(NullHandlingTests, bareNullAsInstanceMethodArg) {
    EXPECT_EQ(runFull(
        "package test;\n"
        "public final class Box {\n"
        "    public Box() { }\n"
        "    public int32 take(String s) {\n"
        "        if (s == null) return 9;\n"
        "        return 0;\n"
        "    }\n"
        "}\n"
        "public final class N {\n"
        "    public static int32 run() {\n"
        "        Box b = new Box();\n"
        "        return b.take(null);\n"
        "    }\n"
        "}\n"), 9);
}
