//
// nucleo-frame U13 — the index interface + zone-maps (plan 13.1.1-13.1.3;
// spec §6, §9). The concrete payoff of the pluggable index interface for the
// common analytical scan: a ZONE-MAP (per-chunk min/max) lets a pushed range
// predicate skip whole chunks without reading their cells.
//
//  - 13.1.1 the contract + soundness: a range predicate over a chunked source
//    with zone-maps yields IDENTICAL rows to the same predicate run as a full
//    scan (`optimize(false)`) — skipping is sound, never lossy (both float and
//    int keys);
//  - 13.1.2 zone-maps skip provably-outside chunks (skip counts observable via
//    `scanChunksSkipped()`); an overlapping chunk is READ and row-filtered
//    (prune != filter) so only matching rows materialize (`scanRows()`);
//  - 13.1.3 a non-range predicate (`!=`) and an UNINDEXED (utf8) column degrade
//    to a full scan — never a wrong answer, just no skip.
//
// The chunk width is a source knob (`zoneChunkRows(n)`); the tests set 4 over a
// 16-row table so the four chunks are individually observable.
//
// Fold shape (test-battery-restructure 2.3): the three original tests each paid
// their own JIT compile of the SAME 16-row grid program — per-test coverage
// measurement (2026-08-10) showed their compiler line footprints near-identical.
// One program now carries all three use-cases as separate static entry points
// (each builds its own table, so no scenario can perturb another's scan
// counters), compiled once, with one C++ test asserting scenario by scenario
// against the original score expectations.
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

// 16 rows: a = 0.0 .. 15.0, k = 0 .. 15, g = "lo" (i<8) else "hi".
// Chunk width 4 -> chunks c0=[0..3] c1=[4..7] c2=[8..11] c3=[12..15].
const char* kBuild =
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
    "            StringColumn.of(gv));\n"
    "        t.zoneChunkRows(4);\n";

// One program, one entry point per use-case. Each entry rebuilds the grid
// locally (kBuild) so its scan counters are its own.
std::string zoneMapMatrixSrc() {
    return std::string(kPrelude) +
        "public final class D {\n"
        // 13.1.2 — zone-maps skip provably-outside chunks; the skip is
        // observable and an overlapping chunk is read + row-filtered (prune !=
        // filter). `a > 9.0` keeps 10..15 (6 rows): c0 (max 3) and c1 (max 7)
        // are entirely <= 9 and are SKIPPED; c2 [8..11] overlaps -> read and
        // filtered to {10,11}; c3 kept whole.
        "    public static int32 rangeSkipScore() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        "        Table<Grid> h = t.lazy().filter((GridCols c) -> c.a() > 9.0);\n"
        "        Table<Grid> r = h.collect();\n"
        "        if (r.rowCount() == 6 && r.a.get(0) == 10.0\n"
        "                && r.a.get(5) == 15.0 && r.k.get(0) == 10) {\n"
        "            score = score + 1;\n"
        "        }\n"
        "        if (h.scanRows() == 6) { score = score + 2; }\n"    // only matches materialize
        "        if (h.scanChunks() == 4) { score = score + 4; }\n"  // four chunks total
        "        if (h.scanChunksSkipped() == 2) { score = score + 8; }\n" // c0,c1 skipped
        "        return score;\n"
        "    }\n"
        // 13.1.1 — soundness: the zone-map-accelerated result is row-for-row
        // identical to the full scan (`optimize(false)`), for BOTH a float key
        // (`a > 9.0`) and an int key (`k >= 10`). Skipping never changes the
        // answer, it only skips work.
        "    public static int32 soundVsFullScanScore() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        // float key, accelerated vs full scan
        "        Table<Grid> h = t.lazy().filter((GridCols c) -> c.a() > 9.0);\n"
        "        Table<Grid> r = h.collect();\n"
        "        Table<Grid> h2 = t.lazy().filter((GridCols c) -> c.a() > 9.0);\n"
        "        h2.optimize(false);\n"
        "        Table<Grid> r2 = h2.collect();\n"
        "        boolean same = r.rowCount() == r2.rowCount();\n"
        "        int64 i = 0;\n"
        "        while (i < r.rowCount()) {\n"
        "            if (r.a.get(i) != r2.a.get(i)) { same = false; }\n"
        "            if (r.k.get(i) != r2.k.get(i)) { same = false; }\n"
        "            if (!r.g.get(i).equals(r2.g.get(i))) { same = false; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (same && h2.scanRows() == 16) { score = score + 1; }\n"
        // int key: k >= 10 keeps 10..15; c0 (max 3) c1 (max 7) skipped
        "        Table<Grid> hk = t.lazy().filter((GridCols c) -> c.k() >= 10);\n"
        "        Table<Grid> rk = hk.collect();\n"
        "        if (rk.rowCount() == 6 && rk.k.get(0) == 10\n"
        "                && hk.scanChunksSkipped() == 2 && hk.scanRows() == 6) {\n"
        "            score = score + 2;\n"
        "        }\n"
        "        Table<Grid> hk2 = t.lazy().filter((GridCols c) -> c.k() >= 10);\n"
        "        hk2.optimize(false);\n"
        "        Table<Grid> rk2 = hk2.collect();\n"
        "        boolean samek = rk.rowCount() == rk2.rowCount();\n"
        "        int64 j = 0;\n"
        "        while (j < rk.rowCount()) {\n"
        "            if (rk.k.get(j) != rk2.k.get(j)) { samek = false; }\n"
        "            j = j + 1;\n"
        "        }\n"
        "        if (samek) { score = score + 4; }\n"
        "        return score;\n"
        "    }\n"
        // 13.1.3 — a non-range predicate and an unindexed column degrade to a
        // full scan, never a wrong answer. `a != 5.0` is not range-shaped -> no
        // chunk is provably outside it -> zero skipped, all 4 chunks read, 15
        // rows kept. A utf8 column carries no zone-map (unindexed) -> the
        // `g == "hi"` filter also skips nothing, and its result matches the
        // full scan.
        "    public static int32 degradeToFullScanScore() {\n"
        + kBuild +
        "        int32 score = 0;\n"
        // non-range predicate on an indexed column
        "        Table<Grid> hn = t.lazy().filter((GridCols c) -> c.a() != 5.0);\n"
        "        Table<Grid> rn = hn.collect();\n"
        "        if (rn.rowCount() == 15 && hn.scanChunks() == 4\n"
        "                && hn.scanChunksSkipped() == 0 && hn.scanRows() == 15) {\n"
        "            score = score + 1;\n"
        "        }\n"
        // unindexed (utf8) column
        "        Table<Grid> hg = t.lazy().filter((GridCols c) -> c.g() == \"hi\");\n"
        "        Table<Grid> rg = hg.collect();\n"
        "        if (rg.rowCount() == 8 && rg.g.get(0).equals(\"hi\")\n"
        "                && hg.scanChunksSkipped() == 0) {\n"
        "            score = score + 2;\n"
        "        }\n"
        "        Table<Grid> hg2 = t.lazy().filter((GridCols c) -> c.g() == \"hi\");\n"
        "        hg2.optimize(false);\n"
        "        Table<Grid> rg2 = hg2.collect();\n"
        "        boolean same = rg.rowCount() == rg2.rowCount();\n"
        "        int64 i = 0;\n"
        "        while (i < rg.rowCount()) {\n"
        "            if (!rg.g.get(i).equals(rg2.g.get(i))) { same = false; }\n"
        "            if (rg.a.get(i) != rg2.a.get(i)) { same = false; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (same) { score = score + 4; }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
}

} // namespace

// The zone-map use-case matrix, one compiled program (was: ZoneMapTests.{zone
// MapSkipsChunksOutsideRangePredicate, chunkSkipIsSoundVsFullScan, nonRangeAnd
// UnindexedDegradeToFullScan} — three compiles of the same 16-row grid, every
// assertion preserved):
//   13.1.2 a range predicate skips the provably-outside chunks and row-filters
//   the overlapping one; 13.1.1 the accelerated answer is row-for-row identical
//   to `optimize(false)` for a float key and an int key; 13.1.3 a non-range
//   predicate and an unindexed utf8 column degrade to a full scan with zero
//   skips and the same rows.
TEST(ZoneMapTests, zoneMapSkipSoundnessAndFullScanDegradationMatrix) {
    auto jit = CajetaJit::compile(zoneMapMatrixSrc(), "test.D");
    ASSERT_NE(jit, nullptr);
    auto rangeSkipScore        = jit->lookup<int32_t (*)()>("rangeSkipScore");
    auto soundVsFullScanScore  = jit->lookup<int32_t (*)()>("soundVsFullScanScore");
    auto degradeToFullScanScore = jit->lookup<int32_t (*)()>("degradeToFullScanScore");
    ASSERT_NE(rangeSkipScore, nullptr);
    ASSERT_NE(soundVsFullScanScore, nullptr);
    ASSERT_NE(degradeToFullScanScore, nullptr);

    // 13.1.2 — bits: 1 rows/values, 2 scanRows()==6, 4 scanChunks()==4,
    // 8 scanChunksSkipped()==2.
    EXPECT_EQ(rangeSkipScore(), 15) << "score bits: 1=rows/values 2=scanRows "
                                       "4=scanChunks 8=scanChunksSkipped";

    // 13.1.1 — bits: 1 float key identical to full scan (and the full scan read
    // all 16 rows), 2 int key rows + skip counts, 4 int key identical to full
    // scan.
    EXPECT_EQ(soundVsFullScanScore(), 7)
        << "score bits: 1=float-key==fullscan 2=int-key rows/skips "
           "4=int-key==fullscan";

    // 13.1.3 — bits: 1 non-range predicate skips nothing, 2 unindexed utf8
    // filter skips nothing, 4 unindexed result identical to full scan.
    EXPECT_EQ(degradeToFullScanScore(), 7)
        << "score bits: 1=non-range no-skip 2=unindexed no-skip "
           "4=unindexed==fullscan";
}
