//
// cajeta.time — LocalDateTime, Period, ZoneOffset, ZonedDateTime.
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
           "import cajeta.time.Instant;\n"
           "import cajeta.time.Period;\n"
           "import cajeta.time.ZoneOffset;\n"
           "import cajeta.time.ZonedDateTime;\n"
           "import cajeta.lang.String;\n"
           "public final class Dtz {\n"
           "    public static int64 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

int64_t runJit(const std::string& body) {
    auto jit = CajetaJit::compile(makeSource(body), "test.Dtz");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}

} // namespace

// ---- LocalDateTime ----

TEST(LocalDateTimeTests, ofAndGetters) {
    EXPECT_EQ(runJit(
        "LocalDateTime t = LocalDateTime.ofFull(2026,6,5, 14,30,15, 7);\n"
        "if (t.getYear()==2026 && t.getMonthValue()==6 && t.getDayOfMonth()==5 "
        "&& t.getHour()==14 && t.getMinute()==30 && t.getSecond()==15 && t.getNano()==7) return 1;\n"
        "return 0;"), 1LL);
}

TEST(LocalDateTimeTests, plusHoursCarriesDate) {
    // 2026-06-05 23:30 + 2h -> 2026-06-06 01:30
    EXPECT_EQ(runJit(
        "LocalDateTime t = LocalDateTime.of(2026,6,5, 23,30);\n"
        "LocalDateTime u = t.plusHours(2);\n"
        "if (u.getDayOfMonth()==6 && u.getHour()==1 && u.getMinute()==30) return 1;\n"
        "return 0;"), 1LL);
}

TEST(LocalDateTimeTests, toInstantAtOffset) {
    // 1970-01-01T00:00:00 at +00:00 -> epochSecond 0
    EXPECT_EQ(runJit(
        "LocalDateTime t = LocalDateTime.of(1970,1,1, 0,0);\n"
        "ZoneOffset z = ZoneOffset.utc();\n"
        "Instant i = t.toInstant(z);\n"
        "return i.getEpochSecond();"), 0LL);
    // 1970-01-01T00:00:00 at +01:00 -> instant is one hour earlier: -3600
    EXPECT_EQ(runJit(
        "LocalDateTime t = LocalDateTime.of(1970,1,1, 0,0);\n"
        "ZoneOffset z = ZoneOffset.ofHours(1);\n"
        "Instant i = t.toInstant(z);\n"
        "return i.getEpochSecond();"), -3600LL);
}

TEST(LocalDateTimeTests, isoFormat) {
    // "2026-06-05T14:30:15" -> size 19, 'T' at 10
    EXPECT_EQ(runJit(
        "LocalDateTime t = LocalDateTime.ofFull(2026,6,5, 14,30,15, 0);\n"
        "String s = t.iso();\n"
        "if (s.size()==19 && s.charAt(10)==84) return 1;\n"
        "return 0;"), 1LL);
}

// ---- Period ----

TEST(PeriodTests, fieldsAndTotalMonths) {
    EXPECT_EQ(runJit("return Period.of(1,2,3).toTotalMonths();"), 14LL);
}

TEST(PeriodTests, normalized) {
    // 14 months -> 1 year 2 months
    EXPECT_EQ(runJit(
        "Period p = Period.ofMonths(14);\n"
        "Period n = p.normalized();\n"
        "if (n.getYears()==1 && n.getMonths()==2) return 1;\n"
        "return 0;"), 1LL);
}

TEST(PeriodTests, isoFormat) {
    // P1Y2M3D
    EXPECT_EQ(runJit(
        "Period p = Period.of(1,2,3);\n"
        "String s = p.iso();\n"
        "if (s.size()==7 && s.charAt(0)==80 && s.charAt(2)==89 && s.charAt(6)==68) return 1;\n"
        "return 0;"), 1LL);
}

TEST(LocalDatePeriodTests, plusPeriod) {
    // 2026-01-31 + P1M -> 2026-02-28 (clamped)
    EXPECT_EQ(runJit(
        "LocalDate d = LocalDate.of(2026,1,31);\n"
        "Period p = Period.ofMonths(1);\n"
        "LocalDate e = d.plus(p);\n"
        "if (e.getYear()==2026 && e.getMonthValue()==2 && e.getDayOfMonth()==28) return 1;\n"
        "return 0;"), 1LL);
}

// ---- ZoneOffset ----

TEST(ZoneOffsetTests, hoursMinutes) {
    EXPECT_EQ(runJit("return (int64) ZoneOffset.ofHoursMinutes(5,30).getTotalSeconds();"),
              5 * 3600 + 30 * 60);
    EXPECT_EQ(runJit("return (int64) ZoneOffset.ofHours(-8).getTotalSeconds();"), -8 * 3600);
}

TEST(ZoneOffsetTests, isoUtcAndOffset) {
    EXPECT_EQ(runJit(
        "ZoneOffset z = ZoneOffset.utc();\n"
        "String s = z.iso();\n"
        "if (s.size()==1 && s.charAt(0)==90) return 1;\n"  // 'Z'
        "return 0;"), 1LL);
    EXPECT_EQ(runJit(
        "ZoneOffset z = ZoneOffset.ofHoursMinutes(5,30);\n"
        "String s = z.iso();\n"
        "if (s.size()==6 && s.charAt(0)==43 && s.charAt(3)==58) return 1;\n"  // +05:30
        "return 0;"), 1LL);
}

TEST(ZoneOffsetTests, outOfRangeThrows) {
    EXPECT_EQ(runJit(
        "try { ZoneOffset z = ZoneOffset.ofHours(19); return 0; }\n"
        "catch (cajeta.time.DateTimeException e) { return 1; }"), 1LL);
}

// ---- ZonedDateTime ----

TEST(ZonedDateTimeTests, instantRoundTrip) {
    EXPECT_EQ(runJit(
        "Instant i = Instant.ofEpochSecond(1000);\n"
        "ZoneOffset z = ZoneOffset.ofHours(2);\n"
        "ZonedDateTime zdt = ZonedDateTime.ofInstant(i, z);\n"
        "Instant j = zdt.toInstant();\n"
        "return j.getEpochSecond();"), 1000LL);
}

TEST(ZonedDateTimeTests, localShiftedByOffset) {
    // instant epoch 0 at +02:00 -> local 1970-01-01T02:00
    EXPECT_EQ(runJit(
        "Instant i = Instant.ofEpochSecond(0);\n"
        "ZoneOffset z = ZoneOffset.ofHours(2);\n"
        "ZonedDateTime zdt = ZonedDateTime.ofInstant(i, z);\n"
        "if (zdt.getYear()==1970 && zdt.getMonthValue()==1 && zdt.getDayOfMonth()==1 "
        "&& zdt.getHour()==2) return 1;\n"
        "return 0;"), 1LL);
}

TEST(ZonedDateTimeTests, ofLocalThenInstant) {
    // local 1970-01-01T00:00 at +00:00 -> instant epoch 0
    EXPECT_EQ(runJit(
        "LocalDateTime t = LocalDateTime.of(1970,1,1, 0,0);\n"
        "ZoneOffset z = ZoneOffset.utc();\n"
        "ZonedDateTime zdt = ZonedDateTime.ofLocal(t, z);\n"
        "Instant i = zdt.toInstant();\n"
        "return i.getEpochSecond();"), 0LL);
}

TEST(ZonedDateTimeTests, isoHasOffset) {
    // 1970-01-01T02:00:00+02:00
    EXPECT_EQ(runJit(
        "Instant i = Instant.ofEpochSecond(0);\n"
        "ZoneOffset z = ZoneOffset.ofHours(2);\n"
        "ZonedDateTime zdt = ZonedDateTime.ofInstant(i, z);\n"
        "String s = zdt.iso();\n"
        "if (s.charAt(s.size()-6)==43 && s.charAt(s.size()-3)==58) return 1;\n"  // +02:00
        "return 0;"), 1LL);
}
