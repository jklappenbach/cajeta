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
