//
// nucleo-frame U7 — pushdown optimization (plan 7.1.1–7.1.4; spec §5). The
// built plan is REWRITTEN at force, before execution:
//  - predicate pushdown: a filter above the scan becomes a scan-level
//    predicate — rows failing it are never materialized (`scanRows()`
//    proves it);
//  - projection pushdown: when a `select` bounds the output, the scan reads
//    exactly the referenced columns (`scanCols()` proves it);
//  - provable reorders only: a filter sinks below `with`/`fillNull` when
//    its predicate references none of the columns the node (re)defines,
//    and below `dropNulls` always; `project` blocks (it renames and
//    re-ordinals). Every rewrite is result-preserving — each case pins
//    equality against the same chain run with `optimize(false)`;
//  - the optimized plan prints (`explain()` — the spec §2.5 inspect story)
//    showing the pushed predicates and the pruned column set on the scan
//    leaf, while `describe()` still shows the chain as built.
//
// Fold shape (test-battery-restructure 2.3): the four original tests each
// paid their own JIT compile of the same six-column table program, and the
// per-test coverage measurement (2026-08-10) showed their compiler line
// footprints near-identical. The four `run()` bodies are now four static
// entry points of ONE program — each builds its own table(s) locally, so no
// measurement or plan handle can leak between scenarios — behind one
// compile, with one C++ test asserting each scenario's score separately.
// Every score bit, equality pin, and explain()/describe() string check is
// carried over verbatim.
//
#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

const char* kPrelude =
    "package test;\n"
    "import cajeta.lang.Utf8;\n"
    "import cajeta.nucleo.frame.Table;\n"
    "import cajeta.nucleo.frame.FrameException;\n"
    "import cajeta.nucleo.frame.Pred;\n"
    "import cajeta.nucleo.frame.Sels;\n"
    "import cajeta.nucleo.column.Column;\n"
    "import cajeta.nucleo.column.NullableColumn;\n"
    "import cajeta.nucleo.column.StringColumn;\n"
    "public record Wide {\n"
    "    float64 a;\n"
    "    float64 b;\n"
    "    float64 c;\n"
    "    float64 d;\n"
    "    int64 k;\n"
    "    Utf8 tag;\n"
    "}\n"
    "public record AB { float64 a; float64 b; }\n"
    "public record WideD {\n"
    "    float64 a; float64 b; float64 c; float64 d;\n"
    "    int64 k; Utf8 tag; float64 dbl;\n"
    "}\n"
    "public record NW { float64 a; @Nullable float64 nb; }\n";

// Five rows, six columns. a = 10..50, b = 1.5..5.5, c = 100..500, d = 7,
// k = 1..5, tag = x,y,x,y,x.
const char* kBuild =
    "        float64[] av = heap float64[5];\n"
    "        av[0] = 10.0; av[1] = 20.0; av[2] = 30.0; av[3] = 40.0; av[4] = 50.0;\n"
    "        float64[] bv = heap float64[5];\n"
    "        bv[0] = 1.5; bv[1] = 2.5; bv[2] = 3.5; bv[3] = 4.5; bv[4] = 5.5;\n"
    "        float64[] cv = heap float64[5];\n"
    "        cv[0] = 100.0; cv[1] = 200.0; cv[2] = 300.0; cv[3] = 400.0; cv[4] = 500.0;\n"
    "        float64[] dv = heap float64[5];\n"
    "        dv[0] = 7.0; dv[1] = 7.0; dv[2] = 7.0; dv[3] = 7.0; dv[4] = 7.0;\n"
    "        int64[] kv = heap int64[5];\n"
    "        kv[0] = 1; kv[1] = 2; kv[2] = 3; kv[3] = 4; kv[4] = 5;\n"
    "        String[] tv = heap String[5];\n"
    "        tv[0] = \"x\"; tv[1] = \"y\"; tv[2] = \"x\"; tv[3] = \"y\"; tv[4] = \"x\";\n"
    "        Table<Wide> t = heap Table<Wide>(\n"
    "            Column.of<float64>(av), Column.of<float64>(bv),\n"
    "            Column.of<float64>(cv), Column.of<float64>(dv),\n"
    "            Column.of<int64>(kv), StringColumn.of(tv));\n";

// Five rows: a = 10..50; nb = [10, null, 30, null, 50].
const char* kBuildNW =
    "        float64[] a2 = heap float64[5];\n"
    "        a2[0] = 10.0; a2[1] = 20.0; a2[2] = 30.0; a2[3] = 40.0; a2[4] = 50.0;\n"
    "        float64[] n2 = heap float64[5];\n"
    "        n2[0] = 10.0; n2[1] = 99.0; n2[2] = 30.0; n2[3] = 99.0; n2[4] = 50.0;\n"
    "        boolean[] ok2 = heap boolean[5];\n"
    "        ok2[0] = true; ok2[1] = false; ok2[2] = true; ok2[3] = false; ok2[4] = true;\n"
    "        Table<NW> t2 = heap Table<NW>(\n"
    "            Column.of<float64>(a2), NullableColumn.of<float64>(n2, ok2));\n";

// One program, four entry points — one per U7 use case. Each entry builds its
// own table(s) from kBuild / kBuildNW and returns its own score, so the
// scan counters (scanRows()/scanCols()) a scenario reads can only come from
// the plan handles that scenario built.
std::string pushdownMatrixSrc() {
    return std::string(kPrelude) +
        "public final class D {\n"
        // 7.1.1 — predicate pushdown: a filter directly above the scan runs AT
        // the scan; rows failing the predicate are never materialized
        // (scanRows() counts what the scan emitted: 3 pushed vs 5
        // unoptimized), and the result is identical to the unoptimized
        // execution of the same chain.
        "    public static int32 predicatePushdown() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        "        Table<Wide> h = t.lazy().filter((WideCols c) -> c.a() > 25.0);\n"
        "        if (h.scanRows() == -1) { score = score + 1; }\n"  // built, not run
        "        Table<Wide> r = h.collect();\n"
        "        if (r.rowCount() == 3 && r.a.get(0) == 30.0\n"
        "                && r.a.get(2) == 50.0 && r.tag.get(1).equals(\"y\")\n"
        "                && r.k.get(0) == 3) {\n"
        "            score = score + 2;\n"
        "        }\n"
        "        if (h.scanRows() == 3) { score = score + 4; }\n"   // failing rows never left the scan
        "        if (h.scanCols() == 6) { score = score + 8; }\n"   // no select -> every column
        "        Table<Wide> h2 = t.lazy().filter((WideCols c) -> c.a() > 25.0);\n"
        "        h2.optimize(false);\n"
        "        Table<Wide> r2 = h2.collect();\n"
        "        if (h2.scanRows() == 5 && h2.scanCols() == 6) { score = score + 16; }\n"
        "        boolean same = r2.rowCount() == r.rowCount();\n"
        "        int64 i = 0;\n"
        "        while (i < r.rowCount()) {\n"
        "            if (r.a.get(i) != r2.a.get(i)) { same = false; }\n"
        "            if (r.b.get(i) != r2.b.get(i)) { same = false; }\n"
        "            if (r.k.get(i) != r2.k.get(i)) { same = false; }\n"
        "            if (!r.tag.get(i).equals(r2.tag.get(i))) { same = false; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (same) { score = score + 32; }\n"
        "        return score;\n"
        "    }\n"
        // 7.1.2 — projection pushdown: a filter + select pipeline touching 2
        // of 6 columns reads exactly those 2 at the scan (scanCols(); 6
        // unoptimized), composed with the pushed predicate (scanRows());
        // results identical.
        "    public static int32 projectionPushdown() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        "        Table<Wide> f = t.lazy().filter((WideCols c) -> c.a() > 15.0);\n"
        "        Table<?> s = f.select((WideCols c, Sels ss) -> {\n"
        "            ss.add(c.a()); ss.add(c.b());\n"
        "        });\n"
        "        Table<AB> h = s.as<AB>();\n"
        "        Table<AB> r = h.collect();\n"
        "        if (r.rowCount() == 4 && r.a.get(0) == 20.0\n"
        "                && r.b.get(3) == 5.5) {\n"
        "            score = score + 1;\n"
        "        }\n"
        "        if (h.scanCols() == 2) { score = score + 2; }\n"  // a,b only - 2 of 6
        "        if (h.scanRows() == 4) { score = score + 4; }\n"  // pred pushed too
        "        Table<Wide> f2 = t.lazy().filter((WideCols c) -> c.a() > 15.0);\n"
        "        Table<?> s2 = f2.select((WideCols c, Sels ss) -> {\n"
        "            ss.add(c.a()); ss.add(c.b());\n"
        "        });\n"
        "        Table<AB> h2 = s2.as<AB>();\n"
        "        h2.optimize(false);\n"
        "        Table<AB> r2 = h2.collect();\n"
        "        if (h2.scanCols() == 6 && h2.scanRows() == 5) { score = score + 8; }\n"
        "        boolean same = r2.rowCount() == r.rowCount();\n"
        "        int64 i = 0;\n"
        "        while (i < r.rowCount()) {\n"
        "            if (r.a.get(i) != r2.a.get(i)) { same = false; }\n"
        "            if (r.b.get(i) != r2.b.get(i)) { same = false; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (same) { score = score + 16; }\n"
        "        return score;\n"
        "    }\n"
        // 7.1.3 — provable reorders only (the groupBy variant lands with U8;
        // the shipped ops pin the rule mechanics): a filter sinks below `with`
        // when its predicate references only pass-through columns and below
        // `fillNull` when it doesn't reference the filled column (reaching the
        // scan both times); a predicate on the computed / filled column BLOCKS
        // the move — pushing it would change the answer (the fillNull case is
        // constructed so the wrong order gives 0 rows, not 2). Result-set
        // equality is pinned against `optimize(false)` for both the moved and
        // the blocked shapes.
        "    public static int32 provableReorders() {\n"
        + kBuild + kBuildNW +
        "        int32 score = 0;\n"
        // A: filter on pass-through "b" sinks below the with, to the scan.
        "        Table<?> w = t.lazy().with((WideCols c) -> (c.a() * 2.0).alias(\"dbl\"));\n"
        "        Pred p = w.col(\"b\") > 2.0;\n"
        "        Table<?> f = w.filter(#p);\n"
        "        Table<?> r = f.collect();\n"
        "        if (r.rowCount() == 4 && f.scanRows() == 4) { score = score + 1; }\n"
        "        Table<WideD> rt = r.as<WideD>();\n"
        "        if (rt.dbl.get(0) == 40.0 && rt.dbl.get(3) == 100.0\n"
        "                && rt.a.get(0) == 20.0) {\n"
        "            score = score + 2;\n"
        "        }\n"
        // B: filter on the COMPUTED "dbl" is blocked - full scan, applied above.
        "        Table<?> w2 = t.lazy().with((WideCols c) -> (c.a() * 2.0).alias(\"dbl\"));\n"
        "        Pred p2 = w2.col(\"dbl\") > 50.0;\n"
        "        Table<?> f2 = w2.filter(#p2);\n"
        "        Table<?> r2 = f2.collect();\n"
        "        if (r2.rowCount() == 3 && f2.scanRows() == 5) { score = score + 4; }\n"
        "        Table<WideD> rt2 = r2.as<WideD>();\n"
        "        if (rt2.dbl.get(0) == 60.0 && rt2.a.get(2) == 50.0) { score = score + 8; }\n"
        // B equality against the unoptimized run.
        "        Table<?> w3 = t.lazy().with((WideCols c) -> (c.a() * 2.0).alias(\"dbl\"));\n"
        "        Pred p3 = w3.col(\"dbl\") > 50.0;\n"
        "        Table<?> f3 = w3.filter(#p3);\n"
        "        f3.optimize(false);\n"
        "        Table<?> r3 = f3.collect();\n"
        "        Table<WideD> rt3 = r3.as<WideD>();\n"
        "        boolean same = rt3.rowCount() == rt2.rowCount();\n"
        "        int64 i = 0;\n"
        "        while (i < rt2.rowCount()) {\n"
        "            if (rt2.dbl.get(i) != rt3.dbl.get(i)) { same = false; }\n"
        "            if (rt2.a.get(i) != rt3.a.get(i)) { same = false; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (same) { score = score + 16; }\n"
        // C: filter on "a" sinks below fillNull("nb") to the scan...
        "        Table<?> g = t2.lazy().fillNull(\"nb\", 0.0);\n"
        "        Pred q = g.col(\"a\") > 15.0;\n"
        "        Table<?> gf = g.filter(#q);\n"
        "        Table<?> gr = gf.collect();\n"
        "        if (gr.rowCount() == 4 && gf.scanRows() == 4) { score = score + 32; }\n"
        // ...but a filter on the FILLED column must see filled values:
        // nb <= 5.0 keeps exactly the two filled (null->0.0) rows; pushed
        // wrongly below the fill it would keep zero (null excludes).
        "        Table<?> g2 = t2.lazy().fillNull(\"nb\", 0.0);\n"
        "        Pred q2 = g2.col(\"nb\") <= 5.0;\n"
        "        Table<?> gf2 = g2.filter(#q2);\n"
        "        Table<?> gr2 = gf2.collect();\n"
        "        if (gr2.rowCount() == 2 && gf2.scanRows() == 5) { score = score + 64; }\n"
        "        return score;\n"
        "    }\n"
        // 7.1.4 — the optimized plan is printable (spec §2.5): `explain()`
        // shows the rewritten chain — pushed predicates and the pruned column
        // set marked on the scan leaf — while `describe()` still shows the
        // chain as built. Printing rewrites nothing (the handle collects
        // normally afterwards).
        "    public static int32 planPrinting() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        "        Table<?> w = t.lazy().with((WideCols c) -> (c.a() * 2.0).alias(\"dbl\"));\n"
        "        Pred p = w.col(\"b\") > 2.0;\n"
        "        Table<?> f = w.filter(#p);\n"
        "        if (f.describe().equals(\"Table<?> [unforced] plan: \"\n"
        "                + \"filter <- with <- scan schema {a: float64, \"\n"
        "                + \"b: float64, c: float64, d: float64, k: int64, \"\n"
        "                + \"tag: utf8, dbl: float64}\")) {\n"
        "            score = score + 1;\n"
        "        }\n"
        "        if (f.explain().equals(\"Table<?> [optimized] plan: \"\n"
        "                + \"with <- scan[filter] schema {a: float64, \"\n"
        "                + \"b: float64, c: float64, d: float64, k: int64, \"\n"
        "                + \"tag: utf8, dbl: float64}\")) {\n"
        "            score = score + 2;\n"
        "        }\n"
        "        Table<?> r = f.collect();\n"                       // explain() disturbed nothing
        "        if (r.rowCount() == 4) { score = score + 4; }\n"
        // Projection pushdown shows the pruned column set on the scan.
        "        Table<Wide> f2 = t.lazy().filter((WideCols c) -> c.a() > 15.0);\n"
        "        Table<?> s = f2.select((WideCols c2, Sels ss) -> {\n"
        "            ss.add(c2.a()); ss.add(c2.b());\n"
        "        });\n"
        "        if (s.explain().equals(\"Table<?> [optimized] plan: \"\n"
        "                + \"project <- scan[filter, cols: a,b] \"\n"
        "                + \"schema {a: float64, b: float64}\")) {\n"
        "            score = score + 8;\n"
        "        }\n"
        // Two sunk filters print their count.
        "        Table<?> w4 = t.lazy().with((WideCols c) -> (c.a() * 2.0).alias(\"dbl\"));\n"
        "        Pred p4 = w4.col(\"b\") > 2.0;\n"
        "        Table<?> f4 = w4.filter(#p4);\n"
        "        Pred p5 = f4.col(\"b\") < 5.0;\n"
        "        Table<?> f5 = f4.filter(#p5);\n"
        "        if (f5.explain().equals(\"Table<?> [optimized] plan: \"\n"
        "                + \"with <- scan[filter x2] schema {a: float64, \"\n"
        "                + \"b: float64, c: float64, d: float64, k: int64, \"\n"
        "                + \"tag: utf8, dbl: float64}\")) {\n"
        "            score = score + 16;\n"
        "        }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
}

} // namespace

// The U7 pushdown use-case matrix, one compiled program (was: PushdownTests.
// {predicatePushdownScansOnlyPassingRows, projectionPushdownReadsOnlyReferenced
// Columns, provableReordersPreserveResults, optimizedPlanPrintsTheRewrites} —
// four compiles' worth of claims, every score bit preserved):
//   7.1.1 predicate pushdown (scan-level predicate, scanRows()/scanCols(),
//   equality vs optimize(false)); 7.1.2 projection pushdown (2-of-6 columns
//   at the scan, composed with the pushed predicate, equality vs
//   optimize(false)); 7.1.3 provable reorders only (sink below with/fillNull,
//   blocked on computed/filled columns, equality vs optimize(false));
//   7.1.4 explain() prints the rewrites while describe() prints the chain as
//   built, and printing disturbs nothing.
TEST(PushdownTests, predicateProjectionReorderAndExplainMatrix) {
    auto jit = CajetaJit::compile(pushdownMatrixSrc(), "test.D");
    ASSERT_NE(jit, nullptr);
    auto predicatePushdown  = jit->lookup<int32_t (*)()>("predicatePushdown");
    auto projectionPushdown = jit->lookup<int32_t (*)()>("projectionPushdown");
    auto provableReorders   = jit->lookup<int32_t (*)()>("provableReorders");
    auto planPrinting       = jit->lookup<int32_t (*)()>("planPrinting");
    ASSERT_NE(predicatePushdown, nullptr);
    ASSERT_NE(projectionPushdown, nullptr);
    ASSERT_NE(provableReorders, nullptr);
    ASSERT_NE(planPrinting, nullptr);

    // 7.1.1 — score bits: 1 unforced handle, 2 result values, 4 pushed
    // scanRows()==3, 8 scanCols()==6, 16 unoptimized counts, 32 equality.
    EXPECT_EQ(predicatePushdown(), 63) << "predicate pushdown (7.1.1)";

    // 7.1.2 — score bits: 1 result values, 2 scanCols()==2, 4 scanRows()==4,
    // 8 unoptimized counts, 16 equality.
    EXPECT_EQ(projectionPushdown(), 31) << "projection pushdown (7.1.2)";

    // 7.1.3 — score bits: 1 sunk-below-with counts, 2 with values, 4 blocked
    // on the computed column, 8 blocked values, 16 blocked equality,
    // 32 sunk below fillNull, 64 blocked on the filled column.
    EXPECT_EQ(provableReorders(), 127) << "provable reorders (7.1.3)";

    // 7.1.4 — score bits: 1 describe(), 2 explain(), 4 collect after print,
    // 8 pruned column set printed, 16 two sunk filters printed.
    EXPECT_EQ(planPrinting(), 31) << "optimized plan printing (7.1.4)";
}
