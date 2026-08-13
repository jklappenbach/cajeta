//
// nucleo-frame U6 — nulls end-to-end for the shipped ops (plan 6.1.1–6.3.1;
// spec §7). Missing is a REAL absence carried by the validity bitmap, never
// a NaN sentinel:
//  - filter comparisons are THREE-VALUED: a null-valued predicate excludes
//    the row — even when the slot's physical bytes would pass;
//  - arithmetic propagates null: a computed column over a nullable input
//    is itself nullable (`float64?` in the result schema), and a computed
//    column over non-null inputs STAYS non-null — the no-validity-read
//    fast path, pinned by the result schema (plus the Exec code audit);
//  - null ≠ NaN: `mean([1,2,null]) = 1.5` (skipped) but
//    `mean([1,2,NaN]) = NaN` (a value that propagates); `count()` =
//    non-null, `len()` = all rows (the U8 agg family's preview);
//  - fill/drop are EXPLICIT ops: `fillNull(col, v)` flips the column
//    non-null in the RESULT schema (an erased `Table<?>` narrowed by the
//    filled record — the unfilled table provably cannot narrow there);
//    `dropNulls()` drops rows and keeps the schema (typed result).
//
// Fold shape (test-battery-restructure 2.3): the five original single-claim
// tests each paid their own JIT compile of a near-identical program — per-test
// coverage measurement (2026-08-10) put their compiler line footprints at
// ~18.8k shared lines. The claims now ride two programs, split along the
// use-case families they exercise: the READ-and-COMPUTE semantics (6.1.1
// three-valued filter + null propagation, 6.1.3 null-vs-NaN with count/len,
// 6.3.1 the non-null fast path as a schema fact) and the EXPLICIT null ops
// (6.1.2 fillNull / dropNulls). Every scenario is its own static entry point
// that builds its own table locally and returns its own score, so no scenario
// can perturb another, and each C++ EXPECT names the scenario it failed on.
// Every original score bit is preserved verbatim.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

// Was runI32(src) — one compile per claim. Both folded programs run several
// entry points off ONE compile, so the helper now looks up an entry by name
// and calls it; a missing entry fails loud rather than segfaulting.
int32_t callI32(CajetaJit& jit, const char* entry) {
    auto fn = jit.lookup<int32_t (*)()>(entry);
    EXPECT_NE(fn, nullptr) << "missing entry point: " << entry;
    return fn == nullptr ? -1 : fn();
}

const char* kPrelude =
    "package test;\n"
    "import cajeta.lang.Utf8;\n"
    "import cajeta.nucleo.frame.Table;\n"
    "import cajeta.nucleo.frame.FrameException;\n"
    "import cajeta.nucleo.frame.Pred;\n"
    "import cajeta.nucleo.column.Column;\n"
    "import cajeta.nucleo.column.NullableColumn;\n"
    "import cajeta.nucleo.column.StringColumn;\n"
    "public record TickN {\n"
    "    float64 price;\n"
    "    @Nullable float64 lastTrade;\n"
    "    Utf8 venue;\n"
    "}\n";

// Five rows. lastTrade = [10, null, 30, null, 50]; the null slots carry
// PHYSICAL bytes (99.0) that would pass a `> 5.0` filter — the three-valued
// rule, not the raw buffer, must exclude them. Every entry point below emits
// this block into its OWN body: each scenario builds its own table, nothing
// is shared across entries.
const char* kBuild =
    "        float64[] pv = heap float64[5];\n"
    "        pv[0] = 1.0; pv[1] = 2.0; pv[2] = 3.0; pv[3] = 4.0; pv[4] = 5.0;\n"
    "        float64[] lv = heap float64[5];\n"
    "        lv[0] = 10.0; lv[1] = 99.0; lv[2] = 30.0; lv[3] = 99.0; lv[4] = 50.0;\n"
    "        boolean[] ok = heap boolean[5];\n"
    "        ok[0] = true; ok[1] = false; ok[2] = true; ok[3] = false; ok[4] = true;\n"
    "        String[] vv = heap String[5];\n"
    "        vv[0] = \"A\"; vv[1] = \"B\"; vv[2] = \"C\"; vv[3] = \"D\"; vv[4] = \"E\";\n"
    "        Table<TickN> t = heap Table<TickN>(\n"
    "            Column.of<float64>(pv), NullableColumn.of<float64>(lv, ok),\n"
    "            StringColumn.of(vv));\n";

// Program 1 — the read-and-compute null semantics (6.1.1, 6.1.3, 6.3.1).
std::string semanticsSrc() {
    return std::string(kPrelude) +
        "public record TickNAdj {\n"
        "    float64 price;\n"
        "    @Nullable float64 lastTrade;\n"
        "    Utf8 venue;\n"
        "    @Nullable float64 adj;\n"
        "}\n"
        "public final class D {\n"
        // 6.1.1 — filter comparisons are three-valued (null predicate -> row
        // EXCLUDED even though the physical bytes would pass) and arithmetic
        // propagates null into computed columns (nullable in, nullable out —
        // the result narrows to a record whose computed field is @Nullable).
        "    public static int32 filterAndPropagate() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        "        Table<TickN> h = t.lazy().filter(\n"
        "            (TickNCols c) -> c.lastTrade() > 5.0);\n"
        "        Table<TickN> r = h.collect();\n"
        "        if (r.rowCount() == 3 && r.price.get(0) == 1.0\n"
        "                && r.price.get(2) == 5.0) {\n"
        "            score = score + 1;\n"
        "        }\n"
        "        if (r.lastTrade.count() == 3 && r.lastTrade.len() == 3) {\n"
        "            score = score + 2;\n"     // gathered validity intact
        "        }\n"
        "        Table<?> w = t.with(\n"
        "            (TickNCols c) -> (c.lastTrade() + c.price()).alias(\"adj\"));\n"
        "        Table<TickNAdj> ah = w.as<TickNAdj>();\n"
        "        Table<TickNAdj> a = ah.collect();\n"
        "        if (a.adj.isValid(0) && !a.adj.isValid(1)\n"
        "                && a.adj.get(0) == 11.0 && a.adj.get(4) == 55.0) {\n"
        "            score = score + 4;\n"
        "        }\n"
        "        return score;\n"
        "    }\n"
        // 6.1.1 + 6.1.3 — null is NOT NaN: mean skips nulls (1.5) but a
        // present NaN poisons the reduction (NaN); count() = non-null,
        // len() = all rows.
        "    public static int32 nanAndCounts() {\n"
        "        int32 score = 0;\n"
        "        float64[] xv = heap float64[3];\n"
        "        xv[0] = 1.0; xv[1] = 2.0; xv[2] = 77.0;\n"
        "        boolean[] xo = heap boolean[3];\n"
        "        xo[0] = true; xo[1] = true; xo[2] = false;\n"
        "        NullableColumn<float64> nc = NullableColumn.of<float64>(xv, xo);\n"
        "        if (nc.mean() == 1.5) { score = score + 1; }\n"      // null SKIPPED
        "        if (nc.count() == 2 && nc.len() == 3) { score = score + 2; }\n"
        "        float64 z = 0.0;\n"
        "        float64[] yv = heap float64[3];\n"
        "        yv[0] = 1.0; yv[1] = 2.0; yv[2] = 0.0 / z;\n"        // NaN value
        "        Column<float64> cc = Column.of<float64>(yv);\n"
        "        float64 m = cc.mean();\n"
        "        if (m != m) { score = score + 4; }\n"                // NaN PROPAGATES
        "        if (cc.count() == 3 && cc.len() == 3) { score = score + 8; }\n"
        "        return score;\n"
        "    }\n"
        // 6.3.1 — the fast path is a SCHEMA fact: a computed column over
        // non-null inputs stays `float64` (no validity reads — Exec's
        // non-null loop never touches validAt, code-audited), while one over
        // a nullable input is marked `float64?` at plan build.
        "    public static int32 fastPathSchema() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        "        Table<?> w = t.with((TickNCols c) -> (c.price() * 2.0).alias(\"dbl\"));\n"
        "        String ds = w.describe();\n"
        "        if (ds.contains(\"dbl: float64\") && !ds.contains(\"dbl: float64?\")) {\n"
        "            score = score + 1;\n"
        "        }\n"
        "        Table<?> w2 = t.with((TickNCols c) -> (c.lastTrade() * 2.0).alias(\"adj2\"));\n"
        "        if (w2.describe().contains(\"adj2: float64?\")) { score = score + 2; }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
}

// Program 2 — the EXPLICIT null ops (6.1.2). Separate from program 1 because
// this family narrows through its own record (TickNF, lastTrade non-null) and
// leans on FrameException paths at plan build.
std::string fillDropSrc() {
    return std::string(kPrelude) +
        "public record TickNF {\n"
        "    float64 price;\n"
        "    float64 lastTrade;\n"
        "    Utf8 venue;\n"
        "}\n"
        "public final class D {\n"
        // 6.1.2 — fillNull(col, v) is explicit and flips the column NON-NULL
        // in the result schema: the filled table narrows to the non-null
        // record, the UNFILLED one provably cannot; unknown / non-nullable
        // columns fail loud at plan build.
        "    public static int32 fillNullSchema() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        "        try {\n"
        "            Table<TickN> lz = t.lazy();\n"
        "            Table<TickNF> bad = lz.as<TickNF>();\n"   // still nullable
        "        } catch (FrameException e) {\n"
        "            if (e.getMessage().contains(\"lastTrade\")) { score = score + 1; }\n"
        "        }\n"
        "        Table<?> f = t.fillNull(\"lastTrade\", 0.0);\n"
        "        Table<TickNF> fh = f.as<TickNF>();\n"
        "        Table<TickNF> fr = fh.collect();\n"
        "        if (fr.rowCount() == 5 && fr.lastTrade.get(0) == 10.0\n"
        "                && fr.lastTrade.get(1) == 0.0\n"
        "                && fr.lastTrade.get(3) == 0.0\n"
        "                && fr.lastTrade.get(4) == 50.0) {\n"
        "            score = score + 2;\n"
        "        }\n"
        "        if (!fr.colNullableAt(1)) { score = score + 4; }\n"
        "        try {\n"
        "            t.fillNull(\"nope\", 0.0);\n"
        "        } catch (FrameException e) {\n"
        "            if (e.getMessage().contains(\"nope\")\n"
        "                    && e.getMessage().contains(\"price\")) {\n"  // names the schema
        "                score = score + 8;\n"
        "            }\n"
        "        }\n"
        "        try {\n"
        "            t.fillNull(\"price\", 0.0);\n"
        "        } catch (FrameException e) {\n"
        "            if (e.getMessage().contains(\"not nullable\")) { score = score + 16; }\n"
        "        }\n"
        "        return score;\n"
        "    }\n"
        // 6.1.2 — dropNulls() drops the rows with a missing value and KEEPS
        // the schema (a typed, chainable lazy op; other columns gather in
        // step).
        "    public static int32 dropNullRows() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        "        Table<TickN> d = t.dropNulls();\n"
        "        Table<TickN> dr = d.collect();\n"
        "        if (dr.rowCount() == 3 && dr.price.get(1) == 3.0\n"
        "                && dr.venue.get(2).equals(\"E\")) {\n"
        "            score = score + 1;\n"
        "        }\n"
        "        if (dr.lastTrade.count() == 3 && dr.lastTrade.get(1) == 30.0) {\n"
        "            score = score + 2;\n"
        "        }\n"
        "        Table<TickN> d2 = t.lazy().filter(\n"
        "            (TickNCols c) -> c.price() > 1.0).dropNulls();\n"
        "        Table<TickN> r2 = d2.collect();\n"
        "        if (r2.rowCount() == 2 && r2.price.get(0) == 3.0\n"
        "                && r2.price.get(1) == 5.0) {\n"
        "            score = score + 4;\n"
        "        }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
}

} // namespace

// The read-and-compute null use-cases, one compiled program (was:
// NullSemanticsTests.{threeValuedFilterAndNullPropagation,
// nullIsNotNaNAndCountLen, nonNullComputeStaysNonNullable} — three compiles'
// worth of claims, every score bit preserved):
//   6.1.1 three-valued filter + null propagation into computed columns;
//   6.1.3 null is not NaN, count() vs len() on both column kinds;
//   6.3.1 non-null compute stays non-nullable in the result schema.
TEST(NullSemanticsTests, threeValuedFilterPropagationNaNAndFastPathMatrix) {
    auto jit = CajetaJit::compile(semanticsSrc(), "test.D");
    ASSERT_NE(jit, nullptr);

    // 6.1.1 — null predicate excludes the row (bit 1), gathered validity
    // survives the filter (bit 2), computed column is nullable and carries
    // the propagated null (bit 4).
    EXPECT_EQ(callI32(*jit, "filterAndPropagate"), 7)
        << "three-valued filter / null propagation";

    // 6.1.1 + 6.1.3 — mean skips nulls (bit 1), count/len split on the
    // nullable column (bit 2), a present NaN propagates (bit 4), count/len
    // on the plain column (bit 8).
    EXPECT_EQ(callI32(*jit, "nanAndCounts"), 15)
        << "null is not NaN / count() vs len()";

    // 6.3.1 — `dbl: float64` with no `?` (bit 1), `adj2: float64?` (bit 2).
    EXPECT_EQ(callI32(*jit, "fastPathSchema"), 3)
        << "non-null compute stays non-nullable";
}

// The explicit null-op use-cases, one compiled program (was:
// NullSemanticsTests.{fillNullNarrowsTheResultSchema,
// dropNullsDropsRowsKeepsSchema} — every score bit preserved):
//   6.1.2 fillNull narrows the result schema (and fails loud on unknown /
//   non-nullable columns); dropNulls drops rows and keeps the schema.
TEST(NullSemanticsTests, fillNullNarrowsAndDropNullsKeepsSchema) {
    auto jit = CajetaJit::compile(fillDropSrc(), "test.D");
    ASSERT_NE(jit, nullptr);

    // 6.1.2 — the unfilled table cannot narrow (bit 1), filled values (bit
    // 2), the column is non-nullable in the result schema (bit 4), unknown
    // column names the schema (bit 8), non-nullable column says so (bit 16).
    EXPECT_EQ(callI32(*jit, "fillNullSchema"), 31)
        << "fillNull narrows the result schema";

    // 6.1.2 — rows dropped and other columns gathered in step (bit 1),
    // validity intact on the kept rows (bit 2), chained after a filter
    // (bit 4).
    EXPECT_EQ(callI32(*jit, "dropNullRows"), 7)
        << "dropNulls drops rows, keeps schema";
}
