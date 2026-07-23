//
// nucleo-frame U12 — pivot/melt (plan 12.1.1-12.3.1; spec §4.1). The two
// reshape ops, and the split the spec's gradual-typing model forces:
//  - MELT (wide->long) is LAZY: its output schema is static (the id columns,
//    a utf8 `variable` column of the former value-column NAMES, and a
//    `value` column of the stacked values), so it is an ordinary erased plan
//    node narrowed by `.as<R>()` at plan build.
//  - PIVOT (long->wide) is EAGER: its output COLUMNS come from the DATA (the
//    distinct values of the pivot key), so the schema cannot be known until
//    the data is read. Like Polars, pivot is therefore a forcing op that
//    returns a materialized `Table<?>` — the canonical erased case
//    (spec §4.3), narrowed once the value set is known.
//  - Duplicate (index, pivot-key) cells are a LOUD error (no silent
//    first-wins data loss); pre-aggregate with groupBy to collapse them.
//  - v1 restricts pivot values to numeric and melt value-vars to a shared
//    int64/float64/utf8 physical; the rest is recorded deferred.
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

const char* kPrelude =
    "package test;\n"
    "import cajeta.lang.Utf8;\n"
    "import cajeta.lang.String;\n"
    "import cajeta.nucleo.frame.Table;\n"
    "import cajeta.nucleo.frame.FrameException;\n"
    "import cajeta.nucleo.column.Column;\n"
    "import cajeta.nucleo.column.StringColumn;\n";

// A WIDE table: one sym, two quarter columns.
//   sym  q1   q2
//   A    1.0  2.0
//   B    3.0  4.0
const char* kWide =
    "public record Wide { Utf8 sym; float64 q1; float64 q2; }\n";
const char* kWideBuild =
    "        String[] sv = heap String[2];\n"
    "        sv[0] = \"A\"; sv[1] = \"B\";\n"
    "        float64[] a1 = heap float64[2];\n"
    "        a1[0] = 1.0; a1[1] = 3.0;\n"
    "        float64[] a2 = heap float64[2];\n"
    "        a2[0] = 2.0; a2[1] = 4.0;\n"
    "        Table<Wide> w = heap Table<Wide>(StringColumn.of(sv),\n"
    "            Column.of<float64>(a1), Column.of<float64>(a2));\n";

// A LONG table: the melted form.
//   sym  quarter  amt
//   A    q1       1.0
//   A    q2       2.0
//   B    q1       3.0
//   B    q2       4.0
const char* kLong =
    "public record Long { Utf8 sym; Utf8 quarter; float64 amt; }\n";
const char* kLongBuild =
    "        String[] ls = heap String[4];\n"
    "        ls[0] = \"A\"; ls[1] = \"A\"; ls[2] = \"B\"; ls[3] = \"B\";\n"
    "        String[] lq = heap String[4];\n"
    "        lq[0] = \"q1\"; lq[1] = \"q2\"; lq[2] = \"q1\"; lq[3] = \"q2\";\n"
    "        float64[] la = heap float64[4];\n"
    "        la[0] = 1.0; la[1] = 2.0; la[2] = 3.0; la[3] = 4.0;\n"
    "        Table<Long> lg = heap Table<Long>(StringColumn.of(ls),\n"
    "            StringColumn.of(lq), Column.of<float64>(la));\n";

std::string idAndVal() {
    return
        "        String[] idv = heap String[1];\n"
        "        idv[0] = \"sym\";\n"
        "        String[] vlv = heap String[2];\n"
        "        vlv[0] = \"q1\"; vlv[1] = \"q2\";\n";
}

} // namespace

// 12.1.1 — melt is lazy and its schema is static: id column, the utf8
// `variable` column of the former names, the stacked `value` column. Each
// input row expands to one output row per value var, in order.
TEST(PivotMeltTests, meltWideToLongIsLazyWithStaticSchema) {
    auto src = std::string(kPrelude) + kWide +
        "public record LongRec { Utf8 sym; Utf8 quarter; float64 amt; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kWideBuild + idAndVal() +
        "        int32 score = 0;\n"
        "        Table<?> h = w.lazy().melt(idv, 1, vlv, 2, \"quarter\",\n"
        "            \"amt\");\n"
        "        if (h.executions() == 0) { score = score + 1; }\n"  // built, not run
        "        Table<?> r = h.collect();\n"
        // 2 rows x 2 value vars = 4 rows; width = sym + quarter + amt.
        "        if (r.rowCount() == 4 && r.width() == 3) { score = score + 2; }\n"
        // Row expansion order: (A,q1,1), (A,q2,2), (B,q1,3), (B,q2,4).
        "        if (r.strAt(\"sym\", 0).equals(\"A\")\n"
        "                && r.strAt(\"quarter\", 0).equals(\"q1\")\n"
        "                && r.f64At(\"amt\", 0) == 1.0\n"
        "                && r.strAt(\"quarter\", 1).equals(\"q2\")\n"
        "                && r.f64At(\"amt\", 1) == 2.0\n"
        "                && r.strAt(\"sym\", 2).equals(\"B\")\n"
        "                && r.f64At(\"amt\", 3) == 4.0) {\n"
        "            score = score + 4;\n"
        "        }\n"
        // The static schema narrows at plan build (no forcing needed).
        "        Table<LongRec> lr = w.lazy().melt(idv, 1, vlv, 2, \"quarter\",\n"
        "            \"amt\").as<LongRec>().collect();\n"
        "        if (lr.rowCount() == 4 && lr.amt.get(2) == 3.0\n"
        "                && lr.quarter.get(2).equals(\"q1\")) {\n"
        "            score = score + 8;\n"
        "        }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

// 12.1.1 / 12.1.2 — pivot is EAGER (forces) and erased: the columns come
// from the data. Distinct index rows and pivot-key columns, in
// first-appearance order; absent cells are null; a duplicate cell is loud.
TEST(PivotMeltTests, pivotLongToWideIsEagerAndErased) {
    auto src = std::string(kPrelude) + kLong +
        "public record WideRec {\n"
        "    Utf8 sym; @Nullable float64 q1; @Nullable float64 q2;\n"
        "}\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kLongBuild +
        "        int32 score = 0;\n"
        "        String[] idx = heap String[1];\n"
        "        idx[0] = \"sym\";\n"
        "        Table<?> r = lg.pivot(idx, 1, \"quarter\", \"amt\");\n"
        // One row per distinct sym (A, B), columns sym + q1 + q2.
        "        if (r.rowCount() == 2 && r.width() == 3\n"
        "                && r.strAt(\"sym\", 0).equals(\"A\")\n"
        "                && r.strAt(\"sym\", 1).equals(\"B\")) {\n"
        "            score = score + 1;\n"
        "        }\n"
        "        if (r.f64At(\"q1\", 0) == 1.0 && r.f64At(\"q2\", 0) == 2.0\n"
        "                && r.f64At(\"q1\", 1) == 3.0\n"
        "                && r.f64At(\"q2\", 1) == 4.0) {\n"
        "            score = score + 2;\n"
        "        }\n"
        // The erased result narrows on a record whose pivoted columns are
        // nullable (a pivot cell may be absent).
        "        Table<?> r2 = lg.pivot(idx, 1, \"quarter\", \"amt\");\n"
        "        Table<WideRec> wr = r2.as<WideRec>();\n"
        "        if (wr.rowCount() == 2 && wr.q1.get(1) == 3.0) {\n"
        "            score = score + 4;\n"
        "        }\n"
        // A DUPLICATE (sym, quarter) cell is a loud error, not silent loss.
        "        String[] ds = heap String[2];\n"
        "        ds[0] = \"A\"; ds[1] = \"A\";\n"
        "        String[] dq = heap String[2];\n"
        "        dq[0] = \"q1\"; dq[1] = \"q1\";\n"
        "        float64[] da = heap float64[2];\n"
        "        da[0] = 1.0; da[1] = 9.0;\n"
        "        Table<Long> dup = heap Table<Long>(StringColumn.of(ds),\n"
        "            StringColumn.of(dq), Column.of<float64>(da));\n"
        "        try {\n"
        "            Table<?> bad = dup.pivot(idx, 1, \"quarter\", \"amt\");\n"
        "            bad.rowCount();\n"
        "        } catch (FrameException e) {\n"
        "            if (e.getMessage().contains(\"duplicate\")) {\n"
        "                score = score + 8;\n"
        "            }\n"
        "        }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

// 12.1.1 — the round trip: melt a wide table to long, pivot it back, and
// recover the original wide values.
TEST(PivotMeltTests, meltThenPivotRoundTrips) {
    auto src = std::string(kPrelude) + kWide +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kWideBuild + idAndVal() +
        "        int32 score = 0;\n"
        // wide -> long
        "        Table<?> lng = w.lazy().melt(idv, 1, vlv, 2, \"quarter\",\n"
        "            \"amt\").collect();\n"
        "        if (lng.rowCount() == 4) { score = score + 1; }\n"
        // long -> wide (pivot forces the long frame it is handed)
        "        String[] idx = heap String[1];\n"
        "        idx[0] = \"sym\";\n"
        "        Table<?> back = lng.pivot(idx, 1, \"quarter\", \"amt\");\n"
        "        if (back.rowCount() == 2 && back.width() == 3\n"
        "                && back.strAt(\"sym\", 0).equals(\"A\")\n"
        "                && back.f64At(\"q1\", 0) == 1.0\n"
        "                && back.f64At(\"q2\", 0) == 2.0\n"
        "                && back.f64At(\"q1\", 1) == 3.0\n"
        "                && back.f64At(\"q2\", 1) == 4.0) {\n"
        "            score = score + 2;\n"
        "        }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}
