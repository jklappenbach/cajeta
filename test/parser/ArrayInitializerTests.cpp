//
// `int32[] xs = {1, 2, 3}` literal-array-initializer codegen.
// Replaces the previously-stubbed ArrayInitializer::generateCode that
// returned null without allocating.
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

// Smallest case: three int32 literals, return the second slot.
TEST(ArrayInitializerTests, threeElementInt32Literal) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {10, 20, 30};\n"
        "        return xs[1];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 20);
}

// All three slots, summed. Confirms order is preserved and all elements
// landed in the right slots.
TEST(ArrayInitializerTests, allSlotsRetainValues) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 4};\n"
        "        return xs[0] + xs[1] + xs[2];\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// size() reads the runtime-known length from the array header.
TEST(ArrayInitializerTests, sizeMatchesLiteralLength) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] xs = {1, 2, 3, 4, 5};\n"
        "        return (int32) xs.size();\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 5);
}

// Mixed expressions in initializer slots — not just literals. Each slot's
// initializer is evaluated and stored in sequence.
TEST(ArrayInitializerTests, expressionsAsElements) {
    auto src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 base = 10;\n"
        "        int32[] xs = {base, base + 5, base * 2};\n"
        "        return xs[0] + xs[1] + xs[2];\n"
        "    }\n"
        "}\n";
    // 10 + 15 + 20 = 45
    EXPECT_EQ(runI32(src), 45);
}
