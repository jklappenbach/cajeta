//
// nucleo-frame U4 — the lazy plan + terminals (plan 4.1.1–4.1.5; spec §4,
// F-FORCE). One lazy Table<T>: a materialized table is a forced source; a
// plan handle (built by `lazy()`, and by the U5 ops) executes nothing until
// a terminal — collect() (exactly once, cached), head(n)/fetch(n) (bounded),
// row iteration (the rows() cursor — the for-each protocol only covers
// arrays today, so the `for (row : table)` sugar stays recorded for
// syntax-sugar; the SEMANTICS ship here), and whole-column scalar
// reductions (Column.max/min/sum). Printing an unforced handle describes
// schema + plan, never rows.
//
// The scan snapshot is column ALIASES (zero-copy, refcounted storage
// shares) — pinned below by data-address identity between the source, the
// snapshot, and the collected result.
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
    return fn();
}

const char* kPrelude =
    "package test;\n"
    "import cajeta.time.Instant;\n"
    "import cajeta.lang.Utf8;\n"
    "import cajeta.nucleo.frame.Table;\n"
    "import cajeta.nucleo.frame.FrameException;\n"
    "import cajeta.nucleo.column.Column;\n"
    "import cajeta.nucleo.column.StringColumn;\n"
    "public record Tick {\n"
    "    Instant ts;\n"
    "    float64 price;\n"
    "    float64 size;\n"
    "    Utf8 venue;\n"
    "}\n";

// Three rows; ts values are epoch-NANOS (1.5s, 2s, 2.5s).
const char* kBuild =
    "        int64[] tsv = heap int64[3];\n"
    "        tsv[0] = 1500000000; tsv[1] = 2000000000; tsv[2] = 2500000000;\n"
    "        float64[] pv = heap float64[3];\n"
    "        pv[0] = 1.5; pv[1] = 2.5; pv[2] = 3.5;\n"
    "        float64[] sv = heap float64[3];\n"
    "        sv[0] = 10.0; sv[1] = 20.0; sv[2] = 30.0;\n"
    "        String[] vv = heap String[3];\n"
    "        vv[0] = \"ARCA\"; vv[1] = \"NYSE\"; vv[2] = \"ARCA\";\n"
    "        Table<Tick> t = heap Table<Tick>(\n"
    "            Column.of<int64>(tsv), Column.of<float64>(pv),\n"
    "            Column.of<float64>(sv), StringColumn.of(vv));\n";

} // namespace

// 4.1.1 — building a plan handle executes nothing: zero executions before
// the terminal, the source unchanged (same data addresses, same values —
// the scan snapshot is a zero-copy alias), one execution after collect().
TEST(LazyPlanTests, buildingExecutesNothingCollectForcesOnce) {
    auto src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kBuild +
        "        int64 a0 = t.price.dataAddress();\n"
        "        Table<Tick> h = t.lazy();\n"
        "        int32 score = 0;\n"
        "        if (h.executions() == 0) { score = score + 1; }\n"
        "        if (t.price.dataAddress() == a0 && t.price.get(2) == 3.5) {\n"
        "            score = score + 2;\n"     // source untouched by building
        "        }\n"
        "        Table<Tick> r = h.collect();\n"
        "        if (h.executions() == 1) { score = score + 4; }\n"
        "        if (r.price.dataAddress() == a0) { score = score + 8; }\n"  // zero-copy scan
        "        if (r.rowCount() == 3 && r.price.get(0) == 1.5) { score = score + 16; }\n"
        "        if (t.price.get(2) == 3.5) { score = score + 32; }\n"       // source still intact
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 63);
}

// 4.1.2 — re-forcing the same handle returns the cached result: the
// execution count stays 1 and the result columns are the same storage.
TEST(LazyPlanTests, reforceReturnsCachedResult) {
    auto src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kBuild +
        "        Table<Tick> h = t.lazy();\n"
        "        Table<Tick> r1 = h.collect();\n"
        "        int64 a1 = r1.price.dataAddress();\n"
        "        Table<Tick> r2 = h.collect();\n"
        "        int32 score = 0;\n"
        "        if (h.executions() == 1) { score = score + 1; }\n"
        "        if (r2.price.dataAddress() == a1) { score = score + 2; }\n"
        "        if (r2.rowCount() == 3) { score = score + 4; }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// 4.1.3 — whole-column scalar reductions need no collect() (a scalar ask is
// self-evidently "compute now"), and head(n)/fetch(n) are bounded terminals:
// n rows back (clamped), numeric column slices are zero-copy views.
TEST(LazyPlanTests, scalarReductionsAndBoundedHead) {
    auto src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        "        if (t.price.max() == 3.5 && t.price.min() == 1.5) { score = score + 1; }\n"
        "        if (t.price.sum() == 7.5) { score = score + 2; }\n"
        "        if (t.ts.max() == 2500000000) { score = score + 4; }\n"
        // target-experience §3 spells `var spread = ...`; `var` doesn't yet
        // infer from an instantiated generic return (recorded in the plan
        // ledger) — the explicit type carries the same semantics.
        "        float64 spread = t.price.max() - t.price.min();\n"
        "        if (spread == 2.0) { score = score + 8; }\n"
        "        Table<Tick> h2 = t.lazy().head(2);\n"
        "        if (h2.rowCount() == 2 && h2.price.get(1) == 2.5) { score = score + 16; }\n"
        "        if (h2.price.dataAddress() == t.price.dataAddress()) { score = score + 32; }\n"
        "        Table<Tick> f = t.lazy().fetch(10);\n"   // clamps to 3
        "        if (f.rowCount() == 3) { score = score + 64; }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 127);
}

// 4.1.4 — row iteration: rows() forces and walks TYPED rows — the record is
// the row type, with Instant reconstructed from the epoch-nanos physical and
// Utf8 from the utf8 column.
TEST(LazyPlanTests, rowIterationWalksTypedRows) {
    auto src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kBuild +
        "        TickRows it = t.rows();\n"
        "        float64 acc = 0.0;\n"
        "        int32 n = 0;\n"
        "        int32 arca = 0;\n"
        "        int64 secs = 0;\n"
        "        while (it.hasNext()) {\n"
        "            Tick r = it.next();\n"
        "            acc = acc + r.price * r.size;\n"
        "            secs = secs + r.ts.getEpochSecond();\n"
        "            if (r.venue.equalsString(\"ARCA\")) { arca = arca + 1; }\n"
        "            n = n + 1;\n"
        "        }\n"
        "        int32 score = 0;\n"
        "        if (n == 3) { score = score + 1; }\n"
        "        if (acc == 170.0) { score = score + 2; }\n"      // 15+50+105
        "        if (secs == 5) { score = score + 4; }\n"          // 1+2+2
        "        if (arca == 2) { score = score + 8; }\n"
        "        Tick r1 = t.rowAt(1);\n"
        "        if (r1.price == 2.5 && r1.ts.getEpochSecond() == 2) { score = score + 16; }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 31);
}

// 4.1.5 — printing an unforced handle shows schema + plan, not rows; a
// materialized table describes schema + row count. No data values leak into
// the unforced description.
TEST(LazyPlanTests, describeShowsPlanNotRows) {
    auto src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kBuild +
        "        Table<Tick> h = t.lazy();\n"
        "        String d = h.describe();\n"
        "        int32 score = 0;\n"
        "        if (d.contains(\"unforced\") && d.contains(\"scan\")) { score = score + 1; }\n"
        "        if (!d.contains(\"1.5\") && !d.contains(\"ARCA\")) { score = score + 2; }\n"
        "        String m = t.describe();\n"
        "        if (m.contains(\"3 rows\") && m.contains(\"price\") && m.contains(\"utf8\")) {\n"
        "            score = score + 4;\n"
        "        }\n"
        "        h.collect();\n"
        "        String df = h.describe();\n"
        "        if (df.contains(\"3 rows\")) { score = score + 8; }\n"  // forced handle defers to result
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

// Terminal misuse fails loud: rowAt out of range and lazy() on an already
// lazy handle are named FrameExceptions, not silent wrong answers.
TEST(LazyPlanTests, terminalMisuseFailsLoud) {
    auto src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        "        try {\n"
        "            Tick r = t.rowAt(99);\n"
        "        } catch (FrameException e) {\n"
        "            score = score + 1;\n"
        "        }\n"
        "        Table<Tick> h = t.lazy();\n"
        "        try {\n"
        "            Table<Tick> h2 = h.lazy();\n"
        "        } catch (FrameException e) {\n"
        "            score = score + 2;\n"
        "        }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 3);
}
