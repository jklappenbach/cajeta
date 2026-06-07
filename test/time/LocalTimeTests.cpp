//
// cajeta.time.LocalTime — construction/validation, nano-of-day math, toString.
// Also validates the cajeta.time.Fmt byte-formatting helper end to end.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

std::string makeSource(const std::string& body) {
    return "package test;\n"
           "import cajeta.time.LocalTime;\n"
           "import cajeta.time.DateTimeException;\n"
           "import cajeta.lang.String;\n"
           "public final class Ltt {\n"
           "    public static int64 run() {\n"
           "        " + body + "\n"
           "    }\n"
           "}\n";
}

int64_t runJit(const std::string& body) {
    auto jit = CajetaJit::compile(makeSource(body), "test.Ltt");
    auto fn = jit->lookup<int64_t (*)()>("run");
    return fn();
}

} // namespace

TEST(LocalTimeTests, nanoOfDayRoundTrip) {
    EXPECT_EQ(runJit(
        "LocalTime t = LocalTime.ofHms(1, 2, 3);\n"
        "return t.toNanoOfDay();"),
        (1LL * 3600 + 2 * 60 + 3) * 1000000000LL);
}

TEST(LocalTimeTests, ofNanoOfDayInverse) {
    EXPECT_EQ(runJit(
        "LocalTime t = LocalTime.ofNanoOfDay(45296000000007);\n"
        "if (t.getHour()==12 && t.getMinute()==34 && t.getSecond()==56 && t.getNano()==7) return 1;\n"
        "return 0;"), 1LL);
}

TEST(LocalTimeTests, plusHoursWrapsDay) {
    EXPECT_EQ(runJit(
        "LocalTime t = LocalTime.of(23, 30);\n"
        "LocalTime u = t.plusHours(2);\n"
        "if (u.getHour()==1 && u.getMinute()==30) return 1;\n"
        "return 0;"), 1LL);
}

TEST(LocalTimeTests, plusNanosWrapsNegative) {
    EXPECT_EQ(runJit(
        "LocalTime t = LocalTime.midnight();\n"
        "LocalTime u = t.plusSeconds(-1);\n"
        "if (u.getHour()==23 && u.getMinute()==59 && u.getSecond()==59) return 1;\n"
        "return 0;"), 1LL);
}

TEST(LocalTimeTests, compareAndEquals) {
    EXPECT_EQ(runJit(
        "LocalTime a = LocalTime.of(8, 0);\n"
        "LocalTime b = LocalTime.of(9, 0);\n"
        "return (int64) a.compareTo(b);"), -1LL);
    EXPECT_EQ(runJit(
        "LocalTime a = LocalTime.ofHms(8, 0, 0);\n"
        "LocalTime b = LocalTime.of(8, 0);\n"
        "if (a == b) return 1;\n"
        "return 0;"), 1LL);
}

TEST(LocalTimeTests, invalidHourThrowsIsCaught) {
    // The validation throw is a RecoverableException; a catch handler must see it.
    EXPECT_EQ(runJit(
        "try {\n"
        "    LocalTime t = LocalTime.of(24, 0);\n"
        "    return 0;\n"
        "} catch (DateTimeException e) {\n"
        "    return 1;\n"
        "}"), 1LL);
}

TEST(LocalTimeTests, isoHmShort) {
    // "08:05" -> size 5, chars '0','8',':','0','5'
    EXPECT_EQ(runJit(
        "LocalTime t = LocalTime.of(8, 5);\n"
        "String s = t.iso();\n"
        "if (s.size()==5 && s.charAt(0)==48 && s.charAt(1)==56 && s.charAt(2)==58 "
        "&& s.charAt(3)==48 && s.charAt(4)==53) return 1;\n"
        "return 0;"), 1LL);
}

TEST(LocalTimeTests, isoWithSeconds) {
    // "23:09:07" -> size 8
    EXPECT_EQ(runJit(
        "LocalTime t = LocalTime.ofHms(23, 9, 7);\n"
        "String s = t.iso();\n"
        "if (s.size()==8 && s.charAt(5)==58 && s.charAt(6)==48 && s.charAt(7)==55) return 1;\n"
        "return 0;"), 1LL);
}

TEST(LocalTimeTests, isoWithNanos) {
    // "00:00:00.000000009" -> size 18, '.' at 8, '9' at 17
    EXPECT_EQ(runJit(
        "LocalTime t = LocalTime.ofHmsn(0, 0, 0, 9);\n"
        "String s = t.iso();\n"
        "if (s.size()==18 && s.charAt(8)==46 && s.charAt(17)==57) return 1;\n"
        "return 0;"), 1LL);
}
