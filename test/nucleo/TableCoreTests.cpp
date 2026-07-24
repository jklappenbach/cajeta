//
// nucleo-frame U3 — `Table<T>` core: record-derived columns + accessors
// (plan 3.1.1–3.1.5; spec §2, §3.1). The schema record's fields ARE the
// columns: each field maps to its physical column type (Instant ->
// Column<int64> epoch-nanos, float64 -> Column<float64>, Utf8 ->
// StringColumn utf8, @Nullable prim -> NullableColumn<prim>), the
// synthesized constructor adopts caller columns zero-copy after a loud
// length check, and the synthesized accessor FIELDS give `ticks.price`
// typed reads where a typo fails the compile.
//
// Schema-shape resolutions pinned here (recorded in the plan):
//  - the spec's `Symbol venue` is `cajeta.lang.Utf8` (the 16-byte value-type
//    text designed for record fields; records reject heap `String`);
//  - the spec's `float64?` is the `@Nullable` field annotation (the record
//    face of the `Column<T?>` -> `NullableColumn<T>` column-spec resolution);
//  - `Instant` is `@ValueType` (record-legal) and stores as int64 epoch-nanos.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// Shared prologue: the §2 Tick schema + the frame/column imports.
const char* kPrelude =
    "package test;\n"
    "import cajeta.time.Instant;\n"
    "import cajeta.lang.Utf8;\n"
    "import cajeta.nucleo.frame.Table;\n"
    "import cajeta.nucleo.frame.FrameException;\n"
    "import cajeta.nucleo.column.Column;\n"
    "import cajeta.nucleo.column.NullableColumn;\n"
    "import cajeta.nucleo.column.StringColumn;\n"
    "public record Tick {\n"
    "    Instant ts;\n"
    "    float64 price;\n"
    "    float64 size;\n"
    "    Utf8 venue;\n"
    "}\n";

// Column-building helper snippet: three rows of each physical.
const char* kBuildCols =
    "        int64[] tsv = heap int64[3];\n"
    "        tsv[0] = 1000; tsv[1] = 2000; tsv[2] = 3000;\n"
    "        float64[] pv = heap float64[3];\n"
    "        pv[0] = 1.5; pv[1] = 2.5; pv[2] = 3.5;\n"
    "        float64[] sv = heap float64[3];\n"
    "        sv[0] = 10.0; sv[1] = 20.0; sv[2] = 30.0;\n"
    "        String[] vv = heap String[3];\n"
    "        vv[0] = \"ARCA\"; vv[1] = \"NYSE\"; vv[2] = \"ARCA\";\n";

} // namespace

// 3.1.1 — Table<Tick> derives exactly the four columns with the mapped
// physicals: ts -> int64 (epoch-nanos), price/size -> float64, venue -> utf8.
// Names, physicals, row count, and typed element reads all agree.
TEST(TableCoreTests, derivesMappedColumnsFromRecord) {
    auto src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kBuildCols +
        "        Table<Tick> t = heap Table<Tick>(\n"
        "            Column.of<int64>(tsv), Column.of<float64>(pv),\n"
        "            Column.of<float64>(sv), StringColumn.of(vv));\n"
        "        int32 score = 0;\n"
        "        if (t.colCount() == 4) { score = score + 1; }\n"
        "        if (t.rowCount() == 3) { score = score + 2; }\n"
        "        if (t.colNameAt(0).equals(\"ts\") && t.colNameAt(1).equals(\"price\")\n"
        "                && t.colNameAt(2).equals(\"size\") && t.colNameAt(3).equals(\"venue\")) {\n"
        "            score = score + 4;\n"
        "        }\n"
        "        if (t.colTypeAt(0).equals(\"int64\") && t.colTypeAt(1).equals(\"float64\")\n"
        "                && t.colTypeAt(2).equals(\"float64\") && t.colTypeAt(3).equals(\"utf8\")) {\n"
        "            score = score + 8;\n"
        "        }\n"
        "        if (t.ts.get(1) == 2000) { score = score + 16; }\n"
        "        if (t.price.get(2) == 3.5) { score = score + 32; }\n"
        "        if (t.venue.get(0).equals(\"ARCA\")) { score = score + 64; }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 127);
}

// 3.1.1 — a non-record schema argument is a NAMED compile error, not a
// silent degenerate table.
TEST(TableCoreTests, nonRecordSchemaIsNamedCompileError) {
    std::string src =
        "package test;\n"
        "import cajeta.nucleo.frame.Table;\n"
        "public class Box {\n"
        "    public int32 v;\n"
        "    public Box(int32 v) { this.v = v; }\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Table<Box> t = heap Table<Box>();\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected Table<non-record> to fail the compile";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_FRAME_SCHEMA")
            << "got: " << e.getErrorId() << " — " << e.getMessage();
        EXPECT_NE(e.getMessage().find("Box"), std::string::npos)
            << "must name the offending schema arg: " << e.getMessage();
    }
}

// 3.1.2 — `@Nullable float64` -> NullableColumn<float64> (validity bitmap);
// plain `float64` -> Column<float64> (no bitmap). Nullability is a
// per-column TYPE fact, visible in both introspection and the accessor's
// static type (isValid only exists on the nullable accessor).
TEST(TableCoreTests, nullableFieldDerivesNullableColumn) {
    std::string src =
        "package test;\n"
        "import cajeta.nucleo.frame.Table;\n"
        "import cajeta.nucleo.column.Column;\n"
        "import cajeta.nucleo.column.NullableColumn;\n"
        "public record Quote {\n"
        "    float64 bid;\n"
        "    @Nullable float64 lastTrade;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        float64[] bv = heap float64[2];\n"
        "        bv[0] = 99.5; bv[1] = 100.5;\n"
        "        float64[] lv = heap float64[2];\n"
        "        lv[0] = 9.5; lv[1] = 0.0;\n"
        "        boolean[] valid = heap boolean[2];\n"
        "        valid[0] = true; valid[1] = false;\n"
        "        Table<Quote> q = heap Table<Quote>(\n"
        "            Column.of<float64>(bv), NullableColumn.of<float64>(lv, valid));\n"
        "        int32 score = 0;\n"
        "        if (!q.colNullableAt(0)) { score = score + 1; }\n"
        "        if (q.colNullableAt(1)) { score = score + 2; }\n"
        "        if (q.lastTrade.isValid(0)) { score = score + 4; }\n"
        "        if (!q.lastTrade.isValid(1)) { score = score + 8; }\n"
        "        if (q.bid.get(1) == 100.5) { score = score + 16; }\n"
        "        if (q.lastTrade.get(0) == 9.5) { score = score + 32; }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 63);
}

// 3.1.3 — the synthesized member accessor is a typed read; a typo names no
// member and fails the compile, phrased against the typo. Two tables of the
// same instantiation share one synthesized accessor set (memoization — a
// duplicate synthesis would dup-member-error the compile).
TEST(TableCoreTests, accessorTypoFailsCompileAndSetIsDeterministic) {
    auto good = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kBuildCols +
        "        Table<Tick> t1 = heap Table<Tick>(\n"
        "            Column.of<int64>(tsv), Column.of<float64>(pv),\n"
        "            Column.of<float64>(sv), StringColumn.of(vv));\n"
        "        int64[] tsv2 = heap int64[1];\n"
        "        tsv2[0] = 7;\n"
        "        float64[] pv2 = heap float64[1];\n"
        "        pv2[0] = 7.5;\n"
        "        float64[] sv2 = heap float64[1];\n"
        "        sv2[0] = 70.0;\n"
        "        String[] vv2 = heap String[1];\n"
        "        vv2[0] = \"BATS\";\n"
        "        Table<Tick> t2 = heap Table<Tick>(\n"
        "            Column.of<int64>(tsv2), Column.of<float64>(pv2),\n"
        "            Column.of<float64>(sv2), StringColumn.of(vv2));\n"
        "        if (t1.price.get(0) == 1.5 && t2.price.get(0) == 7.5) { return 42; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(good), 42);

    auto typo = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kBuildCols +
        "        Table<Tick> t = heap Table<Tick>(\n"
        "            Column.of<int64>(tsv), Column.of<float64>(pv),\n"
        "            Column.of<float64>(sv), StringColumn.of(vv));\n"
        "        return (int32) t.prce.get(0);\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(typo, "test.D");
        FAIL() << "expected the accessor typo to fail the compile";
    } catch (cajeta::Exception& e) {
        EXPECT_NE(e.getMessage().find("prce"), std::string::npos)
            << "must name the missing accessor: " << e.getErrorId()
            << " — " << e.getMessage();
    }
}

// 3.1.4 — the synthesized constructor IS the schema check: a wrong column
// type at the call site is a named compile error (constructor overload
// resolution), a row-length mismatch fails loud at runtime, and matching
// columns adopt zero-copy (same data address through the accessor).
TEST(TableCoreTests, fromColumnsChecksSchemaAndAdoptsZeroCopy) {
    // Wrong physical for ts (float64 where int64 is required) -> compile error.
    auto wrongType = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kBuildCols +
        "        Table<Tick> t = heap Table<Tick>(\n"
        "            Column.of<float64>(pv), Column.of<float64>(pv),\n"
        "            Column.of<float64>(sv), StringColumn.of(vv));\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(wrongType, "test.D");
        FAIL() << "expected a column-type mismatch to fail the compile";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_NO_MATCHING_CONSTRUCTOR")
            << "got: " << e.getErrorId() << " — " << e.getMessage();
    }
}

TEST(TableCoreTests, lengthMismatchFailsLoudAndAdoptionIsZeroCopy) {
    auto src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kBuildCols +
        "        int32 score = 0;\n"
        "        float64[] shortPv = heap float64[2];\n"
        "        shortPv[0] = 1.0; shortPv[1] = 2.0;\n"
        "        try {\n"
        "            Table<Tick> bad = heap Table<Tick>(\n"
        "                Column.of<int64>(tsv), Column.of<float64>(shortPv),\n"
        "                Column.of<float64>(sv), StringColumn.of(vv));\n"
        "        } catch (FrameException e) {\n"
        "            score = score + 1;\n"
        "        }\n"
        "        Column<float64> p = Column.of<float64>(pv);\n"
        "        int64 a0 = p.dataAddress();\n"
        "        Table<Tick> t = heap Table<Tick>(\n"
        "            Column.of<int64>(tsv), #p,\n"
        "            Column.of<float64>(sv), StringColumn.of(vv));\n"
        "        if (t.price.dataAddress() == a0) { score = score + 2; }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}

// 3.1.5 — same-schema tables are the same type (one function serves both
// instances), and `Table<? extends Tick>` accepts a `Table<TradeTick>` with
// the `Tick` accessors working through the bound (U2's 2.1.2 re-run against
// the real Table). TradeTick's columns are the inherited Tick columns first,
// then its own — the layout prefix the bound relies on.
TEST(TableCoreTests, sameSchemaSameTypeAndBoundedWildcardAccess) {
    auto src = std::string(kPrelude) +
        "public record TradeTick extends Tick {\n"
        "    int32 flags;\n"
        "}\n"
        "public final class D {\n"
        "    static float64 firstPrice(Table<? extends Tick> t) {\n"
        "        return t.price.get(0);\n"
        "    }\n"
        "    static int64 rowsOf(Table<Tick> t) { return t.rowCount(); }\n"
        "    public static int32 run() {\n"
        + kBuildCols +
        "        Table<Tick> t1 = heap Table<Tick>(\n"
        "            Column.of<int64>(tsv), Column.of<float64>(pv),\n"
        "            Column.of<float64>(sv), StringColumn.of(vv));\n"
        "        int64[] tsv2 = heap int64[1];\n"
        "        tsv2[0] = 7;\n"
        "        float64[] pv2 = heap float64[1];\n"
        "        pv2[0] = 7.5;\n"
        "        float64[] sv2 = heap float64[1];\n"
        "        sv2[0] = 70.0;\n"
        "        String[] vv2 = heap String[1];\n"
        "        vv2[0] = \"BATS\";\n"
        "        int32[] fv2 = heap int32[1];\n"
        "        fv2[0] = 9;\n"
        "        Table<TradeTick> tt = heap Table<TradeTick>(\n"
        "            Column.of<int64>(tsv2), Column.of<float64>(pv2),\n"
        "            Column.of<float64>(sv2), StringColumn.of(vv2),\n"
        "            Column.of<int32>(fv2));\n"
        "        int32 score = 0;\n"
        "        if (rowsOf(t1) == 3) { score = score + 1; }\n"
        "        if (firstPrice(t1) == 1.5) { score = score + 2; }\n"
        "        if (firstPrice(tt) == 7.5) { score = score + 4; }\n"
        "        if (tt.colCount() == 5 && tt.colNameAt(4).equals(\"flags\")\n"
        "                && tt.colTypeAt(4).equals(\"int32\")) {\n"
        "            score = score + 8;\n"
        "        }\n"
        "        if (tt.flags.get(0) == 9) { score = score + 16; }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 31);
}

// 3.2.3 — a class field with no physical mapping (neither primitive nor
// Instant/Utf8) is a NAMED synthesis error naming the field and its type.
TEST(TableCoreTests, unmappedFieldTypeIsNamedError) {
    std::string src =
        "package test;\n"
        "import cajeta.nucleo.frame.Table;\n"
        "import cajeta.math.Sphere;\n"
        "public record Odd {\n"
        "    float64 x;\n"
        "    Sphere pos;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Table<Odd> t = heap Table<Odd>();\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    try {
        CajetaJit::compile(src, "test.D");
        FAIL() << "expected an unmapped schema field to fail the compile";
    } catch (cajeta::Exception& e) {
        EXPECT_EQ(e.getErrorId(), "CAJETA_ERROR_FRAME_UNMAPPED_FIELD")
            << "got: " << e.getErrorId() << " — " << e.getMessage();
        EXPECT_NE(e.getMessage().find("pos"), std::string::npos)
            << "must name the field: " << e.getMessage();
    }
}
