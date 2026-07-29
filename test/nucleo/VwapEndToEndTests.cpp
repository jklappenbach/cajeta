//
// nucleo-frame U18 — the end-to-end headline (plan 18.1.1, 18.1.3; spec §4, §11).
// The spec's own vwap pipeline, run VERBATIM (modulo `.alias` for `as "vwap"`
// and the `.as<R>()` narrow the erased agg result needs before a typed sort):
//
//     ticks.filter(col.price > 0.0)
//          .groupBy(col.venue)
//          .agg((col.price * col.size).sum() / col.size.sum() as "vwap")
//          .sort(col.vwap, descending: true)
//          .collect();
//
// It proves, in one chain: (a) the whole thing is a lazy plan — nothing runs
// until collect() (scanRows() == -1 on the unforced handle); (b) the filter is
// PUSHED to the scan (a negative-price row never materializes — scanRows() == 5
// of 6); (c) projection is pushed (ts + fee pruned — scanCols() == 3 of 5);
// (d) the descending sort REORDERS the groups away from first-appearance order,
// so the sort is demonstrably doing work; (e) the values are hand-exact.
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
    "import cajeta.nucleo.frame.Pred;\n"
    "import cajeta.nucleo.frame.Sels;\n"
    "import cajeta.nucleo.frame.Aggs;\n"
    "import cajeta.nucleo.frame.Sorts;\n"
    "import cajeta.nucleo.column.Column;\n"
    "import cajeta.nucleo.column.NullableColumn;\n"
    "import cajeta.nucleo.column.StringColumn;\n"
    "public record Tick {\n"
    "    Instant ts;\n"
    "    float64 price;\n"
    "    float64 size;\n"
    "    Utf8 venue;\n"
    "    @Nullable float64 fee;\n"
    "}\n"
    "public record VenueVwap { Utf8 venue; float64 vwap; }\n";

// Six rows, two venues, NYSE appearing FIRST so first-appearance order is
// [NYSE, ARCA] — the descending vwap sort must flip it to [ARCA, NYSE].
//   NYSE rows 0,2: notional 40+80 = 120, size 2+2=4  -> vwap 30.0
//   ARCA rows 1,3,4: notional 20+60+200 = 280, size 2+2+4=8 -> vwap 35.0
//   row 5 is ARCA with price -5.0 (size 1): filtered out by price > 0.0, so it
//   never reaches the scan output and never perturbs ARCA's vwap.
const char* kBuild =
    "        int64[] tsv = heap int64[6];\n"
    "        tsv[0]=1000; tsv[1]=2000; tsv[2]=3000; tsv[3]=4000; tsv[4]=5000; tsv[5]=6000;\n"
    "        float64[] pv = heap float64[6];\n"
    "        pv[0]=20.0; pv[1]=10.0; pv[2]=40.0; pv[3]=30.0; pv[4]=50.0; pv[5]=-5.0;\n"
    "        float64[] sv = heap float64[6];\n"
    "        sv[0]=2.0; sv[1]=2.0; sv[2]=2.0; sv[3]=2.0; sv[4]=4.0; sv[5]=1.0;\n"
    "        String[] vv = heap String[6];\n"
    "        vv[0]=\"NYSE\"; vv[1]=\"ARCA\"; vv[2]=\"NYSE\";\n"
    "        vv[3]=\"ARCA\"; vv[4]=\"ARCA\"; vv[5]=\"ARCA\";\n"
    "        float64[] fv = heap float64[6];\n"
    "        fv[0]=1.0; fv[1]=1.0; fv[2]=1.0; fv[3]=1.0; fv[4]=1.0; fv[5]=1.0;\n"
    "        boolean[] fok = heap boolean[6];\n"
    "        fok[0]=true; fok[1]=true; fok[2]=true; fok[3]=true; fok[4]=true; fok[5]=true;\n"
    "        Table<Tick> t = heap Table<Tick>(\n"
    "            Column.of<int64>(tsv), Column.of<float64>(pv),\n"
    "            Column.of<float64>(sv), StringColumn.of(vv),\n"
    "            NullableColumn.of<float64>(fv, fok));\n";

} // namespace

// 18.1.1 + 18.1.3 — the verbatim vwap pipeline: lazy until collect, filter +
// projection pushed and observable, descending sort reorders, values exact.
TEST(VwapEndToEndTests, specVwapPipelineLazyPushedSortedExact) {
    auto src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        "        Table<VenueVwap> h = t.lazy()\n"
        "            .filter((TickCols c) -> { Pred p = c.price() > 0.0; return #p; })\n"
        "            .groupBy((TickCols c, Sels s) -> { s.add(c.venue()); })\n"
        "            .agg((TickCols c, Aggs ag) -> {\n"
        "                ag.add(((c.price() * c.size()).sum() / c.size().sum())\n"
        "                    .alias(\"vwap\"));\n"
        "            })\n"
        "            .as<VenueVwap>()\n"
        "            .sort((VenueVwapCols c, Sorts s) -> { s.add(c.vwap().desc()); });\n"
        // (a) nothing forced early — the whole chain is an unforced plan.
        "        if (h.scanRows() == -1) { score = score + 1; }\n"
        "        Table<VenueVwap> r = h.collect();\n"
        // (b) filter pushed to the scan: the negative-price row (6th) never
        //     materialized, so the scan produced 5 rows.
        "        if (h.scanRows() == 5) { score = score + 2; }\n"
        // (c) projection pushed: only price, size, venue are read (ts + fee pruned).
        "        if (h.scanCols() == 3) { score = score + 4; }\n"
        // (d) descending sort reordered [NYSE,ARCA] -> [ARCA,NYSE], and (e) values
        //     are hand-exact (both vwaps are exact in float64).
        "        if (r.rowCount() == 2\n"
        "                && r.venue.get(0).equals(\"ARCA\") && r.vwap.get(0) == 35.0\n"
        "                && r.venue.get(1).equals(\"NYSE\") && r.vwap.get(1) == 30.0) {\n"
        "            score = score + 8;\n"
        "        }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

// 18.1.3 (companion) — the SAME pipeline ascending yields the identical rows in
// the opposite order: the sort key drives order, and the aggregate values are
// invariant to it (correctness independent of the observable optimization).
TEST(VwapEndToEndTests, ascendingSortFlipsOrderSameValues) {
    auto src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kBuild +
        "        Table<VenueVwap> r = t.lazy()\n"
        "            .filter((TickCols c) -> { Pred p = c.price() > 0.0; return #p; })\n"
        "            .groupBy((TickCols c, Sels s) -> { s.add(c.venue()); })\n"
        "            .agg((TickCols c, Aggs ag) -> {\n"
        "                ag.add(((c.price() * c.size()).sum() / c.size().sum())\n"
        "                    .alias(\"vwap\"));\n"
        "            })\n"
        "            .as<VenueVwap>()\n"
        "            .sort((VenueVwapCols c, Sorts s) -> { s.add(c.vwap()); })\n"
        "            .collect();\n"
        "        int32 score = 0;\n"
        "        if (r.rowCount() == 2\n"
        "                && r.venue.get(0).equals(\"NYSE\") && r.vwap.get(0) == 30.0\n"
        "                && r.venue.get(1).equals(\"ARCA\") && r.vwap.get(1) == 35.0) {\n"
        "            score = 1;\n"
        "        }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
