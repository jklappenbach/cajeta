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
// Fold shape (test-battery-restructure 2.3): the five RUNTIME claims below
// used to compile five separate programs whose compiler line footprints were
// measured near-identical (~18.8k shared lines, per-test coverage 2026-08-10)
// — each paying its own JIT compile of a small cajeta program to assert a
// handful of score bits. They are now ONE program with one static entry point
// per scenario (each entry self-contained: it builds its own arrays, columns
// and tables locally, so no scenario can disturb another) and ONE C++ test
// asserting them scenario by scenario with the original score expectations,
// bit for bit. The compile-FAILURE claims keep their own tests — a compile
// that fails cannot share a program with one that must succeed.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

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

// The one program every runtime scenario shares. `Tick` (kPrelude),
// `TradeTick` and `Quote` are all declared once; each entry point below is a
// former test's `run()` body verbatim, renamed to say which claim it carries.
std::string tableMatrixSrc() {
    return std::string(kPrelude) +
        // 3.1.5 — TradeTick's columns are the inherited Tick columns first,
        // then its own: the layout prefix the `? extends Tick` bound relies on.
        "public record TradeTick extends Tick {\n"
        "    int32 flags;\n"
        "}\n"
        // 3.1.2 — the nullability schema: one plain field, one @Nullable.
        "public record Quote {\n"
        "    float64 bid;\n"
        "    @Nullable float64 lastTrade;\n"
        "}\n"
        "public final class D {\n"
        // 3.1.5 — one function serves both instances (same-schema tables are
        // the same type); the wildcard-bounded one reads Tick accessors
        // through the bound.
        "    static float64 firstPrice(Table<? extends Tick> t) {\n"
        "        return t.price.get(0);\n"
        "    }\n"
        "    static int64 rowsOf(Table<Tick> t) { return t.rowCount(); }\n"
        // 3.1.1 — Table<Tick> derives exactly the four columns with the mapped
        // physicals: ts -> int64 (epoch-nanos), price/size -> float64,
        // venue -> utf8. Names, physicals, row count, and typed element reads
        // all agree.
        "    public static int32 derivedColumns() {\n"
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
        // 3.1.2 — `@Nullable float64` -> NullableColumn<float64> (validity
        // bitmap); plain `float64` -> Column<float64> (no bitmap). Nullability
        // is a per-column TYPE fact, visible in both introspection and the
        // accessor's static type (isValid only exists on the nullable
        // accessor).
        "    public static int32 nullableColumns() {\n"
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
        // 3.1.3 — two tables of the same instantiation share one synthesized
        // accessor set (memoization — a duplicate synthesis would
        // dup-member-error the compile), and the accessor is a typed read.
        // (The typo half of this claim needs a compile that FAILS; it keeps
        // its own test below.)
        "    public static int32 accessorSetIsDeterministic() {\n"
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
        // 3.1.4 — the synthesized constructor IS the schema check: a
        // row-length mismatch fails loud at runtime (FrameException), and
        // matching columns adopt zero-copy (same data address through the
        // accessor). (The wrong-column-type half is a compile error and keeps
        // its own test below.)
        "    public static int32 lengthCheckAndZeroCopy() {\n"
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
        // 3.1.5 — same-schema tables are the same type (one function serves
        // both instances), and `Table<? extends Tick>` accepts a
        // `Table<TradeTick>` with the `Tick` accessors working through the
        // bound (U2's 2.1.2 re-run against the real Table).
        "    public static int32 sameTypeAndWildcard() {\n"
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
}

} // namespace

// The Table<T> core use-case matrix, one compiled program (was: TableCoreTests
// .{derivesMappedColumnsFromRecord, nullableFieldDerivesNullableColumn,
// accessorTypoFailsCompileAndSetIsDeterministic (its passing half),
// lengthMismatchFailsLoudAndAdoptionIsZeroCopy,
// sameSchemaSameTypeAndBoundedWildcardAccess} — five compiles' worth of
// claims, every score bit preserved):
//   3.1.1 record fields derive the mapped physical columns; 3.1.2 @Nullable
//   derives NullableColumn and plain does not; 3.1.3 the synthesized accessor
//   set is memoized across same-instantiation tables; 3.1.4 the synthesized
//   constructor length-checks loudly and adopts zero-copy; 3.1.5 same-schema
//   tables are the same type and `Table<? extends Tick>` reads a
//   `Table<TradeTick>` through the bound.
TEST(TableCoreTests, schemaDerivationAccessorAdoptionAndWildcardMatrix) {
    auto jit = CajetaJit::compile(tableMatrixSrc(), "test.D");
    ASSERT_NE(jit, nullptr);
    auto derivedColumns = jit->lookup<int32_t (*)()>("derivedColumns");
    auto nullableColumns = jit->lookup<int32_t (*)()>("nullableColumns");
    auto accessorSetIsDeterministic =
        jit->lookup<int32_t (*)()>("accessorSetIsDeterministic");
    auto lengthCheckAndZeroCopy =
        jit->lookup<int32_t (*)()>("lengthCheckAndZeroCopy");
    auto sameTypeAndWildcard = jit->lookup<int32_t (*)()>("sameTypeAndWildcard");
    ASSERT_NE(derivedColumns, nullptr);
    ASSERT_NE(nullableColumns, nullptr);
    ASSERT_NE(accessorSetIsDeterministic, nullptr);
    ASSERT_NE(lengthCheckAndZeroCopy, nullptr);
    ASSERT_NE(sameTypeAndWildcard, nullptr);

    // 3.1.1 — colCount(1) rowCount(2) names(4) physicals(8) ts.get(16)
    // price.get(32) venue.get(64).
    EXPECT_EQ(derivedColumns(), 127) << "3.1.1 derived columns: score bits "
        << "1=colCount 2=rowCount 4=names 8=physicals 16=ts 32=price 64=venue";

    // 3.1.2 — bid not nullable(1) lastTrade nullable(2) isValid(0)(4)
    // !isValid(1)(8) bid.get(16) lastTrade.get(32).
    EXPECT_EQ(nullableColumns(), 63) << "3.1.2 nullable derivation: score bits "
        << "1=!nullable(bid) 2=nullable(lastTrade) 4=isValid(0) "
        << "8=!isValid(1) 16=bid.get 32=lastTrade.get";

    // 3.1.3 — both same-instantiation tables read through the one synthesized
    // accessor set.
    EXPECT_EQ(accessorSetIsDeterministic(), 42)
        << "3.1.3 memoized accessor set: both t1.price and t2.price must read";

    // 3.1.4 — length mismatch throws FrameException(1); adoption is
    // zero-copy(2).
    EXPECT_EQ(lengthCheckAndZeroCopy(), 3)
        << "3.1.4 constructor: score bits 1=FrameException on short column "
        << "2=zero-copy adoption (dataAddress unchanged)";

    // 3.1.5 — same type(1), wildcard on Table<Tick>(2), wildcard on
    // Table<TradeTick>(4), inherited-then-own layout(8), own accessor(16).
    EXPECT_EQ(sameTypeAndWildcard(), 31)
        << "3.1.5 same-schema/bounded wildcard: score bits 1=rowsOf(t1) "
        << "2=firstPrice(t1) 4=firstPrice(tt) 8=flags column shape "
        << "16=tt.flags.get";
}

// 3.1.1 — a non-record schema argument is a NAMED compile error, not a
// silent degenerate table. Its own test: the compile FAILS.
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

// 3.1.3 — the synthesized member accessor is a typed read; a typo names no
// member and fails the compile, phrased against the typo. Its own test: the
// compile FAILS (the memoization half of 3.1.3 rides the matrix above, as
// `accessorSetIsDeterministic`).
TEST(TableCoreTests, accessorTypoFailsCompileNamingTheTypo) {
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
// resolution). Its own test: the compile FAILS. (The runtime halves of 3.1.4
// — the loud row-length check and zero-copy adoption — ride the matrix above,
// as `lengthCheckAndZeroCopy`.)
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

// 3.2.3 — a class field with no physical mapping (neither primitive nor
// Instant/Utf8) is a NAMED synthesis error naming the field and its type.
// Its own test: the compile FAILS.
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
