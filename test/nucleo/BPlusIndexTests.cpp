//
// nucleo-frame U14 — in-memory B+ index (plan 14.1.1-14.1.2; spec §9.5.1). The
// second near-term index structure after zone-maps: a BULK-LOADED B+ over a
// numeric key column answers POINT and RANGE lookups exactly, routed by the
// optimizer when a pushed predicate matches and falling back to the zone-map /
// full scan otherwise.
//
//  - 14.1.1 point (`k == v`) and range (`lo <= k <= hi`) lookups are exact vs a
//    full scan, and the B+ locates them DIRECTLY: `scanProbed()` (rows the scan
//    examined) equals the match count, not the table size — the index jumps to
//    the rows rather than scanning. A predicate over an un-indexed column falls
//    back (probes the whole scan);
//  - 14.1.2 an UNSORTED key column is handled: the bulk-load establishes key
//    order internally (sort-first, never refuse), so point/range stay exact and
//    row order is preserved — the accelerated result is row-for-row identical
//    to the full scan.
//
// The B+ contract EXTENDS the index result type past U13's chunk-mask to a
// POSITION SET (the rows a key predicate selects); the scan sorts the positions
// ascending before gathering so the output keeps storage order.
//
// Fold shape (test-battery-restructure 2.3, spec §3.2b/§3.3): the three
// original single-claim tests each paid their own JIT compile of a
// near-identical 16-row table program — per-test coverage measurement
// (2026-08-10) showed the nucleo table suites' compiler line footprints are
// shared almost in full (~18.8k lines), so three compiles bought three copies
// of the same coverage at three times the cost. One program now carries all
// three claims as SEPARATE STATIC ENTRY POINTS — each builds its own 16-row
// table locally and returns its own score, so no scenario can perturb another
// (no shared index state, no shared table) — behind one compile, and one C++
// test asserts each scenario's score independently so a failure still names the
// scenario. Every original score bit is preserved unchanged.
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
    "import cajeta.nucleo.column.Column;\n"
    "import cajeta.nucleo.column.StringColumn;\n"
    "public record Grid { float64 a; int64 k; Utf8 g; }\n";

// 16 rows, key column k SORTED (k = i, a = i).
const char* kSorted =
    "        float64[] av = heap float64[16];\n"
    "        int64[] kv = heap int64[16];\n"
    "        String[] gv = heap String[16];\n"
    "        int64 bi = 0;\n"
    "        while (bi < 16) {\n"
    "            av[bi] = (float64) bi;\n"
    "            kv[bi] = bi;\n"
    "            if (bi < 8) { gv[bi] = \"lo\"; } else { gv[bi] = \"hi\"; }\n"
    "            bi = bi + 1;\n"
    "        }\n"
    "        Table<Grid> t = heap Table<Grid>(\n"
    "            Column.of<float64>(av), Column.of<int64>(kv),\n"
    "            StringColumn.of(gv));\n";

// 16 rows, key column k SCRAMBLED (k = (i*7) % 16 — a permutation of 0..15),
// a = i. The B+ must establish key order at bulk-load.
const char* kScrambled =
    "        float64[] av = heap float64[16];\n"
    "        int64[] kv = heap int64[16];\n"
    "        String[] gv = heap String[16];\n"
    "        int64 bi = 0;\n"
    "        while (bi < 16) {\n"
    "            av[bi] = (float64) bi;\n"
    "            kv[bi] = (bi * 7) % 16;\n"
    "            gv[bi] = \"x\";\n"
    "            bi = bi + 1;\n"
    "        }\n"
    "        Table<Grid> t = heap Table<Grid>(\n"
    "            Column.of<float64>(av), Column.of<int64>(kv),\n"
    "            StringColumn.of(gv));\n";

// The U14 use-case matrix as one program: one entry point per scenario, each
// self-contained (its own table, its own index, its own score). The bodies are
// the original per-test `run()` bodies verbatim; only the method names differ.
std::string bPlusMatrixSrc() {
    return std::string(kPrelude) +
        "public final class D {\n"
        // 14.1.1 — int key point + range located directly by the B+, and a
        // non-indexed column falls back to the full scan.
        "    public static int32 intKeyPointRangeAndFallback() {\n"
        + kSorted +
        "        t.indexColumn(\"k\");\n"
        "        int32 score = 0;\n"
        // point lookup
        "        Table<Grid> hp = t.lazy().filter((GridCols c) -> c.k() == 7);\n"
        "        Table<Grid> rp = hp.collect();\n"
        "        if (rp.rowCount() == 1 && rp.k.get(0) == 7 && rp.a.get(0) == 7.0) {\n"
        "            score = score + 1;\n"
        "        }\n"
        "        if (hp.scanProbed() == 1) { score = score + 2; }\n"  // 1 row examined, not 16
        // range lookup 5 <= k <= 9
        "        Table<Grid> hr = t.lazy().filter((GridCols c) -> c.k() >= 5)\n"
        "                                 .filter((GridCols c) -> c.k() <= 9);\n"
        "        Table<Grid> rr = hr.collect();\n"
        "        if (rr.rowCount() == 5 && rr.k.get(0) == 5 && rr.k.get(4) == 9) {\n"
        "            score = score + 4;\n"
        "        }\n"
        "        if (hr.scanProbed() == 5) { score = score + 8; }\n"
        // fallback: predicate over the un-indexed column 'a' -> full scan
        "        Table<Grid> hf = t.lazy().filter((GridCols c) -> c.a() > 5.0);\n"
        "        Table<Grid> rf = hf.collect();\n"
        "        if (rf.rowCount() == 10 && hf.scanProbed() == 16) { score = score + 16; }\n"
        "        return score;\n"
        "    }\n"
        // 14.1.1 (float key) — a float key column range lookup, exact vs the
        // full scan (the `optimize(false)` re-run is the unoptimized control the
        // accelerated result is pinned against).
        "    public static int32 floatKeyRangeVsFullScan() {\n"
        + kSorted +
        "        t.indexColumn(\"a\");\n"
        "        int32 score = 0;\n"
        "        Table<Grid> hr = t.lazy().filter((GridCols c) -> c.a() >= 5.0)\n"
        "                                 .filter((GridCols c) -> c.a() <= 9.0);\n"
        "        Table<Grid> rr = hr.collect();\n"
        "        if (rr.rowCount() == 5 && rr.a.get(0) == 5.0 && rr.a.get(4) == 9.0) {\n"
        "            score = score + 1;\n"
        "        }\n"
        "        if (hr.scanProbed() == 5) { score = score + 2; }\n"
        "        Table<Grid> hr2 = t.lazy().filter((GridCols c) -> c.a() >= 5.0)\n"
        "                                  .filter((GridCols c) -> c.a() <= 9.0);\n"
        "        hr2.optimize(false);\n"
        "        Table<Grid> rr2 = hr2.collect();\n"
        "        boolean same = rr.rowCount() == rr2.rowCount();\n"
        "        int64 i = 0;\n"
        "        while (i < rr.rowCount()) {\n"
        "            if (rr.a.get(i) != rr2.a.get(i)) { same = false; }\n"
        "            if (rr.k.get(i) != rr2.k.get(i)) { same = false; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (same) { score = score + 4; }\n"
        "        return score;\n"
        "    }\n"
        // 14.1.2 — an unsorted key column: the bulk-load sorts (key, position)
        // pairs internally, so point/range stay exact and the result (positions
        // re-sorted ascending before the gather) is row-for-row identical to the
        // full scan.
        "    public static int32 unsortedKeyStaysExact() {\n"
        + kScrambled +
        "        t.indexColumn(\"k\");\n"
        "        int32 score = 0;\n"
        // range on the scrambled key vs full scan
        "        Table<Grid> hr = t.lazy().filter((GridCols c) -> c.k() >= 5)\n"
        "                                 .filter((GridCols c) -> c.k() <= 9);\n"
        "        Table<Grid> rr = hr.collect();\n"
        "        Table<Grid> hr2 = t.lazy().filter((GridCols c) -> c.k() >= 5)\n"
        "                                  .filter((GridCols c) -> c.k() <= 9);\n"
        "        hr2.optimize(false);\n"
        "        Table<Grid> rr2 = hr2.collect();\n"
        "        boolean same = rr.rowCount() == 5 && rr.rowCount() == rr2.rowCount();\n"
        "        int64 i = 0;\n"
        "        while (i < rr.rowCount()) {\n"
        "            if (rr.k.get(i) != rr2.k.get(i)) { same = false; }\n"
        "            if (rr.a.get(i) != rr2.a.get(i)) { same = false; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (same) { score = score + 1; }\n"
        "        if (hr.scanProbed() == 5) { score = score + 2; }\n"
        // point on the scrambled key vs full scan
        "        Table<Grid> hp = t.lazy().filter((GridCols c) -> c.k() == 8);\n"
        "        Table<Grid> rp = hp.collect();\n"
        "        Table<Grid> hp2 = t.lazy().filter((GridCols c) -> c.k() == 8);\n"
        "        hp2.optimize(false);\n"
        "        Table<Grid> rp2 = hp2.collect();\n"
        "        if (rp.rowCount() == 1 && rp.k.get(0) == rp2.k.get(0)\n"
        "                && rp.a.get(0) == rp2.a.get(0)) {\n"
        "            score = score + 4;\n"
        "        }\n"
        "        if (hp.scanProbed() == 1) { score = score + 8; }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
}

} // namespace

// The U14 B+ use-case matrix, one compiled program (was: BPlusIndexTests.{int
// KeyPointAndRangeLocatedByIndex, floatKeyRangeMatchesFullScan, unsortedKey
// ColumnStaysExact} — three compiles' worth of claims, every score bit
// preserved):
//   int key point + range located DIRECTLY by the index (scanProbed == match
//   count, not table size); a predicate over an un-indexed column falls back to
//   the full scan; a float key column range is exact and pinned row-for-row
//   against the unoptimized run; an UNSORTED key column stays exact for both
//   range and point, row-for-row identical to the full scan, and still located
//   directly.
TEST(BPlusIndexTests, intAndFloatKeyPointRangeFallbackAndUnsortedKeyMatrix) {
    auto jit = CajetaJit::compile(bPlusMatrixSrc(), "test.D");
    ASSERT_NE(jit, nullptr);
    auto intKeyPointRangeAndFallback =
        jit->lookup<int32_t (*)()>("intKeyPointRangeAndFallback");
    auto floatKeyRangeVsFullScan =
        jit->lookup<int32_t (*)()>("floatKeyRangeVsFullScan");
    auto unsortedKeyStaysExact =
        jit->lookup<int32_t (*)()>("unsortedKeyStaysExact");
    ASSERT_NE(intKeyPointRangeAndFallback, nullptr);
    ASSERT_NE(floatKeyRangeVsFullScan, nullptr);
    ASSERT_NE(unsortedKeyStaysExact, nullptr);

    // 14.1.1 — int key point + range located directly by the B+, and a
    // non-indexed column falls back to the full scan. Score bits: 1 = point
    // result exact (1 row, k == 7, a == 7.0); 2 = point probed 1 row, not 16;
    // 4 = range result exact (5 rows, k 5..9); 8 = range probed 5 rows;
    // 16 = un-indexed predicate on 'a' returns 10 rows having probed all 16.
    EXPECT_EQ(intKeyPointRangeAndFallback(), 31)
        << "int key point/range/fallback score bits (1|2|4|8|16)";

    // 14.1.1 (float key) — score bits: 1 = range result exact (5 rows, a
    // 5.0..9.0); 2 = range probed 5 rows; 4 = accelerated result equals the
    // optimize(false) full-scan result row-for-row on both a and k.
    EXPECT_EQ(floatKeyRangeVsFullScan(), 7)
        << "float key range score bits (1|2|4)";

    // 14.1.2 — score bits: 1 = scrambled-key range is 5 rows and row-for-row
    // identical to the unoptimized run; 2 = range probed 5 rows; 4 = scrambled
    // -key point is 1 row and matches the unoptimized run; 8 = point probed 1
    // row.
    EXPECT_EQ(unsortedKeyStaysExact(), 15)
        << "unsorted key point/range score bits (1|2|4|8)";
}
