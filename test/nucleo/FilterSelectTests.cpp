//
// nucleo-frame U5 — filter + select/with + the DSL wired in (plan
// 5.1.1–5.1.4; spec §3.2, §4.1, §4.3). Relational ops BUILD plan nodes;
// collect() executes them:
//  - `filter((TickCols c) -> c.price() > 2.0)` — the lambda-param builder
//    (U1's decided DSL): single comparisons work expression-bodied;
//    COMPOSITION inside a lambda body uses block form with bound locals
//    and the Pred.and/or/negate statics — operator-typed comparison
//    results only resolve in operator-arg/assignment/return positions,
//    and `&`/`|` overrides don't yet resolve in lambda bodies (compiler
//    ledger; method-level operators stay pinned by FrameDslTests). A
//    type mismatch (`c.venue() > 0.0`) stays a COMPILE error;
//  - `select`/`with` — projection and computed columns (`.alias` named);
//    schema-changing results are a visible `Table<?>`, narrowed back by the
//    checked `.as<R>()` whose mismatch errors name the column, the actual
//    type, and the expected field;
//  - `col("...")` — the dynamic accessor on `Table<?>`: a valid name builds
//    a plan node, an invalid one fails at plan-build naming the schema, and
//    typed member access on `Table<?>` is a guided compile error.
//
// Handles are LINEAR: chaining an op off a lazy handle consumes it, and
// reusing a chained-away handle is a named FrameException (the data itself
// is immutable — materialized sources are never consumed).
//
// Fold shape (test-battery-restructure 2.3): the four passing single-claim
// tests each paid their own JIT compile of the same prelude + the same
// five-row table build, and per-test coverage measurement (2026-08-10)
// showed their compiler line footprints near-identical. They are now four
// static entry points of ONE program — each builds its OWN table from
// kBuild and returns its OWN score, so no scenario can disturb another —
// behind one C++ test that asserts each score separately, with the original
// score-bit expectations unchanged. The two tests whose compile must FAIL
// keep their own TESTs: a failing compile cannot share a program.
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
    "import cajeta.lang.Utf8;\n"
    "import cajeta.nucleo.frame.Table;\n"
    "import cajeta.nucleo.frame.FrameException;\n"
    "import cajeta.nucleo.frame.Pred;\n"
    "import cajeta.nucleo.frame.Sels;\n"
    "import cajeta.nucleo.column.Column;\n"
    "import cajeta.nucleo.column.StringColumn;\n"
    "public record Tick {\n"
    "    Instant ts;\n"
    "    float64 price;\n"
    "    float64 size;\n"
    "    Utf8 venue;\n"
    "}\n";

// Five rows. ts is epoch-NANOS; notional (price*size) = 15,50,105,180,275.
const char* kBuild =
    "        int64[] tsv = heap int64[5];\n"
    "        tsv[0] = 1000000000; tsv[1] = 2000000000; tsv[2] = 3000000000;\n"
    "        tsv[3] = 4000000000; tsv[4] = 5000000000;\n"
    "        float64[] pv = heap float64[5];\n"
    "        pv[0] = 1.5; pv[1] = 2.5; pv[2] = 3.5; pv[3] = 4.5; pv[4] = 5.5;\n"
    "        float64[] sv = heap float64[5];\n"
    "        sv[0] = 10.0; sv[1] = 20.0; sv[2] = 30.0; sv[3] = 40.0; sv[4] = 50.0;\n"
    "        String[] vv = heap String[5];\n"
    "        vv[0] = \"ARCA\"; vv[1] = \"NYSE\"; vv[2] = \"ARCA\";\n"
    "        vv[3] = \"ARCA\"; vv[4] = \"BATS\";\n"
    "        Table<Tick> t = heap Table<Tick>(\n"
    "            Column.of<int64>(tsv), Column.of<float64>(pv),\n"
    "            Column.of<float64>(sv), StringColumn.of(vv));\n";

// The U5 use-case matrix as one compilation unit. The result records the
// four scenarios narrow into are declared once here (PN was written twice,
// identically, by the select and dynamic-col tests); every entry method is
// the original test's `run()` body verbatim, renamed to say what it carries
// and rebuilding its own table so the entries stay independent.
std::string buildMatrixSrc() {
    return std::string(kPrelude) +
        "public record PN {\n"
        "    float64 price;\n"
        "    float64 notional;\n"
        "}\n"
        "public record TickPlus {\n"
        "    Instant ts;\n"
        "    float64 price;\n"
        "    float64 size;\n"
        "    Utf8 venue;\n"
        "    float64 notional;\n"
        "}\n"
        "public record VOnly {\n"
        "    Utf8 venue;\n"
        "}\n"
        "public record Missing {\n"
        "    float64 price;\n"
        "    float64 vwap;\n"
        "}\n"
        "public record WrongType {\n"
        "    float64 price;\n"
        "    float64 venue;\n"
        "}\n"
        "public record TooWide {\n"
        "    float64 price;\n"
        "    Utf8 venue;\n"
        "    float64 extra;\n"
        "}\n"
        "public final class D {\n"

        // 5.1.1 — filter excludes rows; `&`/`|`/`.not()` compose; numeric,
        // int64 (Instant epoch-nanos, exact — no float64 round-trip), and
        // utf8 predicates all match hand results; the source table is
        // unchanged; ops chain directly off a materialized table too.
        "    public static int32 filterCompose() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        "        Table<Tick> h1 = t.lazy().filter((TickCols c) -> {\n"
        "            Pred a = c.price() > 2.0;\n"
        "            Pred b = c.size() < 45.0;\n"
        "            return Pred.and(#a, #b);\n"
        "        });\n"
        "        if (h1.executions() == 0) { score = score + 1; }\n"  // built, not run
        "        Table<Tick> r1 = h1.collect();\n"
        "        if (r1.rowCount() == 3 && r1.price.get(0) == 2.5\n"
        "                && r1.price.get(2) == 4.5\n"
        "                && r1.venue.get(0).equals(\"NYSE\")) {\n"
        "            score = score + 2;\n"
        "        }\n"
        "        Table<Tick> h2 = t.lazy().filter((TickCols c) -> {\n"
        "            Pred lo = c.price() < 2.0;\n"
        "            Pred hi = c.price() > 5.0;\n"
        "            return Pred.or(#lo, #hi);\n"
        "        });\n"
        "        Table<Tick> r2 = h2.collect();\n"
        "        if (r2.rowCount() == 2 && r2.price.get(1) == 5.5) { score = score + 4; }\n"
        "        Table<Tick> h3 = t.lazy().filter((TickCols c) -> {\n"
        "            Pred inner = c.price() > 2.0;\n"
        "            return Pred.negate(#inner);\n"
        "        });\n"
        "        Table<Tick> r3 = h3.collect();\n"
        "        if (r3.rowCount() == 1 && r3.price.get(0) == 1.5) { score = score + 8; }\n"
        "        Table<Tick> h4 = t.lazy().filter(\n"
        "            (TickCols c) -> c.ts() >= 3000000000);\n"
        "        Table<Tick> r4 = h4.collect();\n"
        "        if (r4.rowCount() == 3 && r4.ts.get(0) == 3000000000) { score = score + 16; }\n"
        "        Table<Tick> h5 = t.lazy().filter(\n"
        "            (TickCols c) -> c.venue() == \"ARCA\");\n"
        "        Table<Tick> r5 = h5.collect();\n"
        "        if (r5.rowCount() == 3 && r5.price.get(2) == 4.5) { score = score + 32; }\n"
        "        Table<Tick> h6 = t.filter((TickCols c) -> c.price() >= 4.5);\n"  // off materialized
        "        Table<Tick> r6 = h6.collect();\n"
        "        if (r6.rowCount() == 2 && r6.size.get(0) == 40.0) { score = score + 64; }\n"
        "        if (t.rowCount() == 5 && t.price.get(0) == 1.5) { score = score + 128; }\n"  // source intact
        "        return score;\n"
        "    }\n"

        // 5.1.2 — select (projection) and with (computed column, `.alias`
        // named) produce correct columns; the schema-changing result is a
        // visible `Table<?>` narrowed back by the checked `.as<R>()`;
        // passthrough projections stay zero-copy (address identity through
        // the narrow).
        "    public static int32 selectWithNarrow() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        "        Table<?> s = t.lazy().select((TickCols c, Sels ss) -> {\n"
        "            ss.add(c.price());\n"
        "            ss.add((c.price() * c.size()).alias(\"notional\"));\n"
        "        });\n"
        "        if (s.executions() == 0) { score = score + 1; }\n"  // select builds, never runs
        "        Table<PN> ph = s.as<PN>();\n"
        "        Table<PN> p = ph.collect();\n"
        "        if (p.rowCount() == 5 && p.price.get(0) == 1.5\n"
        "                && p.notional.get(4) == 275.0) {\n"
        "            score = score + 2;\n"
        "        }\n"
        "        if (p.price.dataAddress() == t.price.dataAddress()) { score = score + 4; }\n"
        "        Table<?> w = t.with((TickCols c) -> (c.price() * c.size()).alias(\"notional\"));\n"
        "        Table<TickPlus> th = w.as<TickPlus>();\n"
        "        Table<TickPlus> tp = th.collect();\n"
        "        if (tp.rowCount() == 5 && tp.notional.get(1) == 50.0\n"
        "                && tp.venue.get(4).equals(\"BATS\")\n"
        "                && tp.ts.get(0) == 1000000000) {\n"
        "            score = score + 8;\n"
        "        }\n"
        "        Table<?> vsel = t.select((TickCols c, Sels ss) -> {\n"
        "            ss.add(c.venue());\n"     // utf8 passthrough keeps its physical
        "        });\n"
        "        Table<VOnly> vh = vsel.as<VOnly>();\n"
        "        Table<VOnly> v = vh.collect();\n"
        "        if (v.rowCount() == 5 && v.venue.get(1).equals(\"NYSE\")) {\n"
        "            score = score + 16;\n"
        "        }\n"
        "        return score;\n"
        "    }\n"

        // 5.1.2 (checked narrow) — `.as<R>()` mismatches are named
        // FrameExceptions: an absent column names itself and the expected
        // field type; a type mismatch names the column, the ACTUAL type, and
        // the expected one; a width mismatch names both column counts. A
        // failed narrow neither forces nor consumes the handle.
        "    public static int32 narrowErrors() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        "        Table<?> s = t.lazy().select((TickCols c, Sels ss) -> {\n"
        "            ss.add(c.price());\n"
        "            ss.add(c.venue());\n"
        "        });\n"
        "        try {\n"
        "            Table<Missing> x = s.as<Missing>();\n"
        "        } catch (FrameException e) {\n"
        "            String m = e.getMessage();\n"
        "            if (m.contains(\"vwap\") && m.contains(\"absent\")) { score = score + 1; }\n"
        "        }\n"
        "        try {\n"
        "            Table<WrongType> y = s.as<WrongType>();\n"
        "        } catch (FrameException e) {\n"
        "            String m = e.getMessage();\n"
        "            if (m.contains(\"venue\") && m.contains(\"utf8\")\n"
        "                    && m.contains(\"float64\")) {\n"
        "                score = score + 2;\n"
        "            }\n"
        "        }\n"
        "        try {\n"
        "            Table<TooWide> z = s.as<TooWide>();\n"
        "        } catch (FrameException e) {\n"
        "            String m = e.getMessage();\n"
        "            if (m.contains(\"declares 3\")) { score = score + 4; }\n"
        "        }\n"
        "        if (s.executions() == 0) { score = score + 8; }\n"   // no secret force
        "        Pred still = s.col(\"price\") > 0.0;\n"              // handle not consumed
        "        score = score + 16;\n"
        "        return score;\n"
        "    }\n"

        // 5.1.3 — the dynamic accessor on `Table<?>`: `col("...")` with a
        // valid name builds a plan node (a filter over the erased schema);
        // an invalid name fails at PLAN BUILD naming the schema; a utf8
        // column guides to `colStr`; string predicates work through
        // `colStr`; and a chained-away handle fails loud on reuse.
        "    public static int32 dynamicCol() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        "        Table<?> s = t.lazy().select((TickCols c, Sels ss) -> {\n"
        "            ss.add(c.price());\n"
        "            ss.add((c.price() * c.size()).alias(\"notional\"));\n"
        "        });\n"
        "        Pred p = s.col(\"notional\") > 100.0;\n"   // valid name: plans
        "        Table<?> f = s.filter(#p);\n"
        "        Table<PN> nh = f.as<PN>();\n"
        "        Table<PN> n = nh.collect();\n"
        "        if (n.rowCount() == 3 && n.notional.get(0) == 105.0\n"
        "                && n.notional.get(2) == 275.0) {\n"
        "            score = score + 1;\n"
        "        }\n"
        "        Table<?> s2 = t.lazy().select((TickCols c, Sels ss) -> {\n"
        "            ss.add(c.price());\n"
        "        });\n"
        "        try {\n"
        "            s2.col(\"nope\");\n"
        "        } catch (FrameException e) {\n"
        "            String m = e.getMessage();\n"
        "            if (m.contains(\"nope\") && m.contains(\"price\")) { score = score + 2; }\n"
        "        }\n"
        "        try {\n"
        "            t.col(\"venue\");\n"
        "        } catch (FrameException e) {\n"
        "            if (e.getMessage().contains(\"colStr\")) { score = score + 4; }\n"
        "        }\n"
        "        Pred sp = t.colStr(\"venue\") == \"ARCA\";\n"
        "        Table<Tick> tf = t.filter(#sp);\n"
        "        Table<Tick> tr = tf.collect();\n"
        "        if (tr.rowCount() == 3 && tr.price.get(0) == 1.5) { score = score + 8; }\n"
        "        Table<?> g = t.lazy();\n"
        "        Pred gp = g.col(\"price\") > 0.0;\n"
        "        Table<?> g1 = g.filter(#gp);\n"            // consumes g
        "        try {\n"
        "            Pred gp2 = g.col(\"price\") > 1.0;\n"
        "            Table<?> g2 = g.filter(#gp2);\n"        // chained-away handle
        "        } catch (FrameException e) {\n"
        "            if (e.getMessage().contains(\"chained\")) { score = score + 16; }\n"
        "        }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
}

} // namespace

// The U5 relational use-case matrix, one compiled program (was:
// FilterSelectTests.{filterExcludesRowsAndComposes,
// selectWithComputeAndCheckedNarrow, narrowMismatchErrorsNameEverything,
// dynamicColPlansAndFailsLoud} — four compiles' worth of claims, every
// score bit preserved):
//   5.1.1 filter excludes and composes (and/or/negate, int64, utf8, off a
//   materialized table, source intact); 5.1.2 select/with compute the right
//   columns and the checked `.as<R>()` narrows zero-copy; 5.1.2 narrow
//   mismatches are named FrameExceptions that neither force nor consume;
//   5.1.3 `col("...")` plans on a valid name and fails loud otherwise
//   (unknown name, utf8 guided to `colStr`, chained-away handle).
TEST(FilterSelectTests, filterSelectNarrowAndDynamicColMatrix) {
    auto src = buildMatrixSrc();
    auto jit = CajetaJit::compile(src, "test.D");
    ASSERT_NE(jit, nullptr);
    auto filterCompose    = jit->lookup<int32_t (*)()>("filterCompose");
    auto selectWithNarrow = jit->lookup<int32_t (*)()>("selectWithNarrow");
    auto narrowErrors     = jit->lookup<int32_t (*)()>("narrowErrors");
    auto dynamicCol       = jit->lookup<int32_t (*)()>("dynamicCol");
    ASSERT_NE(filterCompose, nullptr);
    ASSERT_NE(selectWithNarrow, nullptr);
    ASSERT_NE(narrowErrors, nullptr);
    ASSERT_NE(dynamicCol, nullptr);

    // 5.1.1 — score bits: 1 built-not-run, 2 and-composition rows, 4 or,
    // 8 negate, 16 int64/Instant, 32 utf8, 64 off a materialized table,
    // 128 source unchanged.
    EXPECT_EQ(filterCompose(), 255) << "5.1.1 filter/compose";

    // 5.1.2 — score bits: 1 select builds but never runs, 2 projected
    // values, 4 passthrough zero-copy, 8 `with` computed column, 16 utf8
    // passthrough.
    EXPECT_EQ(selectWithNarrow(), 31) << "5.1.2 select/with + checked narrow";

    // 5.1.2 (checked narrow) — score bits: 1 absent column, 2 type
    // mismatch, 4 width mismatch, 8 no secret force, 16 handle survives a
    // failed narrow.
    EXPECT_EQ(narrowErrors(), 31) << "5.1.2 narrow mismatch messages";

    // 5.1.3 — score bits: 1 valid name plans and computes, 2 unknown name
    // names the schema, 4 utf8 guided to `colStr`, 8 `colStr` predicate,
    // 16 chained-away handle fails loud.
    EXPECT_EQ(dynamicCol(), 31) << "5.1.3 dynamic col plans and fails loud";
}

// 5.1.3 — typed member access on `Table<?>` is unavailable, and the compile
// error says exactly how to proceed: narrow with `.as<R>()` or use
// `col("...")` (spec §4.3.2's wording contract). Its own test because the
// compile FAILS.
TEST(FilterSelectTests, typedMemberOnErasedIsGuidedCompileError) {
    auto src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kBuild +
        "        Table<?> s = t.lazy().select((TickCols c, Sels ss) -> {\n"
        "            ss.add(c.price());\n"
        "        });\n"
        "        float64 x = s.price.get(0);\n"
        "        return (int32) x;\n"
        "    }\n"
        "}\n";
    bool threw = false;
    try {
        CajetaJit::compile(src, "test.D");
    } catch (cajeta::Exception& e) {
        threw = true;
        EXPECT_NE(std::string(e.getMessage()).find("narrow"), std::string::npos)
            << "must guide to .as<R>(): " << e.getMessage();
        EXPECT_NE(std::string(e.getMessage()).find("col("), std::string::npos)
            << "must guide to col(\"...\"): " << e.getMessage();
    }
    EXPECT_TRUE(threw);
}

// 5.1.4 — a type mismatch in the DSL inside a real relational op
// (`c.venue() > 0.0` — utf8 vs float) fails at COMPILE time under the U1
// operator mechanism: there is no such operator, and the error is located,
// never a runtime surprise. Its own test because the compile FAILS.
TEST(FilterSelectTests, dslTypeMismatchIsCompileError) {
    auto src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kBuild +
        "        Table<Tick> h = t.lazy().filter((TickCols c) -> c.venue() > 0.0);\n"
        "        return (int32) h.collect().rowCount();\n"
        "    }\n"
        "}\n";
    bool threw = false;
    try {
        CajetaJit::compile(src, "test.D");
    } catch (cajeta::Exception& e) {
        threw = true;
        EXPECT_FALSE(std::string(e.getErrorId()).empty());
    }
    EXPECT_TRUE(threw);
}
