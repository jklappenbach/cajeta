//
// stdlib-ownership-convention U4 — pinning tests for the non-exception
// migrations (4.1.4): Match's key transfer, ZoneId's id copy, and
// DateTimeFormatter's pattern copy. Data-survival shape throughout
// (see ExceptionMessageOwnershipTests.cpp for the churn rationale:
// both the String object and its >SSO root must be recycled).
//
// Red-first status (2026-08-18): Match was a LIVE defect — the ctor
// doc said "ownership transfers in" while the plain formal + plain
// store freed the `#`-passed key at ctor exit (found by the Unit 1
// audit's disposition pass). ZoneId/DateTimeFormatter plain-stored a
// borrow of their id/pattern.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn ? fn() : -1;
}

std::string churn() {
    return
        "        int32 j = 0;\n"
        "        while (j < 8) {\n"
        "            int8[] c = heap int8[32];\n"
        "            int32 k = 0;\n"
        "            while (k < 32) { c[k] = (int8) 90; k = k + 1; }\n"
        "            String cs = \"ZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZZ\" + j;\n"
        "            if (cs.byteLength() < 1) { return -9; }\n"
        "            j = j + 1;\n"
        "        }\n";
}

}  // namespace

// Match(T, #String, int32): the `#`-passed key is OWNED by the match —
// the exact Matcher `#kc` call shape, surviving the source frame.
TEST(OwnershipMigrationPinTests, matchOwnsItsTransferredKey) {
    std::string src =
        "package test;\n"
        "import cajeta.search.fuzzy.Match;\n"
        "public final class D {\n"
        "    static #Match<String> make(int32 i) {\n"
        "        String kc #= \"0123456789012345678901234567890\" + i;\n"
        "        Match<String> m #= heap Match<String>(\"v\", #kc, 0);\n"
        "        return #= m;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        Match<String> m #= D.make(7);\n" +
        churn() +
        "        String key = m.key();\n"
        "        if (key == null) { return -2; }\n"
        "        if (key.equals(\"01234567890123456789012345678907\")) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(1, runI32(src));
}

// ZoneId(String): the id is COPIED — the zone outlives its source.
TEST(OwnershipMigrationPinTests, zoneIdCopiesItsId) {
    std::string src =
        "package test;\n"
        "import cajeta.time.ZoneId;\n"
        "public final class D {\n"
        "    static #ZoneId make(int32 i) {\n"
        "        ZoneId z #= heap ZoneId(\"0123456789012345678901234567890\" + i);\n"
        "        return #= z;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        ZoneId z #= D.make(7);\n" +
        churn() +
        "        String id = z.getId();\n"
        "        if (id == null) { return -2; }\n"
        "        if (id.equals(\"01234567890123456789012345678907\")) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(1, runI32(src));
}

// DateTimeFormatter(String): the pattern is COPIED.
TEST(OwnershipMigrationPinTests, formatterCopiesItsPattern) {
    std::string src =
        "package test;\n"
        "import cajeta.time.DateTimeFormatter;\n"
        "public final class D {\n"
        "    static #DateTimeFormatter make(int32 i) {\n"
        "        DateTimeFormatter f #= heap DateTimeFormatter(\n"
        "            \"0123456789012345678901234567890\" + i);\n"
        "        return #= f;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        DateTimeFormatter f #= D.make(7);\n" +
        churn() +
        "        String p = f.getPattern();\n"
        "        if (p == null) { return -2; }\n"
        "        if (p.equals(\"01234567890123456789012345678907\")) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(1, runI32(src));
}
