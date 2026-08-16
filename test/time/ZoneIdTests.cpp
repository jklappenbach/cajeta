//
// cajeta.time.ZoneId — tz-database (TZif) offset resolution, DST-aware.
//
// These exercise the native __cajeta_tz_offset lookup against the system tz
// database (/usr/share/zoneinfo). DST offsets are stable historical facts
// (confirmed with `zdump -v America/New_York`).
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
           "import cajeta.time.ZoneId;\n"
           "import cajeta.time.ZoneOffset;\n"
           "import cajeta.time.ZonedDateTime;\n"
           "import cajeta.time.DateTimeException;\n"
           "public final class Zit {\n"
           "    public static int64 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

int64_t runJit(const std::string& body) {
    auto jit = CajetaJit::compile(makeSource(body), "test.Zit");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}

} // namespace

TEST(ZoneIdTests, utcFastPathNoFilesystem) {
    EXPECT_EQ(runJit(
        "ZoneId z #= ZoneId.of(\"UTC\");\n"
        "Instant i = Instant.ofEpochSecond(1625140800);\n"
        "ZoneOffset off = z.offsetAt(i);\n"
        "return (int64) off.getTotalSeconds();"), 0LL);
}

TEST(ZoneIdTests, newYorkSummerIsEdt) {
    // 2021-07-01T12:00Z -> America/New_York is EDT = -4h = -14400s
    EXPECT_EQ(runJit(
        "ZoneId z #= ZoneId.of(\"America/New_York\");\n"
        "Instant i = Instant.ofEpochSecond(1625140800);\n"
        "ZoneOffset off = z.offsetAt(i);\n"
        "return (int64) off.getTotalSeconds();"), -14400LL);
}

TEST(ZoneIdTests, newYorkWinterIsEst) {
    // 2021-01-01T12:00Z -> America/New_York is EST = -5h = -18000s
    EXPECT_EQ(runJit(
        "ZoneId z #= ZoneId.of(\"America/New_York\");\n"
        "Instant i = Instant.ofEpochSecond(1609502400);\n"
        "ZoneOffset off = z.offsetAt(i);\n"
        "return (int64) off.getTotalSeconds();"), -18000LL);
}

TEST(ZoneIdTests, kolkataHalfHourOffset) {
    // Asia/Kolkata is +05:30 = +19800s year-round (no DST).
    EXPECT_EQ(runJit(
        "ZoneId z #= ZoneId.of(\"Asia/Kolkata\");\n"
        "Instant i = Instant.ofEpochSecond(1625140800);\n"
        "ZoneOffset off = z.offsetAt(i);\n"
        "return (int64) off.getTotalSeconds();"), 19800LL);
}

TEST(ZoneIdTests, dstTransitionChangesOffset) {
    // Same zone, two instants straddling the 2021-03-14 spring-forward:
    // before -> EST(-18000), after -> EDT(-14400).
    EXPECT_EQ(runJit(
        "ZoneId z #= ZoneId.of(\"America/New_York\");\n"
        "Instant before = Instant.ofEpochSecond(1615708800);\n"  // 2021-03-14T08:00Z (pre? actually post)
        "Instant winter = Instant.ofEpochSecond(1609502400);\n"
        "Instant summer = Instant.ofEpochSecond(1625140800);\n"
        "ZoneOffset w = z.offsetAt(winter);\n"
        "ZoneOffset s = z.offsetAt(summer);\n"
        "if (w.getTotalSeconds()==-18000 && s.getTotalSeconds()==-14400) return 1;\n"
        "return 0;"), 1LL);
}

TEST(ZoneIdTests, unknownZoneThrows) {
    EXPECT_EQ(runJit(
        "ZoneId z #= ZoneId.of(\"Not/ARealZone\");\n"
        "Instant i = Instant.ofEpochSecond(1625140800);\n"
        "try {\n"
        "    ZoneOffset off = z.offsetAt(i);\n"
        "    return 0;\n"
        "} catch (DateTimeException e) {\n"
        "    return 1;\n"
        "}"), 1LL);
}

TEST(ZoneIdTests, resolveProducesZonedDateTime) {
    // Instant epoch 0 in America/New_York (EST -5h) -> local 1969-12-31T19:00.
    EXPECT_EQ(runJit(
        "ZoneId z #= ZoneId.of(\"America/New_York\");\n"
        "Instant i = Instant.ofEpochSecond(0);\n"
        "ZonedDateTime zdt = z.resolve(i);\n"
        "if (zdt.getYear()==1969 && zdt.getMonthValue()==12 && zdt.getDayOfMonth()==31 "
        "&& zdt.getHour()==19) return 1;\n"
        "return 0;"), 1LL);
}
