//
// cajeta.time.DateTimeFormatter — strftime ofPattern + FormatStyle standards.
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
           "import cajeta.time.LocalTime;\n"
           "import cajeta.time.LocalDateTime;\n"
           "import cajeta.time.ZonedDateTime;\n"
           "import cajeta.time.Instant;\n"
           "import cajeta.time.ZoneOffset;\n"
           "import cajeta.time.DateTimeFormatter;\n"
           "import cajeta.time.FormatStyle;\n"
           "import cajeta.lang.String;\n"
           "public final class Fmtt {\n"
           "    public static int64 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

int64_t runJit(const std::string& body) {
    auto jit = CajetaJit::compile(makeSource(body), "test.Fmtt");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}

std::string expectEq(const std::string& setup, const std::string& expect) {
    std::string checks = "if (s.size()==" + std::to_string(expect.size());
    for (size_t i = 0; i < expect.size(); ++i) {
        checks += " && s.charAt(" + std::to_string(i) + ")==" +
                  std::to_string((int) (unsigned char) expect[i]);
    }
    checks += ") return 1;\nreturn 0;";
    return setup + "\n" + checks;
}

} // namespace

// ---- strftime patterns ----

TEST(DateTimeFormatterPatternTests, ymdHms) {
    std::string setup =
        "LocalDateTime t = LocalDateTime.ofFull(2026,6,5, 14,3,9, 0);\n"
        "DateTimeFormatter f = DateTimeFormatter.ofPattern(\"%Y-%m-%d %H:%M:%S\");\n"
        "String s = f.formatDateTime(t);";
    EXPECT_EQ(runJit(expectEq(setup, "2026-06-05 14:03:09")), 1LL);
}

TEST(DateTimeFormatterPatternTests, namesAndPercentLiteral) {
    std::string setup =
        "LocalDate d = LocalDate.of(2026,6,5);\n"
        "DateTimeFormatter f = DateTimeFormatter.ofPattern(\"%a %d %b %Y 100%%\");\n"
        "String s = f.formatDate(d);";
    EXPECT_EQ(runJit(expectEq(setup, "Fri 05 Jun 2026 100%")), 1LL);
}

TEST(DateTimeFormatterPatternTests, twelveHourAndMillis) {
    std::string setup =
        "LocalTime t = LocalTime.ofHmsn(14,3,9, 123456789);\n"
        "DateTimeFormatter f = DateTimeFormatter.ofPattern(\"%I:%M:%S.%L %p\");\n"
        "String s = f.formatTime(t);";
    EXPECT_EQ(runJit(expectEq(setup, "02:03:09.123 PM")), 1LL);
}

TEST(DateTimeFormatterPatternTests, fullMonthAndWeekday) {
    std::string setup =
        "LocalDate d = LocalDate.of(2026,6,5);\n"
        "DateTimeFormatter f = DateTimeFormatter.ofPattern(\"%A, %B %d\");\n"
        "String s = f.formatDate(d);";
    EXPECT_EQ(runJit(expectEq(setup, "Friday, June 05")), 1LL);
}

TEST(DateTimeFormatterPatternTests, offsetForms) {
    std::string setup =
        "Instant i = Instant.ofEpochSecond(0);\n"
        "ZoneOffset z = ZoneOffset.ofHoursMinutes(5,30);\n"
        "ZonedDateTime zdt = ZonedDateTime.ofInstant(i, z);\n"
        "DateTimeFormatter f = DateTimeFormatter.ofPattern(\"%H:%M %z %Z\");\n"
        "String s = f.formatZoned(zdt);";
    EXPECT_EQ(runJit(expectEq(setup, "05:30 +0530 +05:30")), 1LL);
}

// ---- standard styles ----

TEST(DateTimeFormatterStandardTests, isoLocalDateTime) {
    std::string setup =
        "LocalDateTime t = LocalDateTime.ofFull(2026,6,5, 14,3,9, 0);\n"
        "DateTimeFormatter f = DateTimeFormatter.ofStandard(FormatStyle.ISO_LOCAL_DATE_TIME);\n"
        "String s = f.formatDateTime(t);";
    EXPECT_EQ(runJit(expectEq(setup, "2026-06-05T14:03:09")), 1LL);
}

TEST(DateTimeFormatterStandardTests, isoLocalDate) {
    std::string setup =
        "LocalDate d = LocalDate.of(2026,6,5);\n"
        "DateTimeFormatter f = DateTimeFormatter.ofStandard(FormatStyle.ISO_LOCAL_DATE);\n"
        "String s = f.formatDate(d);";
    EXPECT_EQ(runJit(expectEq(setup, "2026-06-05")), 1LL);
}

TEST(DateTimeFormatterStandardTests, basicIsoDate) {
    std::string setup =
        "LocalDate d = LocalDate.of(2026,6,5);\n"
        "DateTimeFormatter f = DateTimeFormatter.ofStandard(FormatStyle.BASIC_ISO_DATE);\n"
        "String s = f.formatDate(d);";
    EXPECT_EQ(runJit(expectEq(setup, "20260605")), 1LL);
}

TEST(DateTimeFormatterStandardTests, rfc1123) {
    std::string setup =
        "LocalDateTime t = LocalDateTime.ofFull(2026,6,5, 14,3,9, 0);\n"
        "DateTimeFormatter f = DateTimeFormatter.ofStandard(FormatStyle.RFC_1123_DATE_TIME);\n"
        "String s = f.formatDateTime(t);";
    EXPECT_EQ(runJit(expectEq(setup, "Fri, 05 Jun 2026 14:03:09 GMT")), 1LL);
}

TEST(DateTimeFormatterStandardTests, usEuroSql) {
    EXPECT_EQ(runJit(expectEq(
        "LocalDate d = LocalDate.of(2026,6,5);\n"
        "DateTimeFormatter f = DateTimeFormatter.ofStandard(FormatStyle.US_DATE);\n"
        "String s = f.formatDate(d);", "06/05/2026")), 1LL);
    EXPECT_EQ(runJit(expectEq(
        "LocalDate d = LocalDate.of(2026,6,5);\n"
        "DateTimeFormatter f = DateTimeFormatter.ofStandard(FormatStyle.EURO_DATE);\n"
        "String s = f.formatDate(d);", "05.06.2026")), 1LL);
    EXPECT_EQ(runJit(expectEq(
        "LocalDateTime t = LocalDateTime.ofFull(2026,6,5, 14,3,9, 0);\n"
        "DateTimeFormatter f = DateTimeFormatter.ofStandard(FormatStyle.SQL_TIMESTAMP);\n"
        "String s = f.formatDateTime(t);", "2026-06-05 14:03:09")), 1LL);
}

TEST(DateTimeFormatterStandardTests, isoInstantHasZ) {
    std::string setup =
        "Instant i = Instant.ofEpochSecond(0);\n"
        "ZoneOffset z = ZoneOffset.utc();\n"
        "ZonedDateTime zdt = ZonedDateTime.ofInstant(i, z);\n"
        "DateTimeFormatter f = DateTimeFormatter.ofStandard(FormatStyle.ISO_INSTANT);\n"
        "String s = f.formatZoned(zdt);";
    EXPECT_EQ(runJit(expectEq(setup, "1970-01-01T00:00:00Z")), 1LL);
}
