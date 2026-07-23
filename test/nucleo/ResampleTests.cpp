//
// nucleo-frame U10 — resample (plan 10.1.1-10.3.1; spec §8). Resample is
// LAZY: `resample(col.ts, every)` names the time column and a fixed
// `Duration` bucket width, building a group node keyed by time bucket, and
// `.agg(...)` completes it — nothing runs until a terminal.
//  - Buckets are epoch-aligned and LEFT-CLOSED by default (`[start,
//    start+every)`), with `origin`/`offset`/`closed` overrides. The
//    downsample agg family (first/last/min/max/sum/count/mean — the OHLC
//    set) reduces each bucket, keyed by the bucket-start time.
//  - Resample ESTABLISHES time order (it sorts by the time column) so
//    `first`/`last` are earliest/latest by TIME, not by input row order —
//    an unsorted source never silently mis-buckets (spec §8.3).
//  - An EMPTY bucket in the middle of the range is emitted as a NULL row
//    (no data is *missing*, not zero — spec §8.4); every resample aggregate
//    is therefore nullable, and forward-fill is an explicit `fillNull`
//    afterward, never implicit.
//  - The result is a schema-erased `Table<?>` narrowed by the checked
//    `.as<R>()`; a non-integer / absent time column fails LOUD at plan
//    build. Calendar offsets (months/years) are the one spec-sanctioned
//    deferral.
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
    "import cajeta.time.Duration;\n"
    "import cajeta.nucleo.frame.Table;\n"
    "import cajeta.nucleo.frame.FrameException;\n"
    "import cajeta.nucleo.frame.Aggs;\n"
    "import cajeta.nucleo.column.Column;\n"
    "public record Bar {\n"
    "    Instant ts;\n"
    "    float64 px;\n"
    "    float64 vol;\n"
    "}\n";

// Four rows, SCRAMBLED in time so the internal time-ordering is observable.
// every = 60s = 60_000_000_000 ns. Time buckets (left-closed):
//   b0 [0,60s):   t=0s  (px 12, vol 1)   and  t=10s (px 10, vol 2)
//   b1 [60,120s): t=70s (px 20, vol 1)
//   b2 [120,180s): EMPTY  -> a null row
//   b3 [180,240s): t=200s (px 30, vol 3)
// Input row order is t10, t0, t200, t70 (not sorted).
const char* kBuild =
    "        int64[] tsv = heap int64[4];\n"
    "        tsv[0] = 10000000000; tsv[1] = 0;\n"
    "        tsv[2] = 200000000000; tsv[3] = 70000000000;\n"
    "        float64[] pv = heap float64[4];\n"
    "        pv[0] = 10.0; pv[1] = 12.0; pv[2] = 30.0; pv[3] = 20.0;\n"
    "        float64[] vv = heap float64[4];\n"
    "        vv[0] = 2.0; vv[1] = 1.0; vv[2] = 3.0; vv[3] = 1.0;\n"
    "        Table<Bar> t = heap Table<Bar>(Column.of<int64>(tsv),\n"
    "            Column.of<float64>(pv), Column.of<float64>(vv));\n";

} // namespace

// 10.1.1 / 10.1.2 / 10.1.3 — the downsample agg family over epoch-aligned
// left-closed buckets, first/last by TIME (establishes order), an empty
// middle bucket emitted as a NULL row, and the erased result narrowing.
TEST(ResampleTests, downsampleFamilyEmptyBucketsAndTimeOrder) {
    auto src = std::string(kPrelude) +
        "public record MinuteBar { Instant ts; @Nullable float64 lastpx; }\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        "        Table<?> h = t.lazy().resample((BarCols c) -> c.ts(),\n"
        "            Duration.ofSeconds(60)).agg((BarCols c, Aggs ag) -> {\n"
        "            ag.add(c.px().first().alias(\"o\"));\n"
        "            ag.add(c.px().max().alias(\"hi\"));\n"
        "            ag.add(c.px().min().alias(\"lo\"));\n"
        "            ag.add(c.px().last().alias(\"cl\"));\n"
        "            ag.add(c.vol().sum().alias(\"v\"));\n"
        "            ag.add(c.px().count().alias(\"n\"));\n"
        "            ag.add(c.px().mean().alias(\"m\"));\n"
        "        });\n"
        "        if (h.executions() == 0) { score = score + 1; }\n"  // built, not run
        "        Table<?> r = h.collect();\n"
        // Four buckets emitted (b0..b3), including the empty b2.
        "        if (r.rowCount() == 4 && r.width() == 8) { score = score + 2; }\n"
        // The bucket-start time column is epoch-aligned.
        "        if (r.i64At(\"ts\", 0) == 0\n"
        "                && r.i64At(\"ts\", 1) == 60000000000\n"
        "                && r.i64At(\"ts\", 2) == 120000000000\n"
        "                && r.i64At(\"ts\", 3) == 180000000000) {\n"
        "            score = score + 4;\n"
        "        }\n"
        // first/last are by TIME: b0's first is the t=0s row (px 12), its
        // last is the t=10s row (px 10) — NOT the input row order.
        "        if (r.f64At(\"o\", 0) == 12.0 && r.f64At(\"cl\", 0) == 10.0\n"
        "                && r.f64At(\"hi\", 0) == 12.0 && r.f64At(\"lo\", 0) == 10.0\n"
        "                && r.f64At(\"m\", 0) == 11.0 && r.f64At(\"v\", 0) == 3.0\n"
        "                && r.i64At(\"n\", 0) == 2) {\n"
        "            score = score + 8;\n"
        "        }\n"
        // b1: the single t=70s row.
        "        if (r.f64At(\"o\", 1) == 20.0 && r.f64At(\"cl\", 1) == 20.0\n"
        "                && r.f64At(\"v\", 1) == 1.0 && r.i64At(\"n\", 1) == 1) {\n"
        "            score = score + 16;\n"
        "        }\n"
        // b2 is EMPTY -> a null row: EVERY aggregate is null, even sum and
        // count (no data is missing, not zero). The time key stays present.
        "        if (r.validAt(\"ts\", 2) && !r.validAt(\"o\", 2)\n"
        "                && !r.validAt(\"cl\", 2) && !r.validAt(\"v\", 2)\n"
        "                && !r.validAt(\"n\", 2) && !r.validAt(\"m\", 2)) {\n"
        "            score = score + 32;\n"
        "        }\n"
        // The erased result narrows on a record whose agg field is nullable
        // (resample aggregates always are). A last-only resample:
        "        Table<?> h2 = t.lazy().resample((BarCols c) -> c.ts(),\n"
        "            Duration.ofSeconds(60)).agg((BarCols c, Aggs ag) -> {\n"
        "            ag.add(c.px().last().alias(\"lastpx\"));\n"
        "        });\n"
        "        Table<MinuteBar> mb = h2.as<MinuteBar>().collect();\n"
        "        if (mb.rowCount() == 4 && mb.ts.get(3) == 180000000000\n"
        "                && mb.lastpx.get(3) == 30.0 && !mb.lastpx.isValid(2)) {\n"
        "            score = score + 64;\n"
        "        }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 127);
}

// 10.1.1 — the alignment overrides: LEFT- vs RIGHT-closed on a boundary
// value, and an `offset` that shifts the bucket grid.
TEST(ResampleTests, closedAndOffsetOverrides) {
    auto src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // Two rows: one exactly on the 60s boundary, one at 45s.
        "        int64[] tsv = heap int64[2];\n"
        "        tsv[0] = 60000000000; tsv[1] = 45000000000;\n"
        "        float64[] pv = heap float64[2];\n"
        "        pv[0] = 5.0; pv[1] = 9.0;\n"
        "        float64[] vv = heap float64[2];\n"
        "        vv[0] = 1.0; vv[1] = 1.0;\n"
        "        Table<Bar> t = heap Table<Bar>(Column.of<int64>(tsv),\n"
        "            Column.of<float64>(pv), Column.of<float64>(vv));\n"
        "        int32 score = 0;\n"
        // LEFT-closed (default): the 60s row opens bucket b1 [60,120), the
        // 45s row is in b0 [0,60). Two buckets, both non-empty.
        "        Table<?> rl = t.lazy().resample((BarCols c) -> c.ts(),\n"
        "            Duration.ofSeconds(60)).agg((BarCols c, Aggs ag) -> {\n"
        "            ag.add(c.px().last().alias(\"cl\"));\n"
        "        }).collect();\n"
        "        if (rl.rowCount() == 2 && rl.i64At(\"ts\", 0) == 0\n"
        "                && rl.f64At(\"cl\", 0) == 9.0\n"
        "                && rl.i64At(\"ts\", 1) == 60000000000\n"
        "                && rl.f64At(\"cl\", 1) == 5.0) {\n"
        "            score = score + 1;\n"
        "        }\n"
        // RIGHT-closed: (start, start+every]. The 60s row now CLOSES bucket
        // b0 (0,60], the 45s row is also in b0. One bucket, both rows.
        "        Table<?> rr = t.lazy().resample((BarCols c) -> c.ts(),\n"
        "            Duration.ofSeconds(60), Duration.zero(), Duration.zero(),\n"
        "            true).agg((BarCols c, Aggs ag) -> {\n"
        "            ag.add(c.px().count().alias(\"n\"));\n"
        "            ag.add(c.px().last().alias(\"cl\"));\n"
        "        }).collect();\n"
        "        if (rr.rowCount() == 1 && rr.i64At(\"n\", 0) == 2\n"
        "                && rr.f64At(\"cl\", 0) == 5.0) {\n"
        "            score = score + 2;\n"
        "        }\n"
        // OFFSET shifts the grid by 30s: boundaries at 30s, 90s. The 45s
        // row falls in [30,90), the 60s row also in [30,90). One bucket
        // starting at 30s.
        "        Table<?> ro = t.lazy().resample((BarCols c) -> c.ts(),\n"
        "            Duration.ofSeconds(60), Duration.zero(),\n"
        "            Duration.ofSeconds(30), false).agg((BarCols c, Aggs ag) -> {\n"
        "            ag.add(c.px().count().alias(\"n\"));\n"
        "        }).collect();\n"
        "        if (ro.rowCount() == 1 && ro.i64At(\"ts\", 0) == 30000000000\n"
        "                && ro.i64At(\"n\", 0) == 2) {\n"
        "            score = score + 4;\n"
        "        }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// 10.1.2 / 10.3.1 — the dynamic string form, and bad time columns fail
// LOUD at plan build (never a silent mis-bucket).
TEST(ResampleTests, dynamicFormAndBadTimeColumnFailLoud) {
    auto src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        // The dynamic string form: resample keyed by a named column.
        "        Table<?> r = t.lazy().resample(\"ts\", Duration.ofSeconds(60))\n"
        "            .agg((BarCols c, Aggs ag) -> {\n"
        "            ag.add(c.px().last().alias(\"cl\"));\n"
        "        }).collect();\n"
        "        if (r.rowCount() == 4 && r.f64At(\"cl\", 3) == 30.0) {\n"
        "            score = score + 1;\n"
        "        }\n"
        // A non-integer time column: px is float64, not an epoch column.
        "        try {\n"
        "            Table<?> bad = t.lazy().resample(\"px\",\n"
        "                Duration.ofSeconds(60)).agg((BarCols c, Aggs ag) -> {\n"
        "                ag.add(c.px().last().alias(\"cl\"));\n"
        "            }).collect();\n"
        "        } catch (FrameException e) {\n"
        "            if (e.getMessage().contains(\"px\")\n"
        "                    && e.getMessage().contains(\"time\")) {\n"
        "                score = score + 2;\n"
        "            }\n"
        "        }\n"
        // An absent time column names the schema it searched.
        "        try {\n"
        "            Table<?> bad2 = t.lazy().resample(\"nope\",\n"
        "                Duration.ofSeconds(60)).agg((BarCols c, Aggs ag) -> {\n"
        "                ag.add(c.px().last().alias(\"cl\"));\n"
        "            }).collect();\n"
        "        } catch (FrameException e) {\n"
        "            if (e.getMessage().contains(\"nope\")) { score = score + 4; }\n"
        "        }\n"
        // A resample never completed by an agg cannot be forced.
        "        try {\n"
        "            Table<?> bad3 = t.lazy().resample(\"ts\",\n"
        "                Duration.ofSeconds(60)).collect();\n"
        "        } catch (FrameException e) {\n"
        "            if (e.getMessage().contains(\"agg\")) { score = score + 8; }\n"
        "        }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}
