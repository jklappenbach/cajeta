//
// Comparison and short-circuit logical expression tests.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& returnType, const std::string& body) {
    return "package test;\n"
           "public final class C {\n"
           "    public static " + returnType + " run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

} // namespace

TEST(CompareTests, lessThanTrue) {
    auto jit = CajetaJit::compile(makeSource("boolean", "return 3 < 5;"), "test.C");
    auto fn = jit->lookup<bool (*)()>("run");
    EXPECT_TRUE(fn());
}

TEST(CompareTests, lessThanFalse) {
    auto jit = CajetaJit::compile(makeSource("boolean", "return 5 < 3;"), "test.C");
    auto fn = jit->lookup<bool (*)()>("run");
    EXPECT_FALSE(fn());
}

TEST(CompareTests, lessOrEqual) {
    auto jit = CajetaJit::compile(makeSource("boolean",
        "int32 a = 5;\n"
        "int32 b = 5;\n"
        "return a <= b;"), "test.C");
    auto fn = jit->lookup<bool (*)()>("run");
    EXPECT_TRUE(fn());
}

TEST(CompareTests, greaterThan) {
    auto jit = CajetaJit::compile(makeSource("boolean", "return 10 > 4;"), "test.C");
    auto fn = jit->lookup<bool (*)()>("run");
    EXPECT_TRUE(fn());
}

TEST(CompareTests, greaterOrEqual) {
    auto jit = CajetaJit::compile(makeSource("boolean", "return 10 >= 10;"), "test.C");
    auto fn = jit->lookup<bool (*)()>("run");
    EXPECT_TRUE(fn());
}

TEST(CompareTests, equalIntsTrue) {
    auto jit = CajetaJit::compile(makeSource("boolean",
        "int32 a = 42;\n"
        "int32 b = 42;\n"
        "return a == b;"), "test.C");
    auto fn = jit->lookup<bool (*)()>("run");
    EXPECT_TRUE(fn());
}

TEST(CompareTests, equalIntsFalse) {
    auto jit = CajetaJit::compile(makeSource("boolean", "return 42 == 43;"), "test.C");
    auto fn = jit->lookup<bool (*)()>("run");
    EXPECT_FALSE(fn());
}

TEST(CompareTests, notEqualInts) {
    auto jit = CajetaJit::compile(makeSource("boolean", "return 42 != 43;"), "test.C");
    auto fn = jit->lookup<bool (*)()>("run");
    EXPECT_TRUE(fn());
}

TEST(CompareTests, floatGreater) {
    auto jit = CajetaJit::compile(makeSource("boolean",
        "float64 a = 3.14;\n"
        "float64 b = 2.71;\n"
        "return a > b;"), "test.C");
    auto fn = jit->lookup<bool (*)()>("run");
    EXPECT_TRUE(fn());
}

TEST(CompareTests, floatLessOrEqual) {
    auto jit = CajetaJit::compile(makeSource("boolean",
        "float64 a = 1.5;\n"
        "float64 b = 1.5;\n"
        "return a <= b;"), "test.C");
    auto fn = jit->lookup<bool (*)()>("run");
    EXPECT_TRUE(fn());
}

// --- logical &&/|| -------------------------------------------------------------

TEST(LogicalTests, andBothTrue) {
    auto jit = CajetaJit::compile(makeSource("boolean", "return true && true;"), "test.C");
    auto fn = jit->lookup<bool (*)()>("run");
    EXPECT_TRUE(fn());
}

TEST(LogicalTests, andLhsFalse) {
    auto jit = CajetaJit::compile(makeSource("boolean", "return false && true;"), "test.C");
    auto fn = jit->lookup<bool (*)()>("run");
    EXPECT_FALSE(fn());
}

TEST(LogicalTests, andRhsFalse) {
    auto jit = CajetaJit::compile(makeSource("boolean", "return true && false;"), "test.C");
    auto fn = jit->lookup<bool (*)()>("run");
    EXPECT_FALSE(fn());
}

TEST(LogicalTests, orBothFalse) {
    auto jit = CajetaJit::compile(makeSource("boolean", "return false || false;"), "test.C");
    auto fn = jit->lookup<bool (*)()>("run");
    EXPECT_FALSE(fn());
}

TEST(LogicalTests, orLhsTrue) {
    auto jit = CajetaJit::compile(makeSource("boolean", "return true || false;"), "test.C");
    auto fn = jit->lookup<bool (*)()>("run");
    EXPECT_TRUE(fn());
}

TEST(LogicalTests, orRhsTrue) {
    auto jit = CajetaJit::compile(makeSource("boolean", "return false || true;"), "test.C");
    auto fn = jit->lookup<bool (*)()>("run");
    EXPECT_TRUE(fn());
}

// Compound with comparisons.
TEST(LogicalTests, comparisonsCombined) {
    auto jit = CajetaJit::compile(makeSource("boolean",
        "int32 x = 5;\n"
        "return x > 0 && x < 10;"), "test.C");
    auto fn = jit->lookup<bool (*)()>("run");
    EXPECT_TRUE(fn());
}
