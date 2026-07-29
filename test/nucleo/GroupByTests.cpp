//
// nucleo-frame U8 — groupBy/agg (plan 8.1.1–8.3.1; spec §4.4). Grouping is
// LAZY like every other op: `groupBy` names the keys, `agg` completes the
// node, and nothing runs until a terminal:
//  - the agg family (sum/mean/min/max/count/first/last) over float64 and
//    exact-int64 columns, plus the headline vwap — a fused elementwise
//    expression feeding a reduction, divided by another reduction:
//    `(c.price() * c.size()).sum() / c.size().sum()`;
//  - NULL semantics (spec §7): aggregates SKIP nulls (mean = sum over the
//    non-null count; count() counts non-null), an all-null group's mean /
//    min / max / first / last is NULL while its sum is 0 (the identity),
//    and a null KEY forms its own group rather than vanishing;
//  - the result is a schema-erased `Table<?>` narrowed by the checked
//    `.as<R>()`; an unnamed aggregate, a computed group key, and an
//    unknown dynamic column all fail LOUD at plan build (nothing runs);
//  - the U7 rewriter sinks a filter below the group when the predicate
//    reads only key columns (deferred item 7.1.3), and blocks it when the
//    predicate reads an aggregate output.
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
    "import cajeta.time.Instant;\n"
    "import cajeta.lang.Utf8;\n"
    "import cajeta.nucleo.frame.Table;\n"
    "import cajeta.nucleo.frame.FrameException;\n"
    "import cajeta.nucleo.frame.Agg;\n"
    "import cajeta.nucleo.frame.Aggs;\n"
    "import cajeta.nucleo.frame.Pred;\n"
    "import cajeta.nucleo.frame.Sels;\n"
    "import cajeta.nucleo.column.Column;\n"
    "import cajeta.nucleo.column.NullableColumn;\n"
    "import cajeta.nucleo.column.StringColumn;\n"
    "public record Tick {\n"
    "    Instant ts;\n"
    "    float64 price;\n"
    "    float64 size;\n"
    "    Utf8 venue;\n"
    "    @Nullable float64 fee;\n"
    "}\n";

// Five rows, two venues. Sizes chosen so both vwaps are EXACT in float64:
//   ARCA rows 0,2,4: notional 20+60+200 = 280, size 8  -> vwap 35.0
//   NYSE rows 1,3:   notional 40+80     = 120, size 4  -> vwap 30.0
// fee is null for BOTH NYSE rows (an all-null group) and present for
// every ARCA row.
const char* kBuild =
    "        int64[] tsv = heap int64[5];\n"
    "        tsv[0] = 1000; tsv[1] = 2000; tsv[2] = 3000;\n"
    "        tsv[3] = 4000; tsv[4] = 5000;\n"
    "        float64[] pv = heap float64[5];\n"
    "        pv[0] = 10.0; pv[1] = 20.0; pv[2] = 30.0; pv[3] = 40.0; pv[4] = 50.0;\n"
    "        float64[] sv = heap float64[5];\n"
    "        sv[0] = 2.0; sv[1] = 2.0; sv[2] = 2.0; sv[3] = 2.0; sv[4] = 4.0;\n"
    "        String[] vv = heap String[5];\n"
    "        vv[0] = \"ARCA\"; vv[1] = \"NYSE\"; vv[2] = \"ARCA\";\n"
    "        vv[3] = \"NYSE\"; vv[4] = \"ARCA\";\n"
    "        float64[] fv = heap float64[5];\n"
    "        fv[0] = 1.0; fv[1] = 99.0; fv[2] = 3.0; fv[3] = 99.0; fv[4] = 5.0;\n"
    "        boolean[] fok = heap boolean[5];\n"
    "        fok[0] = true; fok[1] = false; fok[2] = true;\n"
    "        fok[3] = false; fok[4] = true;\n"
    "        Table<Tick> t = heap Table<Tick>(\n"
    "            Column.of<int64>(tsv), Column.of<float64>(pv),\n"
    "            Column.of<float64>(sv), StringColumn.of(vv),\n"
    "            NullableColumn.of<float64>(fv, fok));\n";

} // namespace

// 8.1.1 — the agg family over float64 and exact int64, and the headline
// vwap: an elementwise product feeding a reduction, divided by another
// reduction. Groups come out in FIRST-APPEARANCE order (ARCA, NYSE).
TEST(GroupByTests, aggFamilyAndVwapMatchHandResults) {
    auto src = std::string(kPrelude) +
        "public record VenueVwap { Utf8 venue; float64 vwap; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        // The headline: vwap per venue.
        "        Table<Tick> g = t.lazy().groupBy((TickCols c, Sels s) -> {\n"
        "            s.add(c.venue());\n"
        "        });\n"
        "        if (g.executions() == 0) { score = score + 1; }\n"  // built, not run
        // The spec's own spelling, fully inline: an elementwise product
        // feeding a reduction, divided by another reduction, then named.
        "        Table<?> a = g.agg((TickCols c, Aggs ag) -> {\n"
        "            ag.add(((c.price() * c.size()).sum() / c.size().sum())\n"
        "                .alias(\"vwap\"));\n"
        "        });\n"
        "        Table<VenueVwap> vh = a.as<VenueVwap>();\n"
        "        Table<VenueVwap> vr = vh.collect();\n"
        "        if (vr.rowCount() == 2 && vr.venue.get(0).equals(\"ARCA\")\n"
        "                && vr.venue.get(1).equals(\"NYSE\")\n"
        "                && vr.vwap.get(0) == 35.0 && vr.vwap.get(1) == 30.0) {\n"
        "            score = score + 2;\n"
        "        }\n"
        // The full family over float64 `price`.
        "        Table<?> f = t.lazy().groupBy((TickCols c, Sels s) -> {\n"
        "            s.add(c.venue());\n"
        "        }).agg((TickCols c, Aggs ag) -> {\n"
        "            ag.add(c.price().sum().alias(\"psum\"));\n"
        "            ag.add(c.price().mean().alias(\"pmean\"));\n"
        "            ag.add(c.price().min().alias(\"pmin\"));\n"
        "            ag.add(c.price().max().alias(\"pmax\"));\n"
        "            ag.add(c.price().count().alias(\"pcount\"));\n"
        "            ag.add(c.price().first().alias(\"pfirst\"));\n"
        "            ag.add(c.price().last().alias(\"plast\"));\n"
        "        });\n"
        "        Table<?> fr = f.collect();\n"
        "        if (fr.rowCount() == 2 && fr.width() == 8) { score = score + 4; }\n"
        "        if (fr.f64At(\"psum\", 0) == 90.0 && fr.f64At(\"psum\", 1) == 60.0\n"
        "                && fr.f64At(\"pmean\", 0) == 30.0\n"
        "                && fr.f64At(\"pmean\", 1) == 30.0\n"
        "                && fr.f64At(\"pmin\", 0) == 10.0\n"
        "                && fr.f64At(\"pmax\", 0) == 50.0\n"
        "                && fr.f64At(\"pmin\", 1) == 20.0\n"
        "                && fr.f64At(\"pmax\", 1) == 40.0) {\n"
        "            score = score + 8;\n"
        "        }\n"
        "        if (fr.i64At(\"pcount\", 0) == 3 && fr.i64At(\"pcount\", 1) == 2\n"
        "                && fr.f64At(\"pfirst\", 0) == 10.0\n"
        "                && fr.f64At(\"plast\", 0) == 50.0\n"
        "                && fr.f64At(\"pfirst\", 1) == 20.0\n"
        "                && fr.f64At(\"plast\", 1) == 40.0) {\n"
        "            score = score + 16;\n"
        "        }\n"
        // Exact int64 aggregates stay int64 — no float64 round-trip.
        "        Table<?> i = t.lazy().groupBy((TickCols c, Sels s) -> {\n"
        "            s.add(c.venue());\n"
        "        }).agg((TickCols c, Aggs ag) -> {\n"
        "            ag.add(c.ts().min().alias(\"tmin\"));\n"
        "            ag.add(c.ts().max().alias(\"tmax\"));\n"
        "            ag.add(c.ts().first().alias(\"tfirst\"));\n"
        "            ag.add(c.ts().sum().alias(\"tsum\"));\n"
        "        });\n"
        "        Table<?> ir = i.collect();\n"
        "        if (ir.i64At(\"tmin\", 0) == 1000 && ir.i64At(\"tmax\", 0) == 5000\n"
        "                && ir.i64At(\"tfirst\", 1) == 2000\n"
        "                && ir.i64At(\"tsum\", 0) == 9000\n"
        "                && ir.i64At(\"tsum\", 1) == 6000) {\n"
        "            score = score + 32;\n"
        "        }\n"
        // Multi-key grouping: (venue, size) — ARCA splits into 2.0 and 4.0.
        "        Table<?> m = t.lazy().groupBy((TickCols c, Sels s) -> {\n"
        "            s.add(c.venue());\n"
        "            s.add(c.size());\n"
        "        }).agg((TickCols c, Aggs ag) -> {\n"
        "            ag.add(c.price().sum().alias(\"psum\"));\n"
        "        });\n"
        "        Table<?> mr = m.collect();\n"
        "        if (mr.rowCount() == 3 && mr.f64At(\"psum\", 0) == 40.0\n"
        "                && mr.f64At(\"psum\", 1) == 60.0\n"
        "                && mr.f64At(\"psum\", 2) == 50.0) {\n"
        "            score = score + 64;\n"
        "        }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 127);
}

// 8.1.2 — null semantics: aggregates skip nulls, so `mean` divides by the
// NON-NULL count; an all-null group yields null mean/min/max/first/last
// but sum 0 and count 0; and a null KEY forms its own group.
TEST(GroupByTests, aggsSkipNullsAndNullKeysGroupTogether) {
    auto src = std::string(kPrelude) +
        "public record KeyedN { @Nullable float64 k; float64 v; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        // fee is [1, null, 3, null, 5]: ARCA has all three, NYSE none.
        "        Table<?> r = t.lazy().groupBy((TickCols c, Sels s) -> {\n"
        "            s.add(c.venue());\n"
        "        }).agg((TickCols c, Aggs ag) -> {\n"
        "            ag.add(c.fee().sum().alias(\"fsum\"));\n"
        "            ag.add(c.fee().mean().alias(\"fmean\"));\n"
        "            ag.add(c.fee().count().alias(\"fcount\"));\n"
        "            ag.add(c.fee().min().alias(\"fmin\"));\n"
        "            ag.add(c.fee().first().alias(\"ffirst\"));\n"
        "        }).collect();\n"
        // ARCA: fees 1,3,5 -> sum 9, mean 3 (9/3, not 9/5), count 3.
        "        if (r.f64At(\"fsum\", 0) == 9.0 && r.f64At(\"fmean\", 0) == 3.0\n"
        "                && r.i64At(\"fcount\", 0) == 3\n"
        "                && r.f64At(\"fmin\", 0) == 1.0\n"
        "                && r.f64At(\"ffirst\", 0) == 1.0) {\n"
        "            score = score + 1;\n"
        "        }\n"
        // NYSE: every fee null -> sum 0 (identity), count 0, the rest NULL.
        "        if (r.f64At(\"fsum\", 1) == 0.0 && r.i64At(\"fcount\", 1) == 0) {\n"
        "            score = score + 2;\n"
        "        }\n"
        "        if (!r.validAt(\"fmean\", 1) && !r.validAt(\"fmin\", 1)\n"
        "                && !r.validAt(\"ffirst\", 1)) {\n"
        "            score = score + 4;\n"
        "        }\n"
        // The ARCA cells of those same columns ARE valid — nullability is
        // per cell, not a whole-column verdict.
        "        if (r.validAt(\"fmean\", 0) && r.validAt(\"fmin\", 0)) {\n"
        "            score = score + 8;\n"
        "        }\n"
        // A NULL KEY forms its own group: k = [1, null, 1, null, 2].
        "        float64[] kv = heap float64[5];\n"
        "        kv[0] = 1.0; kv[1] = 77.0; kv[2] = 1.0; kv[3] = 77.0; kv[4] = 2.0;\n"
        "        boolean[] kok = heap boolean[5];\n"
        "        kok[0] = true; kok[1] = false; kok[2] = true;\n"
        "        kok[3] = false; kok[4] = true;\n"
        "        float64[] nv = heap float64[5];\n"
        "        nv[0] = 10.0; nv[1] = 20.0; nv[2] = 30.0; nv[3] = 40.0; nv[4] = 50.0;\n"
        "        Table<KeyedN> kt = heap Table<KeyedN>(\n"
        "            NullableColumn.of<float64>(kv, kok), Column.of<float64>(nv));\n"
        "        Table<?> kr = kt.lazy().groupBy((KeyedNCols c, Sels s) -> {\n"
        "            s.add(c.k());\n"
        "        }).agg((KeyedNCols c, Aggs ag) -> {\n"
        "            ag.add(c.v().sum().alias(\"vsum\"));\n"
        "            ag.add(c.v().count().alias(\"n\"));\n"
        "        }).collect();\n"
        // Groups: k=1 {10,30}, k=null {20,40}, k=2 {50} — three groups, and
        // the null-key group carries its rows rather than dropping them.
        "        if (kr.rowCount() == 3 && kr.f64At(\"vsum\", 0) == 40.0\n"
        "                && kr.f64At(\"vsum\", 1) == 60.0\n"
        "                && kr.f64At(\"vsum\", 2) == 50.0\n"
        "                && kr.i64At(\"n\", 1) == 2) {\n"
        "            score = score + 16;\n"
        "        }\n"
        "        if (kr.validAt(\"k\", 0) && !kr.validAt(\"k\", 1)\n"
        "                && kr.validAt(\"k\", 2)) {\n"
        "            score = score + 32;\n"
        "        }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 63);
}

// 8.1.3 — the result is `Table<?>` narrowed by the checked `.as<R>()`, and
// the malformed shapes fail LOUD at plan build (nothing executes): an
// unnamed aggregate, a computed group key, an unknown column, and a
// groupBy left dangling without its agg.
TEST(GroupByTests, erasedResultNarrowsAndBadAggsFailAtPlanBuild) {
    auto src = std::string(kPrelude) +
        "public record VenueStats { Utf8 venue; float64 psum; }\n"
        "public record WrongStats { Utf8 venue; float64 nope; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        "        Table<?> a = t.lazy().groupBy((TickCols c, Sels s) -> {\n"
        "            s.add(c.venue());\n"
        "        }).agg((TickCols c, Aggs ag) -> {\n"
        "            ag.add(c.price().sum().alias(\"psum\"));\n"
        "        });\n"
        "        if (a.describe().equals(\"Table<?> [unforced] plan: \"\n"
        "                + \"groupBy <- scan schema {venue: utf8, \"\n"
        "                + \"psum: float64}\")) {\n"
        "            score = score + 1;\n"
        "        }\n"
        "        Table<VenueStats> vh = a.as<VenueStats>();\n"
        "        Table<VenueStats> vr = vh.collect();\n"
        "        if (vr.rowCount() == 2 && vr.psum.get(0) == 90.0) { score = score + 2; }\n"
        // A record naming a column the group result doesn't have.
        "        Table<?> a2 = t.lazy().groupBy((TickCols c, Sels s) -> {\n"
        "            s.add(c.venue());\n"
        "        }).agg((TickCols c, Aggs ag) -> {\n"
        "            ag.add(c.price().sum().alias(\"psum\"));\n"
        "        });\n"
        "        try {\n"
        "            Table<WrongStats> w = a2.as<WrongStats>();\n"
        "            score = score + 1000;\n"
        "        } catch (FrameException e) {\n"
        "            if (e.getMessage().contains(\"nope\")) { score = score + 4; }\n"
        "        }\n"
        // An UNNAMED aggregate: no alias, no output name.
        "        try {\n"
        "            Table<?> b = t.lazy().groupBy((TickCols c, Sels s) -> {\n"
        "                s.add(c.venue());\n"
        "            }).agg((TickCols c, Aggs ag) -> {\n"
        "                ag.add(c.price().sum());\n"
        "            });\n"
        "            score = score + 1000;\n"
        "        } catch (FrameException e) {\n"
        "            if (e.getMessage().contains(\"alias\")) { score = score + 8; }\n"
        "        }\n"
        // A COMPUTED group key — keys must be plain column references.
        "        try {\n"
        "            Table<?> b2 = t.lazy().groupBy((TickCols c, Sels s) -> {\n"
        "                s.add((c.price() * 2.0).alias(\"dbl\"));\n"
        "            }).agg((TickCols c, Aggs ag) -> {\n"
        "                ag.add(c.price().sum().alias(\"psum\"));\n"
        "            });\n"
        "            score = score + 1000;\n"
        "        } catch (FrameException e) {\n"
        "            if (e.getMessage().contains(\"key\")) { score = score + 16; }\n"
        "        }\n"
        // A groupBy never completed by an agg cannot be forced.
        "        try {\n"
        "            Table<Tick> g = t.lazy().groupBy((TickCols c, Sels s) -> {\n"
        "                s.add(c.venue());\n"
        "            });\n"
        "            Table<Tick> bad = g.collect();\n"
        "            score = score + 1000;\n"
        "        } catch (FrameException e) {\n"
        "            if (e.getMessage().contains(\"agg\")) { score = score + 32; }\n"
        "        }\n"
        // An unknown column through the dynamic accessor, at plan build.
        "        try {\n"
        "            Table<?> b3 = t.lazy().groupBy((TickCols c, Sels s) -> {\n"
        "                s.add(c.venue());\n"
        "            });\n"
        "            Agg bogus = b3.col(\"nosuch\").sum();\n"
        "            score = score + 1000;\n"
        "        } catch (FrameException e) {\n"
        "            if (e.getMessage().contains(\"nosuch\")) { score = score + 64; }\n"
        "        }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 127);
}

// 7.1.3 (deferred from U7) + 8.x — the rewriter sinks a filter below the
// group when its predicate reads only KEY columns (which pass through
// unchanged), and blocks it when the predicate reads an aggregate output.
// Both shapes pin result equality against the unoptimized plan.
TEST(GroupByTests, filterSinksBelowGroupByOnlyWhenProvable) {
    auto src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        // Predicate on the KEY `venue` -> sinks past the group to the scan,
        // so the scan emits only the 3 ARCA rows.
        "        Table<?> g = t.lazy().groupBy((TickCols c, Sels s) -> {\n"
        "            s.add(c.venue());\n"
        "        }).agg((TickCols c, Aggs ag) -> {\n"
        "            ag.add(c.price().sum().alias(\"psum\"));\n"
        "        });\n"
        "        Pred kp = g.colStr(\"venue\") == \"ARCA\";\n"
        "        Table<?> gf = g.filter(#kp);\n"
        "        Table<?> gr = gf.collect();\n"
        "        if (gr.rowCount() == 1 && gr.f64At(\"psum\", 0) == 90.0) {\n"
        "            score = score + 1;\n"
        "        }\n"
        "        if (gf.scanRows() == 3) { score = score + 2; }\n"
        // Predicate on the AGGREGATE output -> blocked, full scan.
        "        Table<?> g2 = t.lazy().groupBy((TickCols c, Sels s) -> {\n"
        "            s.add(c.venue());\n"
        "        }).agg((TickCols c, Aggs ag) -> {\n"
        "            ag.add(c.price().sum().alias(\"psum\"));\n"
        "        });\n"
        "        Pred ap = g2.col(\"psum\") > 70.0;\n"
        "        Table<?> gf2 = g2.filter(#ap);\n"
        "        Table<?> gr2 = gf2.collect();\n"
        "        if (gr2.rowCount() == 1 && gr2.f64At(\"psum\", 0) == 90.0) {\n"
        "            score = score + 4;\n"
        "        }\n"
        "        if (gf2.scanRows() == 5) { score = score + 8; }\n"
        // Same chain unoptimized — identical results either way.
        "        Table<?> g3 = t.lazy().groupBy((TickCols c, Sels s) -> {\n"
        "            s.add(c.venue());\n"
        "        }).agg((TickCols c, Aggs ag) -> {\n"
        "            ag.add(c.price().sum().alias(\"psum\"));\n"
        "        });\n"
        "        Pred kp3 = g3.colStr(\"venue\") == \"ARCA\";\n"
        "        Table<?> gf3 = g3.filter(#kp3);\n"
        "        gf3.optimize(false);\n"
        "        Table<?> gr3 = gf3.collect();\n"
        "        if (gr3.rowCount() == gr.rowCount()\n"
        "                && gr3.f64At(\"psum\", 0) == gr.f64At(\"psum\", 0)\n"
        "                && gf3.scanRows() == 5) {\n"
        "            score = score + 16;\n"
        "        }\n"
        // Projection pushdown through the group: only venue + price are read.
        "        if (gf.scanCols() == 2) { score = score + 32; }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 63);
}
