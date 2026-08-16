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


TEST(SwitchExpressionTests, defaultArm) {
    EXPECT_EQ(runI32(
        "int32 x = 7;\n"
        "return switch (x) {\n"
        "    case 1 -> 10;\n"
        "    case 2 -> 20;\n"
        "    default -> 999;\n"
        "};"), 999);
}




