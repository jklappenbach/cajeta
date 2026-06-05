//
// cajeta.time.Instant + Clock — epoch round-trips, arithmetic, between, and the
// native clock binding. Validates a two-field (int64+int32) stack value type.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.time.Instant;\n"
           "import cajeta.time.Duration;\n"
           "import cajeta.time.Clock;\n"
           "public final class It {\n"
           "    public static int64 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

int64_t runJit(const std::string& body) {
    auto jit = CajetaJit::compile(makeSource(body), "test.It");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}

} // namespace

TEST(InstantTests, epochMilliRoundTrip) {
    EXPECT_EQ(runJit(
        "Instant i = Instant.ofEpochMilli(1500);\n"
        "return i.toEpochMilli();"), 1500LL);
}

TEST(InstantTests, epochSecondAndNano) {
    EXPECT_EQ(runJit(
        "Instant i = Instant.ofEpochMilli(1500);\n"
        "if (i.getEpochSecond()==1 && i.getNano()==500000000) return 1;\n"
        "return 0;"), 1LL);
}

TEST(InstantTests, negativeMilliNormalizes) {
    // -1 ms => epochSecond -1, nano 999000000
    EXPECT_EQ(runJit(
        "Instant i = Instant.ofEpochMilli(-1);\n"
        "if (i.getEpochSecond()==-1 && i.getNano()==999000000) return 1;\n"
        "return 0;"), 1LL);
}

TEST(InstantTests, plusNanosCarry) {
    EXPECT_EQ(runJit(
        "Instant i = Instant.ofEpochSecond(10);\n"
        "Instant j = i.plusNanos(2500000000);\n"  // 2.5s
        "if (j.getEpochSecond()==12 && j.getNano()==500000000) return 1;\n"
        "return 0;"), 1LL);
}

TEST(InstantTests, plusDuration) {
    EXPECT_EQ(runJit(
        "Instant i = Instant.ofEpochSecond(100);\n"
        "Duration d = Duration.ofSeconds(5);\n"
        "Instant j = i.plus(d);\n"
        "return j.getEpochSecond();"), 105LL);
}

TEST(InstantTests, betweenGivesDuration) {
    EXPECT_EQ(runJit(
        "Instant a = Instant.ofEpochSecond(100);\n"
        "Instant b = Instant.ofEpochSecond(130);\n"
        "Duration d = Instant.between(a, b);\n"
        "return d.toSeconds();"), 30LL);
}

TEST(InstantTests, compareAndEquals) {
    EXPECT_EQ(runJit(
        "Instant a = Instant.ofEpochMilli(1000);\n"
        "Instant b = Instant.ofEpochSecond(1);\n"
        "if (a == b) return 1;\n"
        "return 0;"), 1LL);
    EXPECT_EQ(runJit(
        "Instant a = Instant.ofEpochSecond(1);\n"
        "Instant b = Instant.ofEpochSecond(2);\n"
        "if (a.isBefore(b)) return 1;\n"
        "return 0;"), 1LL);
}

TEST(ClockTests, nanoTimeMonotonicNonZero) {
    EXPECT_EQ(runJit(
        "int64 a = Clock.nanoTime();\n"
        "int64 b = Clock.nanoTime();\n"
        "if (b >= a && a > 0) return 1;\n"
        "return 0;"), 1LL);
}

TEST(ClockTests, millisTimeReasonable) {
    // After 2020-01-01 (1577836800000 ms). Sanity check the wall clock binding.
    EXPECT_EQ(runJit(
        "int64 ms = Clock.millisTime();\n"
        "if (ms > 1577836800000) return 1;\n"
        "return 0;"), 1LL);
}

TEST(ClockTests, nowMatchesMillisRoughly) {
    EXPECT_EQ(runJit(
        "Instant i = Clock.now();\n"
        "if (i.getEpochSecond() > 1577836800) return 1;\n"
        "return 0;"), 1LL);
}
