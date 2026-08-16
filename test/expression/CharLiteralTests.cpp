//
// Tests for char literals — single-quoted character forms that lower to i8.
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
        "public final class C {\n"
        "    public static int32 run() {\n"
        "        " + body + "\n"
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.C");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

} // namespace







TEST(CharLiteralTests, escapeNull) {
    EXPECT_EQ(runI32("return (int32) '\\0';"), 0);
}



