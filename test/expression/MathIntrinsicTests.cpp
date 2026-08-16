//
// Tests for the Math.* intrinsics, lowered to LLVM intrinsics inline.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSourceI32(const std::string& body) {
    return "package test;\n"
           "public final class M {\n"
           "    public static int32 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

std::string makeSourceF64(const std::string& body) {
    return "package test;\n"
           "public final class M {\n"
           "    public static float64 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

int32_t runI32(const std::string& body) {
    auto jit = CajetaJit::compile(makeSourceI32(body), "test.M");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

double runF64(const std::string& body) {
    auto jit = CajetaJit::compile(makeSourceF64(body), "test.M");
    auto fn = jit->lookup<double (*)()>("run");
    return fn();
}

} // namespace


TEST(MathIntrinsicTests, absIntPositive) {
    EXPECT_EQ(runI32("return (int32) Math.abs(7);"), 7);
}












TEST(MathIntrinsicTests, eConstant) {
    EXPECT_DOUBLE_EQ(runF64("return Math.E;"), 2.718281828459045);
}


TEST(MathIntrinsicTests, integerMinValue) {
    // Don't use INT32_MIN directly — that'd be -INT32_MAX-1 and we want the runtime
    // to confirm the constant matches exactly.
    EXPECT_EQ(runI32(
        "if (Integer.MIN_VALUE == -2147483648) return 1;\n"
        "return 0;"), 1);
}





