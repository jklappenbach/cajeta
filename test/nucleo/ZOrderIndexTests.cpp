//
// nucleo-frame U15 — Z-order-over-B+ (plan 15.1.1; spec §9.5.2). A
// low-dimensional (2-D) spatial table query is answered by reusing the SAME B+
// (`BPlusIndex`, int path) on a Z-ORDERED composite key — NO second tree type
// (the deliberate reuse). The 2-D box maps to a single contiguous morton-key
// range that OVER-COVERS the rectangle; the B+ range probe returns candidates
// and the row-wise box predicate makes the answer exact (prune, not filter).
//
//  - 15.1.1 a 2-D range query over a spatial-indexed (x, y) pair is exact vs the
//    full scan; the morton range prunes the candidate set below the table size
//    (`scanProbed()` between the match count and N), and an un-indexed table
//    degrades to a correct full scan.
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
    "import cajeta.nucleo.frame.Table;\n"
    "import cajeta.nucleo.frame.FrameException;\n"
    "import cajeta.nucleo.frame.Morton;\n"
    "import cajeta.nucleo.column.Column;\n"
    "public record Pt { int64 x; int64 y; float64 v; }\n";

// An 8x8 grid: point i at (x = i/8, y = i%8), v = x*10 + y. Storage order is
// (x*8 + y), so a box's rows come out x-major then y.
const char* kGrid =
    "        int64[] xv = heap int64[64];\n"
    "        int64[] yv = heap int64[64];\n"
    "        float64[] vv = heap float64[64];\n"
    "        int64 bi = 0;\n"
    "        while (bi < 64) {\n"
    "            int64 gx = bi / 8;\n"
    "            int64 gy = bi % 8;\n"
    "            xv[bi] = gx;\n"
    "            yv[bi] = gy;\n"
    "            vv[bi] = (float64) (gx * 10 + gy);\n"
    "            bi = bi + 1;\n"
    "        }\n"
    "        Table<Pt> t = heap Table<Pt>(\n"
    "            Column.of<int64>(xv), Column.of<int64>(yv),\n"
    "            Column.of<float64>(vv));\n";

} // namespace

// The morton encoding interleaves the two coordinates' bits; a box's corners
// bound every interior point's key, so [Z(x0,y0), Z(x1,y1)] contains the box.
TEST(ZOrderIndexTests, mortonEncodingInterleavesAndOrders) {
    auto src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32 score = 0;\n"
        // Z(2,3) = 14, Z(4,5) = 50 (interleave x even bits, y odd bits)
        "        if (Morton.encode(2, 3) == 14) { score = score + 1; }\n"
        "        if (Morton.encode(4, 5) == 50) { score = score + 2; }\n"
        // monotone: every box point's key is within the corner keys
        "        int64 zlo = Morton.encode(2, 3);\n"
        "        int64 zhi = Morton.encode(4, 5);\n"
        "        boolean inside = true;\n"
        "        int64 px = 2;\n"
        "        while (px <= 4) {\n"
        "            int64 py = 3;\n"
        "            while (py <= 5) {\n"
        "                int64 z = Morton.encode(px, py);\n"
        "                if (z < zlo || z > zhi) { inside = false; }\n"
        "                py = py + 1;\n"
        "            }\n"
        "            px = px + 1;\n"
        "        }\n"
        "        if (inside) { score = score + 4; }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 7);
}

// 15.1.1 — 2-D range over a spatial-indexed (x, y) pair: exact vs full scan,
// the morton range prunes the candidate set, and an un-indexed table degrades.
TEST(ZOrderIndexTests, spatialBoxQueryExactAndPruned) {
    auto src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kGrid +
        "        t.indexSpatial(\"x\", \"y\");\n"
        "        int32 score = 0;\n"
        // box x in [2,4], y in [3,5] -> 9 points
        "        Table<Pt> h = t.lazy().filter((PtCols c) -> c.x() >= 2)\n"
        "                              .filter((PtCols c) -> c.x() <= 4)\n"
        "                              .filter((PtCols c) -> c.y() >= 3)\n"
        "                              .filter((PtCols c) -> c.y() <= 5);\n"
        "        Table<Pt> r = h.collect();\n"
        "        if (r.rowCount() == 9) { score = score + 1; }\n"
        "        if (r.x.get(0) == 2 && r.y.get(0) == 3 && r.v.get(0) == 23.0\n"
        "                && r.x.get(8) == 4 && r.y.get(8) == 5 && r.v.get(8) == 45.0) {\n"
        "            score = score + 2;\n"
        "        }\n"
        // the morton range prunes: fewer than all 64 candidates, at least the 9
        "        if (h.scanProbed() >= 9 && h.scanProbed() < 64) { score = score + 4; }\n"
        // exact vs full scan
        "        Table<Pt> h2 = t.lazy().filter((PtCols c) -> c.x() >= 2)\n"
        "                               .filter((PtCols c) -> c.x() <= 4)\n"
        "                               .filter((PtCols c) -> c.y() >= 3)\n"
        "                               .filter((PtCols c) -> c.y() <= 5);\n"
        "        h2.optimize(false);\n"
        "        Table<Pt> r2 = h2.collect();\n"
        "        boolean same = r.rowCount() == r2.rowCount();\n"
        "        int64 i = 0;\n"
        "        while (i < r.rowCount()) {\n"
        "            if (r.x.get(i) != r2.x.get(i)) { same = false; }\n"
        "            if (r.y.get(i) != r2.y.get(i)) { same = false; }\n"
        "            if (r.v.get(i) != r2.v.get(i)) { same = false; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (same) { score = score + 8; }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 15);
}

// Degrade: without a spatial index the box query still returns the exact rows
// (routed through the ordinary scan), so an absent index costs performance, not
// correctness.
TEST(ZOrderIndexTests, unindexedSpatialDegradesToFullScan) {
    auto src = std::string(kPrelude) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + kGrid +
        "        int32 score = 0;\n"  // no indexSpatial
        "        Table<Pt> h = t.lazy().filter((PtCols c) -> c.x() >= 2)\n"
        "                              .filter((PtCols c) -> c.x() <= 4)\n"
        "                              .filter((PtCols c) -> c.y() >= 3)\n"
        "                              .filter((PtCols c) -> c.y() <= 5);\n"
        "        Table<Pt> r = h.collect();\n"
        "        if (r.rowCount() == 9 && r.x.get(0) == 2 && r.y.get(0) == 3\n"
        "                && r.v.get(8) == 45.0) {\n"
        "            score = score + 1;\n"
        "        }\n"
        "        return score;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}
