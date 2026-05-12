//
// Tests for Java-17 switch expressions in arrow form. The colon form with
// `yield` and guarded patterns are intentionally out of scope today.
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
        "public final class Se {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.Se");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace

TEST(SwitchExpressionTests, basicArrow) {
    EXPECT_EQ(runI32(
        "int32 x = 2;\n"
        "return switch (x) {\n"
        "    case 1 -> 10;\n"
        "    case 2 -> 20;\n"
        "    case 3 -> 30;\n"
        "    default -> 99;\n"
        "};"), 20);
}

TEST(SwitchExpressionTests, defaultArm) {
    EXPECT_EQ(runI32(
        "int32 x = 7;\n"
        "return switch (x) {\n"
        "    case 1 -> 10;\n"
        "    case 2 -> 20;\n"
        "    default -> 999;\n"
        "};"), 999);
}

TEST(SwitchExpressionTests, multiLabel) {
    // Multiple labels on one arm — `case 1, 2, 3 ->` collapses three cases.
    EXPECT_EQ(runI32(
        "int32 x = 2;\n"
        "return switch (x) {\n"
        "    case 1, 2, 3 -> 100;\n"
        "    default -> 0;\n"
        "};"), 100);
}

TEST(SwitchExpressionTests, assignedToLocal) {
    EXPECT_EQ(runI32(
        "int32 x = 5;\n"
        "int32 r = switch (x) {\n"
        "    case 5 -> 42;\n"
        "    default -> 0;\n"
        "};\n"
        "return r;"), 42);
}

TEST(SwitchExpressionTests, embeddedInExpression) {
    // The switch result participates in the surrounding arithmetic — tests that
    // the phi result has a usable LLVM Value type.
    EXPECT_EQ(runI32(
        "int32 x = 1;\n"
        "return 1000 + switch (x) {\n"
        "    case 1 -> 5;\n"
        "    default -> 0;\n"
        "};"), 1005);
}

TEST(SwitchExpressionTests, discriminatorFromExpression) {
    EXPECT_EQ(runI32(
        "int32 a = 2;\n"
        "int32 b = 3;\n"
        "return switch (a + b) {\n"
        "    case 5 -> 1;\n"
        "    default -> 0;\n"
        "};"), 1);
}
