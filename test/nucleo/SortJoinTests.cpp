//
// nucleo-frame U9 — sort + join (plan 9.1.1-9.3.1; spec §4.1, §7). Both are
// LAZY: they build plan nodes and nothing runs until a terminal.
//  - `sort` is schema-PRESERVING, so the handle stays typed: ascending by
//    default, `.desc()` per key, STABLE (ties keep input order), multi-key,
//    and nulls LAST by default with `Sorts.NULLS_FIRST` as the override
//    (spec §7). A computed sort key fails loud at plan build.
//  - `join` is schema-CHANGING, so the result is a `Table<?>` narrowed by
//    the checked `.as<R>()`. All four `how` kinds over typed keys match
//    hand results; a NULL key matches NOTHING (spec §7) — on either side.
//  - Key errors: a typed key that is not a column is a COMPILE error (the
//    builder has no such method), the dynamic form fails at plan build, and
//    so does a key absent from the RIGHT schema.
//  - The null-extended side of an outer join needs nullable columns, and
//    `@Nullable Utf8` is unsupported (the column package's limit) — so an
//    outer join that would null-extend a utf8 column fails LOUD at plan
//    build naming the limitation, rather than inventing an empty string.
//  - The U7 rewriter sinks a filter below a sort (a stable sort of the
//    survivors is the survivors of the sorted rows), and the optimized and
//    unoptimized runs agree.
//
// Fold shape (test-battery-restructure 2.3, spec §3.2b/§3.3): the four
// original tests compiled four separate programs off the SAME prelude, the
// same two record families, and the same builders — per-test coverage
// measurement (2026-08-10) put their compiler line footprints within noise of
// each other (~18.8k shared lines), so each compile bought assertions, not
// coverage. One program now carries all four scenarios as four independent
// static entry points — each builds its own tables locally and returns its own
// score, so no scenario can perturb another — and one C++ test asserts each
// score with its bit legend. Nothing here needs a FAILING compile (every
// negative case is a caught `FrameException` at plan-build time, inside the
// program), so no test had to stay behind.
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
    "import cajeta.nucleo.frame.Join;\n"
    "import cajeta.nucleo.frame.Pred;\n"
    "import cajeta.nucleo.frame.Sels;\n"
    "import cajeta.nucleo.frame.Sorts;\n"
    "import cajeta.nucleo.frame.SortKey;\n"
    "import cajeta.nucleo.column.Column;\n"
    "import cajeta.nucleo.column.NullableColumn;\n"
    "import cajeta.nucleo.column.StringColumn;\n";

// Five rows with deliberate TIES (px 1.0 twice, sym "a" three times) so the
// stability of the sort is observable, and a nullable column with two nulls
// so the null ORDER is observable.
//   id  1     2      3     4      5
//   px  3.0   1.0    2.0   1.0    5.0
//   sym "b"   "a"    "c"   "a"    "a"
//   bid 2.0   null   1.0   null   4.0
const char* kPxRecord =
    "public record Px {\n"
    "    int64 id;\n"
    "    float64 px;\n"
    "    Utf8 sym;\n"
    "    @Nullable float64 bid;\n"
    "}\n";

const char* kPxBuild =
    "        int64[] iv = heap int64[5];\n"
    "        iv[0] = 1; iv[1] = 2; iv[2] = 3; iv[3] = 4; iv[4] = 5;\n"
    "        float64[] pv = heap float64[5];\n"
    "        pv[0] = 3.0; pv[1] = 1.0; pv[2] = 2.0; pv[3] = 1.0; pv[4] = 5.0;\n"
    "        String[] sv = heap String[5];\n"
    "        sv[0] = \"b\"; sv[1] = \"a\"; sv[2] = \"c\"; sv[3] = \"a\";\n"
    "        sv[4] = \"a\";\n"
    "        float64[] bv = heap float64[5];\n"
    "        bv[0] = 2.0; bv[1] = 99.0; bv[2] = 1.0; bv[3] = 99.0;\n"
    "        bv[4] = 4.0;\n"
    "        boolean[] bok = heap boolean[5];\n"
    "        bok[0] = true; bok[1] = false; bok[2] = true; bok[3] = false;\n"
    "        bok[4] = true;\n"
    "        Table<Px> t = heap Table<Px>(Column.of<int64>(iv),\n"
    "            Column.of<float64>(pv), StringColumn.of(sv),\n"
    "            NullableColumn.of<float64>(bv, bok));\n";

// The join pair. No utf8 outside the key, so every `how` is representable.
//   Ord   acct 1  2  1  3      qty 10 20 30 40
//   Acct  acct 1  2  5         fee 0.5 0.25 0.75   rebate 1.0 null 3.0
// acct 3 is left-only; acct 5 is right-only.
const char* kJoinRecords =
    "public record Ord { int64 acct; float64 qty; }\n"
    "public record Acct {\n"
    "    int64 acct;\n"
    "    float64 fee;\n"
    "    @Nullable float64 rebate;\n"
    "}\n";

const char* kJoinBuild =
    "        int64[] oa = heap int64[4];\n"
    "        oa[0] = 1; oa[1] = 2; oa[2] = 1; oa[3] = 3;\n"
    "        float64[] oq = heap float64[4];\n"
    "        oq[0] = 10.0; oq[1] = 20.0; oq[2] = 30.0; oq[3] = 40.0;\n"
    "        Table<Ord> o = heap Table<Ord>(Column.of<int64>(oa),\n"
    "            Column.of<float64>(oq));\n"
    "        int64[] aa = heap int64[3];\n"
    "        aa[0] = 1; aa[1] = 2; aa[2] = 5;\n"
    "        float64[] af = heap float64[3];\n"
    "        af[0] = 0.5; af[1] = 0.25; af[2] = 0.75;\n"
    "        float64[] ar = heap float64[3];\n"
    "        ar[0] = 1.0; ar[1] = 99.0; ar[2] = 3.0;\n"
    "        boolean[] aok = heap boolean[3];\n"
    "        aok[0] = true; aok[1] = false; aok[2] = true;\n"
    "        Table<Acct> a = heap Table<Acct>(Column.of<int64>(aa),\n"
    "            Column.of<float64>(af),\n"
    "            NullableColumn.of<float64>(ar, aok));\n";

// The narrowing target of the join family: the checked `.as<R>()` shape for an
// INNER join of Ord x Acct (key once, left columns then the right's non-key
// columns).
const char* kFilledRecord =
    "public record Filled {\n"
    "    int64 acct;\n"
    "    float64 qty;\n"
    "    float64 fee;\n"
    "    @Nullable float64 rebate;\n"
    "}\n";

// The nullable-key pair (a null key on each side) plus the utf8-carrying right
// table used by the outer-join utf8 limit.
const char* kNullKeyRecords =
    "public record NOrd { @Nullable int64 acct; float64 qty; }\n"
    "public record NRef { @Nullable int64 acct; float64 fee; }\n"
    "public record Named { int64 acct; Utf8 tag; }\n";

// 9.1.1 — ascending/descending, STABLE, nulls last by default with the
// override, multi-key, and utf8 keys. Sort preserves the schema, so the
// result stays a typed `Table<Px>`.
std::string sortOrderEntry() {
    return std::string(
        "    public static int32 sortOrderScore() {\n")
        + kPxBuild +
        "        int32 score = 0;\n"
        // Ascending, with a tie: ids 2 and 4 both hold px 1.0 and must come
        // out in INPUT order — that is what stable means.
        "        Table<Px> h1 = t.lazy().sort((PxCols c, Sorts s) -> {\n"
        "            s.add(c.px());\n"
        "        });\n"
        "        if (h1.executions() == 0) { score = score + 1; }\n"
        "        Table<Px> r1 = h1.collect();\n"
        "        if (r1.rowCount() == 5 && r1.id.get(0) == 2\n"
        "                && r1.id.get(1) == 4 && r1.id.get(2) == 3\n"
        "                && r1.id.get(3) == 1 && r1.id.get(4) == 5) {\n"
        "            score = score + 2;\n"
        "        }\n"
        // Descending — the tie STILL keeps input order (2 before 4).
        "        Table<Px> h2 = t.lazy().sort((PxCols c, Sorts s) -> {\n"
        "            s.add(c.px().desc());\n"
        "        });\n"
        "        Table<Px> r2 = h2.collect();\n"
        "        if (r2.id.get(0) == 5 && r2.id.get(1) == 1\n"
        "                && r2.id.get(2) == 3 && r2.id.get(3) == 2\n"
        "                && r2.id.get(4) == 4) {\n"
        "            score = score + 4;\n"
        "        }\n"
        // Nulls LAST by default, whichever direction.
        "        Table<Px> h3 = t.lazy().sort((PxCols c, Sorts s) -> {\n"
        "            s.add(c.bid());\n"
        "        });\n"
        "        Table<Px> r3 = h3.collect();\n"
        "        if (r3.id.get(0) == 3 && r3.id.get(1) == 1\n"
        "                && r3.id.get(2) == 5 && r3.id.get(3) == 2\n"
        "                && r3.id.get(4) == 4) {\n"
        "            score = score + 8;\n"
        "        }\n"
        "        Table<Px> h4 = t.lazy().sort((PxCols c, Sorts s) -> {\n"
        "            s.add(c.bid().desc());\n"
        "        });\n"
        "        Table<Px> r4 = h4.collect();\n"
        "        if (r4.id.get(0) == 5 && r4.id.get(1) == 1\n"
        "                && r4.id.get(2) == 3 && r4.id.get(3) == 2\n"
        "                && r4.id.get(4) == 4) {\n"
        "            score = score + 16;\n"
        "        }\n"
        // The override: nulls FIRST.
        "        Table<Px> h5 = t.lazy().sort((PxCols c, Sorts s) -> {\n"
        "            s.add(c.bid(), Sorts.NULLS_FIRST);\n"
        "        });\n"
        "        Table<Px> r5 = h5.collect();\n"
        "        if (r5.id.get(0) == 2 && r5.id.get(1) == 4\n"
        "                && r5.id.get(2) == 3 && r5.id.get(3) == 1\n"
        "                && r5.id.get(4) == 5) {\n"
        "            score = score + 32;\n"
        "        }\n"
        // Multi-key: sym ascending (utf8), then px DESCENDING within it.
        // sym "a" holds ids 2 (1.0), 4 (1.0), 5 (5.0) -> 5, 2, 4.
        "        Table<Px> h6 = t.lazy().sort((PxCols c, Sorts s) -> {\n"
        "            s.add(c.sym());\n"
        "            s.add(c.px().desc());\n"
        "        });\n"
        "        Table<Px> r6 = h6.collect();\n"
        "        if (r6.id.get(0) == 5 && r6.id.get(1) == 2\n"
        "                && r6.id.get(2) == 4 && r6.id.get(3) == 1\n"
        "                && r6.id.get(4) == 3\n"
        "                && r6.sym.get(4).equals(\"c\")) {\n"
        "            score = score + 64;\n"
        "        }\n"
        "        return score;\n"
        "    }\n";
}

// 9.1.2 — all four `how` kinds over a typed key, against hand results. The
// result is schema-erased and narrows back with the checked `.as<R>()`.
std::string joinFamilyEntry() {
    return std::string(
        "    public static int32 joinFamilyScore() {\n")
        + kJoinBuild +
        "        int32 score = 0;\n"
        // INNER: left rows 0,1,2 match; left row 3 (acct 3) drops. Output
        // is left-driven: matched pairs in left order.
        "        Table<Ord> oh = o.lazy();\n"
        "        Table<Acct> ah = a.lazy();\n"
        "        Table<?> j1 #= oh.join(ah, (OrdCols c, Sels s) -> {\n"
        "            s.add(c.acct());\n"
        "        }, Join.INNER);\n"
        "        if (j1.executions() == 0) { score = score + 1; }\n"
        // The key appears ONCE; the shape is left columns then the right's
        // non-key columns.
        "        Table<Filled> n1 = j1.as<Filled>();\n"
        "        Table<Filled> r1 = n1.collect();\n"
        "        if (r1.rowCount() == 3 && r1.acct.get(0) == 1\n"
        "                && r1.qty.get(0) == 10.0 && r1.fee.get(0) == 0.5\n"
        "                && r1.qty.get(1) == 20.0 && r1.fee.get(1) == 0.25\n"
        "                && r1.qty.get(2) == 30.0 && r1.fee.get(2) == 0.5) {\n"
        "            score = score + 2;\n"
        "        }\n"
        // A null on the right side rides through: acct 2's rebate is null.
        "        if (r1.rebate.isValid(0) && r1.rebate.get(0) == 1.0\n"
        "                && !r1.rebate.isValid(1)\n"
        "                && r1.rebate.isValid(2)) {\n"
        "            score = score + 4;\n"
        "        }\n"
        // LEFT: every left row survives; acct 3 gets nulls on the right.
        "        Table<Ord> oh2 = o.lazy();\n"
        "        Table<Acct> ah2 = a.lazy();\n"
        "        Table<?> j2 #= oh2.join(ah2, (OrdCols c, Sels s) -> {\n"
        "            s.add(c.acct());\n"
        "        }, Join.LEFT);\n"
        "        Table<?> r2 = j2.collect();\n"
        "        if (r2.rowCount() == 4 && r2.width() == 4\n"
        "                && r2.i64At(\"acct\", 3) == 3\n"
        "                && r2.f64At(\"qty\", 3) == 40.0\n"
        "                && !r2.validAt(\"fee\", 3)\n"
        "                && !r2.validAt(\"rebate\", 3)\n"
        "                && r2.validAt(\"fee\", 0)) {\n"
        "            score = score + 8;\n"
        "        }\n"
        // RIGHT: every right row survives; acct 5 has no left row, so its
        // qty is null and its KEY comes from the right side.
        "        Table<Ord> oh3 = o.lazy();\n"
        "        Table<Acct> ah3 = a.lazy();\n"
        "        Table<?> j3 #= oh3.join(ah3, (OrdCols c, Sels s) -> {\n"
        "            s.add(c.acct());\n"
        "        }, Join.RIGHT);\n"
        "        Table<?> r3 = j3.collect();\n"
        "        if (r3.rowCount() == 4\n"
        "                && r3.i64At(\"acct\", 3) == 5\n"
        "                && !r3.validAt(\"qty\", 3)\n"
        "                && r3.f64At(\"fee\", 3) == 0.75\n"
        "                && r3.f64At(\"qty\", 0) == 10.0) {\n"
        "            score = score + 16;\n"
        "        }\n"
        // FULL: matched pairs, then the left-only row, then the right-only
        // row — 3 + 1 + 1.
        "        Table<Ord> oh4 = o.lazy();\n"
        "        Table<Acct> ah4 = a.lazy();\n"
        "        Table<?> j4 #= oh4.join(ah4, (OrdCols c, Sels s) -> {\n"
        "            s.add(c.acct());\n"
        "        }, Join.FULL);\n"
        "        Table<?> r4 = j4.collect();\n"
        "        if (r4.rowCount() == 5\n"
        "                && r4.i64At(\"acct\", 3) == 3\n"
        "                && !r4.validAt(\"fee\", 3)\n"
        "                && r4.i64At(\"acct\", 4) == 5\n"
        "                && !r4.validAt(\"qty\", 4)\n"
        "                && r4.f64At(\"fee\", 4) == 0.75) {\n"
        "            score = score + 32;\n"
        "        }\n"
        // An INNER join's key is NON-null even when a source key is
        // nullable (a null key matches nothing), so `Filled.acct` — a
        // plain int64 — narrows. The LEFT join's `fee` went nullable, so
        // narrowing THAT to `Filled` must fail.
        "        Table<Ord> oh5 = o.lazy();\n"
        "        Table<Acct> ah5 = a.lazy();\n"
        "        Table<?> j5 #= oh5.join(ah5, (OrdCols c, Sels s) -> {\n"
        "            s.add(c.acct());\n"
        "        }, Join.LEFT);\n"
        "        try {\n"
        "            Table<Filled> bad = j5.as<Filled>();\n"
        "        } catch (FrameException e) {\n"
        "            if (e.getMessage().contains(\"fee\")) { score = score + 64; }\n"
        "        }\n"
        "        return score;\n"
        "    }\n";
}

// 9.1.2 / 9.1.3 — a NULL key matches nothing on either side, and bad keys
// fail loud: the dynamic form at plan build, and the outer-join utf8
// limitation with a message that names the fix.
std::string nullKeyEntry() {
    return std::string(
        "    public static int32 nullKeyScore() {\n"
        "        int64[] la = heap int64[2];\n"
        "        la[0] = 1; la[1] = 7;\n"
        "        boolean[] lok = heap boolean[2];\n"
        "        lok[0] = true; lok[1] = false;\n"
        "        float64[] lq = heap float64[2];\n"
        "        lq[0] = 10.0; lq[1] = 20.0;\n"
        "        Table<NOrd> l = heap Table<NOrd>(\n"
        "            NullableColumn.of<int64>(la, lok),\n"
        "            Column.of<float64>(lq));\n"
        "        int64[] ra = heap int64[2];\n"
        "        ra[0] = 1; ra[1] = 7;\n"
        "        boolean[] rok = heap boolean[2];\n"
        "        rok[0] = true; rok[1] = false;\n"
        "        float64[] rf = heap float64[2];\n"
        "        rf[0] = 0.5; rf[1] = 0.25;\n"
        "        Table<NRef> r = heap Table<NRef>(\n"
        "            NullableColumn.of<int64>(ra, rok),\n"
        "            Column.of<float64>(rf));\n"
        "        int32 score = 0;\n"
        // INNER: the null key on each side matches NOTHING — not each
        // other, and not the non-null key. One row out.
        "        Table<NOrd> lh #= l.lazy();\n"
        "        Table<NRef> rh #= r.lazy();\n"
        "        Table<?> j1 #= lh.join(rh, (NOrdCols c, Sels s) -> {\n"
        "            s.add(c.acct());\n"
        "        }, Join.INNER);\n"
        "        Table<?> c1 = j1.collect();\n"
        "        if (c1.rowCount() == 1 && c1.i64At(\"acct\", 0) == 1\n"
        "                && c1.f64At(\"qty\", 0) == 10.0\n"
        "                && c1.f64At(\"fee\", 0) == 0.5) {\n"
        "            score = score + 1;\n"
        "        }\n"
        // FULL: the matched row, the unmatched left null-key row, and the
        // unmatched right null-key row — three, all distinct.
        "        Table<NOrd> lh2 #= l.lazy();\n"
        "        Table<NRef> rh2 #= r.lazy();\n"
        "        Table<?> j2 #= lh2.join(rh2, (NOrdCols c, Sels s) -> {\n"
        "            s.add(c.acct());\n"
        "        }, Join.FULL);\n"
        "        Table<?> c2 = j2.collect();\n"
        "        if (c2.rowCount() == 3 && !c2.validAt(\"acct\", 1)\n"
        "                && c2.f64At(\"qty\", 1) == 20.0\n"
        "                && !c2.validAt(\"acct\", 2)\n"
        "                && c2.f64At(\"fee\", 2) == 0.25) {\n"
        "            score = score + 2;\n"
        "        }\n"
        // The dynamic form: an unknown key names the schema it searched.
        "        Table<NOrd> lh3 #= l.lazy();\n"
        "        Table<NRef> rh3 #= r.lazy();\n"
        "        try {\n"
        "            Table<?> bad #= lh3.join(rh3, \"nope\", Join.INNER);\n"
        "        } catch (FrameException e) {\n"
        "            if (e.getMessage().contains(\"nope\")) { score = score + 4; }\n"
        "        }\n"
        // A key the LEFT has and the RIGHT does not: not statically
        // knowable (the right is erased), so it is a plan-build error.
        "        Table<NOrd> lh4 #= l.lazy();\n"
        "        Table<NRef> rh4 #= r.lazy();\n"
        "        try {\n"
        "            Table<?> bad2 #= lh4.join(rh4, (NOrdCols c, Sels s) -> {\n"
        "                s.add(c.qty());\n"
        "            }, Join.INNER);\n"
        "        } catch (FrameException e) {\n"
        "            if (e.getMessage().contains(\"qty\")) { score = score + 8; }\n"
        "        }\n"
        // The outer-join utf8 limit: null-extending a utf8 column is not
        // representable (no @Nullable Utf8), so it fails at plan build
        // rather than inventing an empty string. The INNER join of the
        // same pair is fine — nothing gets null-extended there.
        "        int64[] na = heap int64[1];\n"
        "        na[0] = 1;\n"
        "        String[] nt = heap String[1];\n"
        "        nt[0] = \"x\";\n"
        "        Table<Named> nm = heap Table<Named>(Column.of<int64>(na),\n"
        "            StringColumn.of(nt));\n"
        "        Table<NOrd> lh5 #= l.lazy();\n"
        "        Table<Named> nh5 #= nm.lazy();\n"
        "        try {\n"
        "            Table<?> bad3 #= lh5.join(nh5, (NOrdCols c, Sels s) -> {\n"
        "                s.add(c.acct());\n"
        "            }, Join.LEFT);\n"
        "        } catch (FrameException e) {\n"
        "            if (e.getMessage().contains(\"tag\")\n"
        "                    && e.getMessage().contains(\"utf8\")) {\n"
        "                score = score + 16;\n"
        "            }\n"
        "        }\n"
        "        Table<NOrd> lh6 #= l.lazy();\n"
        "        Table<Named> nh6 #= nm.lazy();\n"
        "        Table<?> ok #= lh6.join(nh6, (NOrdCols c, Sels s) -> {\n"
        "            s.add(c.acct());\n"
        "        }, Join.INNER);\n"
        "        Table<?> okc = ok.collect();\n"
        "        if (okc.rowCount() == 1 && okc.strAt(\"tag\", 0).equals(\"x\")) {\n"
        "            score = score + 32;\n"
        "        }\n"
        "        return score;\n"
        "    }\n");
}

// 9.2.1 — the U7 rewriter over the new nodes: a filter sinks below a sort
// (both orders give the same rows, and the scan never materializes the
// failing ones), while a join is a barrier the rewriter does not cross.
// Optimized and unoptimized runs agree, which is the pushdown contract.
std::string pushdownEntry() {
    return std::string(
        "    public static int32 pushdownScore() {\n")
        + kPxBuild + kJoinBuild +
        "        int32 score = 0;\n"
        // sort <- filter: the predicate sinks to the scan, so only the
        // three passing rows are ever materialized.
        "        Table<Px> h = t.lazy().sort((PxCols c, Sorts s) -> {\n"
        "            s.add(c.px().desc());\n"
        "        }).filter((PxCols c) -> c.px() > 1.5);\n"
        "        if (h.explain().contains(\"scan[filter]\")) {\n"
        "            score = score + 1;\n"
        "        }\n"
        "        Table<Px> r = h.collect();\n"
        "        if (r.rowCount() == 3 && r.id.get(0) == 5\n"
        "                && r.id.get(1) == 1 && r.id.get(2) == 3) {\n"
        "            score = score + 2;\n"
        "        }\n"
        "        if (h.scanRows() == 3 && t.rowCount() == 5) {\n"
        "            score = score + 4;\n"
        "        }\n"
        // The same chain unoptimized must give the identical answer.
        "        Table<Px> hu = t.lazy().sort((PxCols c, Sorts s) -> {\n"
        "            s.add(c.px().desc());\n"
        "        }).filter((PxCols c) -> c.px() > 1.5);\n"
        "        hu.optimize(false);\n"
        "        Table<Px> ru = hu.collect();\n"
        "        if (ru.rowCount() == 3 && ru.id.get(0) == 5\n"
        "                && ru.id.get(1) == 1 && ru.id.get(2) == 3\n"
        "                && hu.scanRows() == 5) {\n"
        "            score = score + 8;\n"
        "        }\n"
        // A join is a barrier: the filter above it stays above it, and the
        // scan reads every column and row.
        "        Table<Ord> oh = o.lazy();\n"
        "        Table<Acct> ah = a.lazy();\n"
        "        Table<?> j #= oh.join(ah, (OrdCols c, Sels s) -> {\n"
        "            s.add(c.acct());\n"
        "        }, Join.INNER);\n"
        "        Pred jp #= j.col(\"qty\") > 15.0;\n"
        "        Table<?> jf #= j.filter(#jp);\n"
        "        String ex #= jf.explain();\n"
        "        if (ex.contains(\"filter <- join <- scan\")\n"
        "                && !ex.contains(\"scan[\")) {\n"
        "            score = score + 16;\n"
        "        }\n"
        "        Table<?> jr = jf.collect();\n"
        "        if (jr.rowCount() == 2 && jr.f64At(\"qty\", 0) == 20.0\n"
        "                && jr.f64At(\"qty\", 1) == 30.0) {\n"
        "            score = score + 32;\n"
        "        }\n"
        // A computed sort key has no column to order by — loud at plan
        // build, naming the fix.
        "        try {\n"
        "            Table<Px> bad = t.lazy().sort((PxCols c, Sorts s) -> {\n"
        "                s.add(c.px() * 2.0);\n"
        "            });\n"
        "        } catch (FrameException e) {\n"
        "            if (e.getMessage().contains(\"sort\")) { score = score + 64; }\n"
        "        }\n"
        "        return score;\n"
        "    }\n";
}

// One program, four independent entry points. Every record family the four
// scenarios need is declared once; each entry builds its own tables from the
// shared BUILDER TEXT (not from shared state), so a scenario can neither see
// nor disturb another's tables.
std::string sortJoinMatrixSrc() {
    return std::string(kPrelude) + kPxRecord + kJoinRecords + kFilledRecord
        + kNullKeyRecords
        + "public final class D {\n"
        + sortOrderEntry()
        + joinFamilyEntry()
        + nullKeyEntry()
        + pushdownEntry()
        + "}\n";
}

} // namespace

// The U9 sort/join use-case matrix, one compiled program (was: SortJoinTests.
// {sortOrdersAscDescStablyWithNullsLast, joinFamilyMatchesHandResults,
// nullKeysMatchNothingAndBadKeysFailLoud, filterSinksBelowSortAndJoinIsA
// Barrier} — four compiles' worth of claims, every assertion preserved):
//   9.1.1 sort order (asc/desc, stable ties, nulls last + NULLS_FIRST,
//   multi-key, utf8 keys); 9.1.2 the four join `how` kinds against hand
//   results with the checked narrow; 9.1.2/9.1.3 null keys matching nothing
//   and the loud key errors (dynamic form, key absent right, outer-join utf8
//   limit); 9.2.1 the U7 rewriter (filter sinks below sort, optimized ==
//   unoptimized, join is a barrier, computed sort key fails loud).
TEST(SortJoinTests, sortOrderJoinFamilyKeyErrorsAndPushdownMatrix) {
    auto jit = CajetaJit::compile(sortJoinMatrixSrc(), "test.D");
    ASSERT_NE(jit, nullptr);
    auto sortOrderScore  = jit->lookup<int32_t (*)()>("sortOrderScore");
    auto joinFamilyScore = jit->lookup<int32_t (*)()>("joinFamilyScore");
    auto nullKeyScore    = jit->lookup<int32_t (*)()>("nullKeyScore");
    auto pushdownScore   = jit->lookup<int32_t (*)()>("pushdownScore");
    ASSERT_NE(sortOrderScore, nullptr);
    ASSERT_NE(joinFamilyScore, nullptr);
    ASSERT_NE(nullKeyScore, nullptr);
    ASSERT_NE(pushdownScore, nullptr);

    // 9.1.1 — bits: 1 lazy (no execution at plan build), 2 ascending stable,
    // 4 descending stable, 8 nulls last ascending, 16 nulls last descending,
    // 32 Sorts.NULLS_FIRST override, 64 multi-key sym asc + px desc.
    EXPECT_EQ(sortOrderScore(), 127) << "sort order matrix";

    // 9.1.2 — bits: 1 lazy, 2 INNER shape/values, 4 right-side null rides
    // through, 8 LEFT null-extends, 16 RIGHT null-extends with the key from
    // the right, 32 FULL, 64 narrowing a nullable `fee` to `Filled` fails.
    EXPECT_EQ(joinFamilyScore(), 127) << "join family matrix";

    // 9.1.2 / 9.1.3 — bits: 1 INNER null key matches nothing, 2 FULL keeps
    // both null-key rows distinct, 4 dynamic key names the schema, 8 key
    // absent from the RIGHT fails at plan build, 16 outer-join utf8 limit
    // names `tag` and `utf8`, 32 the INNER join of the same pair is fine.
    EXPECT_EQ(nullKeyScore(), 63) << "null keys and loud key errors";

    // 9.2.1 — bits: 1 filter sank into scan[filter], 2 rows correct, 4 only
    // survivors scanned, 8 unoptimized agrees (and scans all 5), 16 join is a
    // barrier, 32 filtered join rows correct, 64 computed sort key fails loud.
    EXPECT_EQ(pushdownScore(), 127) << "rewriter pushdown and join barrier";
}
