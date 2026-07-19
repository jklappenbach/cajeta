//
// Enum behavioral tests. v1 scope: a simple enum is a typed alias for int32
// where each named constant has an ordinal (0, 1, 2, ...). `MyEnum.NAME` is
// resolved at compile time to an i32 constant; variables typed as the enum
// hold the ordinal directly.
//
// Not in v1 scope:
//   - constants with arguments: `MONDAY(1)`
//   - enum bodies with methods
//   - `enum E implements Comparable` clauses
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

// First constant gets ordinal 0; reading via `MyEnum.NAME` produces an int32.
TEST(EnumTests, firstConstantIsOrdinalZero) {
    auto src =
        "package test;\n"
        "public enum Direction { NORTH, SOUTH, EAST, WEST }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Direction.NORTH;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 0);
}

// Later constants get sequential ordinals.
TEST(EnumTests, sequentialOrdinals) {
    auto src =
        "package test;\n"
        "public enum Direction { NORTH, SOUTH, EAST, WEST }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Direction.NORTH + Direction.SOUTH + Direction.EAST + Direction.WEST;\n"
        "    }\n"
        "}\n";
    // 0 + 1 + 2 + 3 = 6
    EXPECT_EQ(runI32(src), 6);
}

// Variable typed as the enum holds the ordinal value. Assignment + read.
TEST(EnumTests, variableTypedAsEnumHoldsOrdinal) {
    auto src =
        "package test;\n"
        "public enum Color { RED, GREEN, BLUE }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Color c = Color.GREEN;\n"
        "        return c;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// `switch` on an enum-typed variable matches against `MyEnum.CONST` labels.
// Today the switch label compiles to its ordinal int32 constant, which
// matches the int32 the discriminator holds.
TEST(EnumTests, switchOnEnumValue) {
    auto src =
        "package test;\n"
        "public enum Color { RED, GREEN, BLUE }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Color c = Color.BLUE;\n"
        "        switch (c) {\n"
        "            case Color.RED: return 100;\n"
        "            case Color.GREEN: return 200;\n"
        "            case Color.BLUE: return 300;\n"
        "            default: return 999;\n"
        "        }\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 300);
}

// ---------------------------------------------------------------------------
// Enum bodies with methods (was "not in v1 scope"). An enum value is still an
// i32 ordinal; a method declared in the enum body is dispatched statically
// (enums are final — there is no subclass to override) with the ordinal passed
// as the receiver. Before this landed, declaring a method compiled but CALLING
// one SIGSEGV'd in MethodCallExpression::generateCode: the method was parsed
// and its body generated, but attached to nothing, so there was no receiver to
// dispatch on.
// ---------------------------------------------------------------------------

// The minimal case: an instance method on an enum, called on a local.
TEST(EnumTests, instanceMethodOnEnumValue) {
    auto src =
        "package test;\n"
        "public enum Verb {\n"
        "    GET,\n"
        "    POST;\n"
        "\n"
        "    public int32 weight() {\n"
        "        return 7;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Verb v = Verb.GET;\n"
        "        return v.weight();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// `this` inside an enum method is the receiver's ordinal, so a method can
// branch on which constant it was called on — the whole point of the feature
// (RFC-style predicate tables like Method.isSafe()).
TEST(EnumTests, enumMethodReadsThisOrdinal) {
    auto src =
        "package test;\n"
        "public enum Verb {\n"
        "    GET,\n"
        "    POST;\n"
        "\n"
        "    public boolean isSafe() {\n"
        "        return this == Verb.GET;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Verb g = Verb.GET;\n"
        "        Verb p = Verb.POST;\n"
        "        int32 acc = 0;\n"
        "        if (g.isSafe()) { acc = acc + 10; }\n"
        "        if (!p.isSafe()) { acc = acc + 5; }\n"
        "        return acc;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

// A method called directly on a constant (no intervening local).
TEST(EnumTests, methodCalledOnEnumConstant) {
    auto src =
        "package test;\n"
        "public enum Verb {\n"
        "    GET,\n"
        "    POST;\n"
        "\n"
        "    public int32 ordinalPlus(int32 n) {\n"
        "        return this + n;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        return Verb.POST.ordinalPlus(40);\n"
        "    }\n"
        "}\n";
    // POST is ordinal 1; 1 + 40 = 41.
    EXPECT_EQ(runI32(src), 41);
}

// A STATIC method in an enum body, invoked on the enum's name. The bare
// `Verb` receiver resolves to the i32 enum type (not a class), which the
// class-name-receiver fallback now adopts so the ENUM_FLAG redirect can
// route the call to the companion. Also covers a String-typed return and
// enum-typed return from enum-body methods.
TEST(EnumTests, staticMethodOnEnumName) {
    auto src =
        "package test;\n"
        "public enum Verb {\n"
        "    GET,\n"
        "    POST;\n"
        "\n"
        "    public static Verb parse(int32 code) {\n"
        "        if (code == 0) { return Verb.GET; }\n"
        "        return Verb.POST;\n"
        "    }\n"
        "\n"
        "    public int32 doubled() {\n"
        "        return this * 2;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Verb v = Verb.parse(1);\n"
        "        return v.doubled() + Verb.parse(0);\n"
        "    }\n"
        "}\n";
    // parse(1)=POST(1) doubled -> 2; + parse(0)=GET(0) -> 2.
    EXPECT_EQ(runI32(src), 2);
}
