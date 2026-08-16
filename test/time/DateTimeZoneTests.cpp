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





// ---- Period ----





// ---- ZoneOffset ----

TEST(ZoneOffsetTests, hoursMinutes) {
    EXPECT_EQ(runJit("return (int64) ZoneOffset.ofHoursMinutes(5,30).getTotalSeconds();"),
              5 * 3600 + 30 * 60);
    EXPECT_EQ(runJit("return (int64) ZoneOffset.ofHours(-8).getTotalSeconds();"), -8 * 3600);
}

TEST(ZoneOffsetTests, isoUtcAndOffset) {
    EXPECT_EQ(runJit(
        "ZoneOffset z = ZoneOffset.utc();\n"
        "String s #= z.iso();\n"
        "if (s.size()==1 && s.charAt(0)==90) return 1;\n"  // 'Z'
        "return 0;"), 1LL);
    EXPECT_EQ(runJit(
        "ZoneOffset z = ZoneOffset.ofHoursMinutes(5,30);\n"
        "String s #= z.iso();\n"
        "if (s.size()==6 && s.charAt(0)==43 && s.charAt(3)==58) return 1;\n"  // +05:30
        "return 0;"), 1LL);
}

TEST(ZoneOffsetTests, outOfRangeThrows) {
    EXPECT_EQ(runJit(
        "try { ZoneOffset z = ZoneOffset.ofHours(19); return 0; }\n"
        "catch (cajeta.time.DateTimeException e) { return 1; }"), 1LL);
}

// ---- ZonedDateTime ----




