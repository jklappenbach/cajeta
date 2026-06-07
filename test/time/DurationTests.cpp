//
// cajeta.time.Duration — arithmetic, ordering, operators.
//
// Also the toolchain-validation slice for the cajeta.time work: it exercises a
// stdlib value type that `implements Comparable<Duration>`, returns `stack`
// values from factories, and overloads `operator==` / `operator+`.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.time.Duration;\n"
           "public final class Dt {\n"
           "    public static int64 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

int64_t runJit(const std::string& body) {
    auto jit = CajetaJit::compile(makeSource(body), "test.Dt");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}

} // namespace

TEST(DurationTests, ofSecondsToNanos) {
    EXPECT_EQ(runJit(
        "Duration d = Duration.ofSeconds(2);\n"
        "return d.toNanos();"), 2000000000LL);
}

TEST(DurationTests, unitConversions) {
    EXPECT_EQ(runJit(
        "Duration d = Duration.ofHours(1);\n"
        "return d.toMinutes();"), 60LL);
    EXPECT_EQ(runJit(
        "Duration d = Duration.ofDays(1);\n"
        "return d.toHours();"), 24LL);
}

TEST(DurationTests, plusMinus) {
    EXPECT_EQ(runJit(
        "Duration a = Duration.ofSeconds(5);\n"
        "Duration b = Duration.ofSeconds(3);\n"
        "Duration c = a.minus(b);\n"
        "return c.toSeconds();"), 2LL);
}

TEST(DurationTests, negatedAndAbs) {
    EXPECT_EQ(runJit(
        "Duration a = Duration.ofSeconds(7);\n"
        "Duration n = a.negated();\n"
        "Duration b = n.abs();\n"
        "return b.toSeconds();"), 7LL);
}

TEST(DurationTests, isNegative) {
    EXPECT_EQ(runJit(
        "Duration a = Duration.ofSeconds(-1);\n"
        "if (a.isNegative()) return 1;\n"
        "return 0;"), 1LL);
}

TEST(DurationTests, compareTo) {
    EXPECT_EQ(runJit(
        "Duration a = Duration.ofSeconds(1);\n"
        "Duration b = Duration.ofSeconds(2);\n"
        "return (int64) a.compareTo(b);"), -1LL);
    EXPECT_EQ(runJit(
        "Duration a = Duration.ofSeconds(2);\n"
        "Duration b = Duration.ofSeconds(2);\n"
        "return (int64) a.compareTo(b);"), 0LL);
}

TEST(DurationTests, operatorEquals) {
    EXPECT_EQ(runJit(
        "Duration a = Duration.ofMillis(1000);\n"
        "Duration b = Duration.ofSeconds(1);\n"
        "if (a == b) return 1;\n"
        "return 0;"), 1LL);
}

TEST(DurationTests, plusInstanceMethod) {
    EXPECT_EQ(runJit(
        "Duration a = Duration.ofSeconds(4);\n"
        "Duration b = Duration.ofSeconds(6);\n"
        "Duration c = a.plus(b);\n"
        "return c.toSeconds();"), 10LL);
}
