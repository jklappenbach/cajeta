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
#include <unistd.h>
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

// LtmBPlusTree/LtmPager encoder holds are `#=` slots: INLINE TEMP encoders
// (the natural spelling) transfer into the tree and survive the building
// frame. Before the 4.2.2 migration the plain `=` stores kept only a borrow
// of the helper frame's temps — the tree's encoders died with make().
// (Post-hoc pin: the migration landed in the same commit; red-first evidence
// for this class lives in the exception-chain canaries.)
TEST(OwnershipMigrationPinTests, ltmTreeOwnsInlineTempEncoders) {
    std::string path = "/tmp/claude-1000/ltm_pin_enc.idx";
    ::unlink(path.c_str());
    std::string src =
        "package test;\n"
        "import cajeta.collection.ltm.LtmBPlusTree;\n"
        "import cajeta.wire.Encoder;\n"
        // Same shape as LtmBPlusTreeTests' kI32Enc — the wire.Encoder
        // contract is encode(#int8[] out) / decode(#int32 out).
        "public final class I32Enc implements Encoder<int32> {\n"
        "    public I32Enc() { }\n"
        "    public #int8[] encode(int32 value) {\n"
        "        int8[] b = heap int8[4];\n"
        "        b[0] = (int8) (value & 0xFF);\n"
        "        b[1] = (int8) ((value >> 8) & 0xFF);\n"
        "        b[2] = (int8) ((value >> 16) & 0xFF);\n"
        "        b[3] = (int8) ((value >> 24) & 0xFF);\n"
        "        return #b;\n"
        "    }\n"
        "    public #int32 decode(int8[] bytes) {\n"
        "        int32 b0 = ((int32) bytes[0]) & 0xFF;\n"
        "        int32 b1 = ((int32) bytes[1]) & 0xFF;\n"
        "        int32 b2 = ((int32) bytes[2]) & 0xFF;\n"
        "        int32 b3 = ((int32) bytes[3]) & 0xFF;\n"
        "        int32 r = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);\n"
        "        return #r;\n"
        "    }\n"
        "}\n"
        "public final class D {\n"
        "    static #LtmBPlusTree<int32, int32> make() {\n"
        "        LtmBPlusTree<int32, int32> t #= heap LtmBPlusTree<int32, int32>(\n"
        "            \"" + path + "\", heap I32Enc(), heap I32Enc(), 4, 8);\n"
        "        return #= t;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        LtmBPlusTree<int32, int32> t #= D.make();\n"
        "        int32 i = 0;\n"
        "        while (i < 50) { t.put(i, i * 7); i = i + 1; }\n"
        "        int32 ok = 0;\n"
        "        int32 j = 0;\n"
        "        while (j < 50) {\n"
        "            if (t.get(j) == j * 7) { ok = ok + 1; }\n"
        "            j = j + 1;\n"
        "        }\n"
        "        t.close();\n"
        "        return ok;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(50, runI32(src));
}

// DynFrame.setSpatial stores its column names with `#=`: temp names survive
// the naming frame (mirrors addIndexed's slot model).
TEST(OwnershipMigrationPinTests, setSpatialOwnsTransferredNames) {
    std::string src =
        "package test;\n"
        "import cajeta.nucleo.frame.DynFrame;\n"
        "public final class D {\n"
        "    static #DynFrame make(int32 i) {\n"
        "        String[] names #= heap String[1];\n"
        "        names[0] #= \"c0\";\n"
        "        int32[] tags #= heap int32[1];\n"
        "        boolean[] nulls #= heap boolean[1];\n"
        "        DynFrame f #= heap DynFrame(#names, #tags, #nulls, 1);\n"
        "        String sx #= \"0123456789012345678901234567890\" + i;\n"
        "        String sy #= \"y123456789012345678901234567890\" + i;\n"
        "        f.setSpatial(#sx, #sy);\n"
        "        return #= f;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        DynFrame f #= D.make(7);\n" +
        churn() +
        "        String x = f.spatialXOf();\n"
        "        if (x == null) { return -2; }\n"
        "        if (x.equals(\"01234567890123456789012345678907\")) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(1, runI32(src));
}
