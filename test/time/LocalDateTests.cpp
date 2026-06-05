//
// cajeta.time.LocalDate — civil<->epoch-day algorithms, validation, arithmetic.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.time.LocalDate;\n"
           "import cajeta.time.DateTimeException;\n"
           "import cajeta.lang.String;\n"
           "public final class Ldt {\n"
           "    public static int64 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

int64_t runJit(const std::string& body) {
    auto jit = CajetaJit::compile(makeSource(body), "test.Ldt");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}

// Reference (from Hinnant's tables / known facts):
//  1970-01-01 -> 0, 2000-01-01 -> 10957, 1969-12-31 -> -1,
//  2026-06-05 -> 20609, 0000-03-01 -> -719468, 1600-02-29 -> -135080
} // namespace

TEST(LocalDateTests, epochDayKnownValues) {
    EXPECT_EQ(runJit("return LocalDate.of(1970,1,1).toEpochDay();"), 0LL);
    EXPECT_EQ(runJit("return LocalDate.of(2000,1,1).toEpochDay();"), 10957LL);
    EXPECT_EQ(runJit("return LocalDate.of(1969,12,31).toEpochDay();"), -1LL);
    EXPECT_EQ(runJit("return LocalDate.of(2026,6,5).toEpochDay();"), 20609LL);
}

TEST(LocalDateTests, ofEpochDayInverse) {
    EXPECT_EQ(runJit(
        "LocalDate d = LocalDate.ofEpochDay(20609);\n"
        "if (d.getYear()==2026 && d.getMonthValue()==6 && d.getDayOfMonth()==5) return 1;\n"
        "return 0;"), 1LL);
}

TEST(LocalDateTests, negativeEpochRoundTrip) {
    EXPECT_EQ(runJit(
        "LocalDate d = LocalDate.of(1900,2,28);\n"
        "LocalDate e = LocalDate.ofEpochDay(d.toEpochDay());\n"
        "if (e.getYear()==1900 && e.getMonthValue()==2 && e.getDayOfMonth()==28) return 1;\n"
        "return 0;"), 1LL);
}

TEST(LocalDateTests, leapYearRules) {
    EXPECT_EQ(runJit("if (LocalDate.isLeapYear(2000)) return 1; return 0;"), 1LL);  // div 400
    EXPECT_EQ(runJit("if (LocalDate.isLeapYear(1900)) return 1; return 0;"), 0LL);  // div 100 not 400
    EXPECT_EQ(runJit("if (LocalDate.isLeapYear(2024)) return 1; return 0;"), 1LL);  // div 4
    EXPECT_EQ(runJit("if (LocalDate.isLeapYear(2023)) return 1; return 0;"), 0LL);
}

TEST(LocalDateTests, lengthOfMonth) {
    EXPECT_EQ(runJit("return (int64) LocalDate.lengthOfMonth(2024,2);"), 29LL);
    EXPECT_EQ(runJit("return (int64) LocalDate.lengthOfMonth(2023,2);"), 28LL);
    EXPECT_EQ(runJit("return (int64) LocalDate.lengthOfMonth(2024,4);"), 30LL);
}

TEST(LocalDateTests, dayOfWeek) {
    // 1970-01-01 was a Thursday (ISO 4); 2026-06-05 is a Friday (ISO 5).
    EXPECT_EQ(runJit("return (int64) LocalDate.of(1970,1,1).getDayOfWeek();"), 4LL);
    EXPECT_EQ(runJit("return (int64) LocalDate.of(2026,6,5).getDayOfWeek();"), 5LL);
}

TEST(LocalDateTests, invalidDateThrows) {
    EXPECT_EQ(runJit(
        "try { LocalDate d = LocalDate.of(2023,2,29); return 0; }\n"
        "catch (DateTimeException e) { return 1; }"), 1LL);
    EXPECT_EQ(runJit(
        "try { LocalDate d = LocalDate.of(2024,2,29); return 0; }\n"   // valid leap day
        "catch (DateTimeException e) { return 1; }"), 0LL);
}

TEST(LocalDateTests, plusDaysAcrossMonth) {
    EXPECT_EQ(runJit(
        "LocalDate d = LocalDate.of(2026,1,31).plusDays(1);\n"
        "if (d.getMonthValue()==2 && d.getDayOfMonth()==1) return 1;\n"
        "return 0;"), 1LL);
}

TEST(LocalDateTests, plusMonthsClampsDay) {
    // Jan 31 + 1 month -> Feb 28 (2026 not leap)
    EXPECT_EQ(runJit(
        "LocalDate d = LocalDate.of(2026,1,31).plusMonths(1);\n"
        "if (d.getMonthValue()==2 && d.getDayOfMonth()==28) return 1;\n"
        "return 0;"), 1LL);
}

TEST(LocalDateTests, plusYearsLeapClamp) {
    // 2024-02-29 + 1 year -> 2025-02-28
    EXPECT_EQ(runJit(
        "LocalDate d = LocalDate.of(2024,2,29).plusYears(1);\n"
        "if (d.getYear()==2025 && d.getMonthValue()==2 && d.getDayOfMonth()==28) return 1;\n"
        "return 0;"), 1LL);
}

TEST(LocalDateTests, isoFormat) {
    // "2026-06-05" size 10
    EXPECT_EQ(runJit(
        "String s = LocalDate.of(2026,6,5).iso();\n"
        "if (s.size()==10 && s.charAt(4)==45 && s.charAt(7)==45 && s.charAt(0)==50) return 1;\n"
        "return 0;"), 1LL);
}

TEST(LocalDateTests, compareAndEquals) {
    EXPECT_EQ(runJit(
        "LocalDate a = LocalDate.of(2026,6,5);\n"
        "LocalDate b = LocalDate.ofEpochDay(20609);\n"
        "if (a == b) return 1;\n"
        "return 0;"), 1LL);
    EXPECT_EQ(runJit(
        "LocalDate a = LocalDate.of(2026,1,1);\n"
        "LocalDate b = LocalDate.of(2026,12,31);\n"
        "if (a.isBefore(b)) return 1;\n"
        "return 0;"), 1LL);
}
