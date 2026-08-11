//
// nucleo-frame U11 — rolling/window (plan 11.1.1-11.3.1; spec §8 resolution).
// Rolling is the SLIDING-window sibling of resample: where resample cuts
// fixed non-overlapping time buckets (one row per bucket), rolling produces
// one row per INPUT row, each aggregating that row's trailing window. Both
// share the one windowing primitive.
//  - TIME window `rolling(col.ts, window: Duration)`: for the row anchored
//    at time t, the window is the trailing `(t - window, t]` (right-closed),
//    over rows up to that anchor in time order — so rolling ESTABLISHES time
//    order like resample, and first/last/OHLC are by time.
//  - COUNT window `rolling(n)`: the trailing n rows `[i-n+1, i]` in input
//    order, clamped at the start (the first rows see a shorter window).
//  - The window is never empty (it always contains its anchor), so the
//    aggregate nullability is the ordinary groupBy rule (sum/count non-null,
//    value-pickers nullable iff the input is) — NOT resample's always-null.
//  - The result is a schema-erased `Table<?>`; the time form carries the
//    anchor time column, the count form the aggregates alone.
//
// Fold shape (test-battery-restructure 2.3, spec §3.1/§3.2b): the three
// original tests compiled three separate programs whose compiler footprints
// were measured near-identical (the nucleo table family's ~18.8k shared
// lines, .coverage/index 2026-08-10), each paying its own JIT compile for a
// handful of assertions. One program now carries all three use-cases as
// three self-contained static entry points — each builds its own table
// locally, so no scenario can perturb another — and one C++ test asserts the
// three score words scenario by scenario, with the original expectations
// unchanged. No rolling test expects a FAILING compile (the loud-failure
// claims are runtime `FrameException` catches inside the program), so no
// test had to stay separate.
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
    "import cajeta.time.Instant;\n"
    "import cajeta.time.Duration;\n"
    "import cajeta.nucleo.frame.Table;\n"
    "import cajeta.nucleo.frame.FrameException;\n"
    "import cajeta.nucleo.frame.Aggs;\n"
    "import cajeta.nucleo.column.Column;\n"
    "public record Tick {\n"
    "    Instant ts;\n"
    "    float64 px;\n"
    "}\n";

// Five rows, SCRAMBLED in time so the internal time-ordering shows. Sorted:
//   t=0s px1, t=30s px2, t=60s px3, t=90s px4, t=200s px5.
// Input order is t60, t0, t200, t30, t90.
const char* kBuild =
    "        int64[] tsv = heap int64[5];\n"
    "        tsv[0] = 60000000000; tsv[1] = 0;\n"
    "        tsv[2] = 200000000000; tsv[3] = 30000000000;\n"
    "        tsv[4] = 90000000000;\n"
    "        float64[] pv = heap float64[5];\n"
    "        pv[0] = 3.0; pv[1] = 1.0; pv[2] = 5.0; pv[3] = 2.0; pv[4] = 4.0;\n"
    "        Table<Tick> t = heap Table<Tick>(Column.of<int64>(tsv),\n"
    "            Column.of<float64>(pv));\n";

// The one rolling program. Each entry point is independent: it builds its own
// table from local arrays and returns its own score word, so the time form's
// scrambled input cannot leak into the count form's ordered input.
std::string rollingMatrixSrc() {
    return std::string(kPrelude) +
        "public final class D {\n"
        // 11.1.1 / 11.1.2 — the time-based sliding window (t-60s, t],
        // right-closed, establishes time order; sum/mean/count/last per row
        // match hand results.
        "    public static int32 timeWindow() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        "        Table<?> h = t.lazy().rolling((TickCols c) -> c.ts(),\n"
        "            Duration.ofSeconds(60)).agg((TickCols c, Aggs ag) -> {\n"
        "            ag.add(c.px().sum().alias(\"s\"));\n"
        "            ag.add(c.px().mean().alias(\"m\"));\n"
        "            ag.add(c.px().count().alias(\"n\"));\n"
        "            ag.add(c.px().last().alias(\"cl\"));\n"
        "        });\n"
        "        if (h.executions() == 0) { score = score + 1; }\n"  // built, not run
        "        Table<?> r = h.collect();\n"
        // One row per input row, in TIME order; the anchor time carried.
        "        if (r.rowCount() == 5 && r.width() == 5\n"
        "                && r.i64At(\"ts\", 0) == 0\n"
        "                && r.i64At(\"ts\", 4) == 200000000000) {\n"
        "            score = score + 2;\n"
        "        }\n"
        // Trailing (t-60s, t] windows, hand-computed:
        //   t=0:   {1}          s=1  n=1  cl=1
        //   t=30:  {1,2}        s=3  n=2  cl=2  (t0=0 > -30)
        //   t=60:  {2,3}        s=5  n=2  cl=3  (t0=0 NOT > 0)
        //   t=90:  {3,4}        s=7  n=2  cl=4  (t30=30 NOT > 30)
        //   t=200: {5}          s=5  n=1  cl=5
        "        if (r.f64At(\"s\", 0) == 1.0 && r.i64At(\"n\", 0) == 1\n"
        "                && r.f64At(\"s\", 1) == 3.0 && r.i64At(\"n\", 1) == 2\n"
        "                && r.f64At(\"s\", 2) == 5.0 && r.i64At(\"n\", 2) == 2\n"
        "                && r.f64At(\"s\", 3) == 7.0 && r.i64At(\"n\", 3) == 2\n"
        "                && r.f64At(\"s\", 4) == 5.0 && r.i64At(\"n\", 4) == 1) {\n"
        "            score = score + 4;\n"
        "        }\n"
        "        if (r.f64At(\"m\", 1) == 1.5 && r.f64At(\"m\", 2) == 2.5\n"
        "                && r.f64At(\"m\", 3) == 3.5) {\n"
        "            score = score + 8;\n"
        "        }\n"
        // last() is by TIME: t=90's window {t60 px3, t90 px4} -> latest is 4.
        "        if (r.f64At(\"cl\", 0) == 1.0 && r.f64At(\"cl\", 2) == 3.0\n"
        "                && r.f64At(\"cl\", 3) == 4.0 && r.f64At(\"cl\", 4) == 5.0) {\n"
        "            score = score + 16;\n"
        "        }\n"
        "        return score;\n"
        "    }\n"
        // 11.1.1 — the row-count window `rolling(n)`: the trailing n rows in
        // INPUT order, clamped at the start; sum/mean/count/min/max per row.
        "    public static int32 countWindow() {\n"
        // px = 1..5 in input order (no scramble — count form keeps input
        // order, no time column involved).
        "        int64[] tsv = heap int64[5];\n"
        "        tsv[0] = 0; tsv[1] = 1; tsv[2] = 2; tsv[3] = 3; tsv[4] = 4;\n"
        "        float64[] pv = heap float64[5];\n"
        "        pv[0] = 1.0; pv[1] = 2.0; pv[2] = 3.0; pv[3] = 4.0; pv[4] = 5.0;\n"
        "        Table<Tick> t = heap Table<Tick>(Column.of<int64>(tsv),\n"
        "            Column.of<float64>(pv));\n"
        "        int32 score = 0;\n"
        "        Table<?> r = t.lazy().rolling(3).agg((TickCols c, Aggs ag) -> {\n"
        "            ag.add(c.px().sum().alias(\"s\"));\n"
        "            ag.add(c.px().mean().alias(\"m\"));\n"
        "            ag.add(c.px().count().alias(\"n\"));\n"
        "            ag.add(c.px().min().alias(\"lo\"));\n"
        "            ag.add(c.px().max().alias(\"hi\"));\n"
        "        }).collect();\n"
        // The count form emits the aggregates alone, one row per input row.
        "        if (r.rowCount() == 5 && r.width() == 5) { score = score + 1; }\n"
        // Trailing 3-row windows, clamped at the start:
        //   i0 {1}       s=1  n=1
        //   i1 {1,2}     s=3  n=2
        //   i2 {1,2,3}   s=6  n=3  m=2  lo=1 hi=3
        //   i3 {2,3,4}   s=9  n=3  m=3  lo=2 hi=4
        //   i4 {3,4,5}   s=12 n=3  m=4  lo=3 hi=5
        "        if (r.f64At(\"s\", 0) == 1.0 && r.i64At(\"n\", 0) == 1\n"
        "                && r.f64At(\"s\", 1) == 3.0 && r.i64At(\"n\", 1) == 2\n"
        "                && r.f64At(\"s\", 2) == 6.0 && r.i64At(\"n\", 2) == 3\n"
        "                && r.f64At(\"s\", 4) == 12.0) {\n"
        "            score = score + 2;\n"
        "        }\n"
        "        if (r.f64At(\"m\", 2) == 2.0 && r.f64At(\"m\", 3) == 3.0\n"
        "                && r.f64At(\"m\", 4) == 4.0) {\n"
        "            score = score + 4;\n"
        "        }\n"
        "        if (r.f64At(\"lo\", 3) == 2.0 && r.f64At(\"hi\", 3) == 4.0\n"
        "                && r.f64At(\"lo\", 4) == 3.0 && r.f64At(\"hi\", 4) == 5.0) {\n"
        "            score = score + 8;\n"
        "        }\n"
        "        return score;\n"
        "    }\n"
        // 11.1.2 — the dynamic time form, and an incomplete rolling (no agg)
        // fails loud, sharing the resample completion contract.
        "    public static int32 dynamicForm() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        // Dynamic string time form.
        "        Table<?> r = t.lazy().rolling(\"ts\", Duration.ofSeconds(60))\n"
        "            .agg((TickCols c, Aggs ag) -> {\n"
        "            ag.add(c.px().sum().alias(\"s\"));\n"
        "        }).collect();\n"
        "        if (r.rowCount() == 5 && r.f64At(\"s\", 1) == 3.0) {\n"
        "            score = score + 1;\n"
        "        }\n"
        // A rolling never completed by an agg cannot be forced.
        "        try {\n"
        "            Table<?> bad = t.lazy().rolling(\"ts\",\n"
        "                Duration.ofSeconds(60)).collect();\n"
        "        } catch (FrameException e) {\n"
        "            if (e.getMessage().contains(\"agg\")) { score = score + 2; }\n"
        "        }\n"
        // A non-integer time column fails loud.
        "        try {\n"
        "            Table<?> bad2 = t.lazy().rolling(\"px\",\n"
        "                Duration.ofSeconds(60)).agg((TickCols c, Aggs ag) -> {\n"
        "                ag.add(c.px().sum().alias(\"s\"));\n"
        "            }).collect();\n"
        "        } catch (FrameException e) {\n"
        "            if (e.getMessage().contains(\"px\")\n"
        "                    && e.getMessage().contains(\"time\")) {\n"
        "                score = score + 4;\n"
        "            }\n"
        "        }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
}

} // namespace

// The rolling use-case matrix, one compiled program (was: RollingTests.{time
// WindowSlidesRightClosedInTimeOrder, countWindowTrailsInInputOrder, dynamic
// FormAndIncompleteFailsLoud} — three compiles' worth of claims, every
// assertion preserved):
//   the TIME window slides right-closed and establishes time order (lazy
//   until collect, anchor time carried, sum/mean/count/last by time); the
//   COUNT window trails in input order clamped at the start (sum/mean/count/
//   min/max); the DYNAMIC string time form agrees with the typed one, an
//   un-agg'd rolling cannot be forced, and a non-integer time column fails
//   loud.
TEST(RollingTests, timeCountAndDynamicWindowFormsMatrix) {
    auto jit = CajetaJit::compile(rollingMatrixSrc(), "test.D");
    ASSERT_NE(jit, nullptr);
    auto timeWindow  = jit->lookup<int32_t (*)()>("timeWindow");
    auto countWindow = jit->lookup<int32_t (*)()>("countWindow");
    auto dynamicForm = jit->lookup<int32_t (*)()>("dynamicForm");
    ASSERT_NE(timeWindow, nullptr);
    ASSERT_NE(countWindow, nullptr);
    ASSERT_NE(dynamicForm, nullptr);

    // 11.1.1 / 11.1.2 — time window: 1 lazy-until-collect, 2 shape + anchor
    // time order, 4 sum/count per row, 8 mean, 16 last-by-time.
    EXPECT_EQ(timeWindow(), 31)
        << "time window score bits: 1=lazy 2=shape/order 4=sum+count 8=mean"
           " 16=last-by-time";

    // 11.1.1 — count window: 1 shape (aggregates alone), 2 sum/count with the
    // start clamp, 4 mean, 8 min/max.
    EXPECT_EQ(countWindow(), 15)
        << "count window score bits: 1=shape 2=sum+count 4=mean 8=min/max";

    // 11.1.2 — dynamic form: 1 string time form computes, 2 un-agg'd rolling
    // refuses to collect, 4 non-integer time column fails loud.
    EXPECT_EQ(dynamicForm(), 7)
        << "dynamic form score bits: 1=string-form 2=no-agg-fails"
           " 4=non-integer-time-fails";
}
