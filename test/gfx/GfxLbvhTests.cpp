//
// GfxLbvhTests — cajeta-gfx plan §3 (slice 3-a-1): Morton encoding for the
// pure-cajeta software LBVH builder (cajeta.gpu.Lbvh).
//
// The LBVH (Lauterbach et al. 2009) orders primitives along a Morton (Z-order)
// space-filling curve, then builds a binary radix tree over the sorted codes.
// This slice is the foundational primitive — the Morton code itself:
//   - quantize(v, lo, hi): map a coordinate in [lo, hi] to a 10-bit cell index
//     in [0, 1023], clamped (the per-axis lattice resolution of a 30-bit code).
//   - expandBits(v): spread the 10 low bits of v across every 3rd output bit
//     (bit i -> bit 3i), the interleave half of a 3-D Morton code. Implemented
//     with LEFT shifts + or + and only (no int32 `>>>`, which mis-lowers to an
//     arithmetic shift in the current codegen — see the value-type-codegen
//     gotchas memory).
//   - morton3D(qx, qy, qz): interleave three 10-bit cell indices into one
//     30-bit Z-order code (x in bits 2,5,..; y in 1,4,..; z in 0,3,..), which
//     fits a positive int32 (max bit position 29).
//
// The clean analytic properties: exact golden interleave values, strict
// monotonicity of the code along a single axis (Z-order preserves per-axis
// order when the other axes are fixed), and correct endpoint/clamp behaviour of
// the quantizer. Pure scalar int/float math -> host-testable (pure host JIT, no
// device) and lowers identically on the GPU.
//

#include <gtest/gtest.h>
#include "../jit/JitTestHelper.h"

#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& imports, const std::string& body) {
    std::string src =
        "package test;\n"
        + imports +
        "public final class D {\n"
        "    public static int32 run() {\n"
        + body +
        "    }\n"
        "}\n";
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* IMP = "import cajeta.gpu.Lbvh;\n";

} // namespace

// 3-a-1 — expandBits golden values. bit i of the input lands at bit 3i of the
// output: 0->0, 1->1, 2->8 (bit1->bit3), 3->9, and all ten bits set (1023)
// spreads to 0x09249249 = 153391689 = sum_{i=0..9} 2^(3i).
TEST(GfxLbvhTests, expandBitsGolden) {
    EXPECT_EQ(runI32(IMP,
        "        if (Lbvh.expandBits(0) != 0) { return -1; }\n"
        "        if (Lbvh.expandBits(1) != 1) { return -2; }\n"
        "        if (Lbvh.expandBits(2) != 8) { return -3; }\n"
        "        if (Lbvh.expandBits(3) != 9) { return -4; }\n"
        "        if (Lbvh.expandBits(4) != 64) { return -5; }\n"
        "        if (Lbvh.expandBits(1023) != 153391689) { return -6; }\n"
        "        return 0;\n"), 0);
}

// 3-a-1 — morton3D interleave golden values. x occupies bits 2,5,..; y bits
// 1,4,..; z bits 0,3,.. So (0,0,0)=0; a unit in x -> 4, y -> 2, z -> 1; the
// all-unit cell (1,1,1) -> 7; and (1,1,1) at the top cell stays consistent.
TEST(GfxLbvhTests, morton3DGolden) {
    EXPECT_EQ(runI32(IMP,
        "        if (Lbvh.morton3D(0, 0, 0) != 0) { return -1; }\n"
        "        if (Lbvh.morton3D(1, 0, 0) != 4) { return -2; }\n"
        "        if (Lbvh.morton3D(0, 1, 0) != 2) { return -3; }\n"
        "        if (Lbvh.morton3D(0, 0, 1) != 1) { return -4; }\n"
        "        if (Lbvh.morton3D(1, 1, 1) != 7) { return -5; }\n"
        "        if (Lbvh.morton3D(2, 0, 0) != 32) { return -6; }\n"
        "        return 0;\n"), 0);
}

// 3-a-1 — the code is strictly increasing along a single axis (the other axes
// fixed at 0): morton3D(q,0,0) for q = 0..1022 < morton3D(q+1,0,0). The 30-bit
// code stays a positive int32 throughout (max bit position 29).
TEST(GfxLbvhTests, mortonAxisMonotonic) {
    EXPECT_EQ(runI32(IMP,
        "        int32 q = 0;\n"
        "        int32 prev = 0 - 1;\n"
        "        while (q < 1023) {\n"
        "            int32 code = Lbvh.morton3D(q, 0, 0);\n"
        "            if (code < 0) { return -1; }\n"
        "            if (code <= prev) { return -2; }\n"
        "            prev = code;\n"
        "            q = q + 1;\n"
        "        }\n"
        "        return 0;\n"), 0);
}

// 3-a-1 — quantize maps the span [lo, hi] onto cells [0, 1023]: the low edge ->
// 0, the high edge -> 1023, the midpoint near 511, and values outside the span
// clamp to the ends (so out-of-bounds prims never index past the lattice).
TEST(GfxLbvhTests, quantizeMapsRange) {
    EXPECT_EQ(runI32(IMP,
        "        if (Lbvh.quantize(0.0f - 2.0f, 0.0f - 2.0f, 6.0f) != 0) { return -1; }\n"
        "        if (Lbvh.quantize(6.0f, 0.0f - 2.0f, 6.0f) != 1023) { return -2; }\n"
        "        int32 mid = Lbvh.quantize(2.0f, 0.0f - 2.0f, 6.0f);\n"
        "        if (mid < 509) { return -3; }\n"
        "        if (mid > 513) { return -4; }\n"
        "        if (Lbvh.quantize(0.0f - 100.0f, 0.0f - 2.0f, 6.0f) != 0) { return -5; }\n"
        "        if (Lbvh.quantize(100.0f, 0.0f - 2.0f, 6.0f) != 1023) { return -6; }\n"
        "        if (Lbvh.quantize(3.0f, 5.0f, 5.0f) != 0) { return -7; }\n"  // degenerate span
        "        return 0;\n"), 0);
}

// 3-a-2 — radixSort sorts the key array ascending and carries its payload along
// (the permutation that orders primitives by Morton code). Keys span several
// bytes (0 .. 1000000) to exercise all four LSD passes; the payload is the
// original index, so after the sort it must read back as the argsort.
TEST(GfxLbvhTests, radixSortBasic) {
    EXPECT_EQ(runI32(IMP,
        "        int32 n = 8;\n"
        "        int32[] keys = heap int32[8];\n"
        "        int32[] vals = heap int32[8];\n"
        "        keys[0] = 300;     vals[0] = 0;\n"
        "        keys[1] = 5;       vals[1] = 1;\n"
        "        keys[2] = 70000;   vals[2] = 2;\n"
        "        keys[3] = 1;       vals[3] = 3;\n"
        "        keys[4] = 256;     vals[4] = 4;\n"
        "        keys[5] = 255;     vals[5] = 5;\n"
        "        keys[6] = 1000000; vals[6] = 6;\n"
        "        keys[7] = 0;       vals[7] = 7;\n"
        "        Lbvh.radixSort(keys, vals, n);\n"
        // expected keys ascending: 0,1,5,255,256,300,70000,1000000
        "        int32 i = 0;\n"
        "        while (i < n) {\n"
        "            if (i > 0) { if (keys[i] < keys[i - 1]) { return -1; } }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (keys[0] != 0) { return -2; }\n"
        "        if (keys[7] != 1000000) { return -3; }\n"
        // payload follows the keys: argsort = {7,3,1,5,4,0,2,6}
        "        if (vals[0] != 7) { return -4; }\n"
        "        if (vals[1] != 3) { return -5; }\n"
        "        if (vals[2] != 1) { return -6; }\n"
        "        if (vals[3] != 5) { return -7; }\n"
        "        if (vals[4] != 4) { return -8; }\n"
        "        if (vals[5] != 0) { return -9; }\n"
        "        if (vals[6] != 2) { return -10; }\n"
        "        if (vals[7] != 6) { return -11; }\n"
        "        return 0;\n"), 0);
}

// 3-a-2 — radixSort is STABLE: equal keys keep their original payload order.
// Four entries with key 42 (payloads 0,1,2,3) interleaved with smaller/larger
// keys must come out with payloads still ascending among the ties.
TEST(GfxLbvhTests, radixSortStable) {
    EXPECT_EQ(runI32(IMP,
        "        int32 n = 6;\n"
        "        int32[] keys = heap int32[6];\n"
        "        int32[] vals = heap int32[6];\n"
        "        keys[0] = 42; vals[0] = 0;\n"
        "        keys[1] = 7;  vals[1] = 100;\n"
        "        keys[2] = 42; vals[2] = 1;\n"
        "        keys[3] = 42; vals[3] = 2;\n"
        "        keys[4] = 9;  vals[4] = 200;\n"
        "        keys[5] = 42; vals[5] = 3;\n"
        "        Lbvh.radixSort(keys, vals, n);\n"
        // order: 7(100), 9(200), 42(0), 42(1), 42(2), 42(3)
        "        if (keys[0] != 7) { return -1; }\n"
        "        if (keys[1] != 9) { return -2; }\n"
        "        if (vals[0] != 100) { return -3; }\n"
        "        if (vals[1] != 200) { return -4; }\n"
        "        if (vals[2] != 0) { return -5; }\n"
        "        if (vals[3] != 1) { return -6; }\n"
        "        if (vals[4] != 2) { return -7; }\n"
        "        if (vals[5] != 3) { return -8; }\n"
        "        return 0;\n"), 0);
}

// 3-a-2 — integration with the Morton primitive: codes for points strung along
// the x-axis are monotonic in x (slice 1), so sorting the codes orders the
// payload (the point index) by x. Build codes for x = 3,1,2,0 and confirm the
// argsort is {3,1,2,0} (the indices of x = 0,1,2,3).
TEST(GfxLbvhTests, radixSortMortonOrder) {
    EXPECT_EQ(runI32(IMP,
        "        int32 n = 4;\n"
        "        int32[] keys = heap int32[4];\n"
        "        int32[] vals = heap int32[4];\n"
        "        int32 qx0 = Lbvh.quantize(3.0f, 0.0f, 3.0f);\n"
        "        int32 qx1 = Lbvh.quantize(1.0f, 0.0f, 3.0f);\n"
        "        int32 qx2 = Lbvh.quantize(2.0f, 0.0f, 3.0f);\n"
        "        int32 qx3 = Lbvh.quantize(0.0f, 0.0f, 3.0f);\n"
        "        keys[0] = Lbvh.morton3D(qx0, 0, 0); vals[0] = 0;\n"
        "        keys[1] = Lbvh.morton3D(qx1, 0, 0); vals[1] = 1;\n"
        "        keys[2] = Lbvh.morton3D(qx2, 0, 0); vals[2] = 2;\n"
        "        keys[3] = Lbvh.morton3D(qx3, 0, 0); vals[3] = 3;\n"
        "        Lbvh.radixSort(keys, vals, n);\n"
        "        int32 i = 0;\n"
        "        while (i < n) {\n"
        "            if (i > 0) { if (keys[i] < keys[i - 1]) { return -1; } }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        if (vals[0] != 3) { return -2; }\n"
        "        if (vals[1] != 1) { return -3; }\n"
        "        if (vals[2] != 2) { return -4; }\n"
        "        if (vals[3] != 0) { return -5; }\n"
        "        return 0;\n"), 0);
}

// 3-a-3a — Lbvh.build emits the frozen threaded-DFS block. Four unit boxes along
// x (centres 0,1,2,3): assert the header words, the root node's AABB (= the
// scene union) and escape (= nodeCount), and that the primRef table lists every
// primitive exactly once. nodeCount = 2*4-1 = 7; nodesOff = 8; primRefOff =
// 8 + 7*9 = 71. Structural integers are compared as floats (exact small ints);
// prim indices are read back via Lbvh.floorToInt (direct float32->int32 casts
// mis-lower).
TEST(GfxLbvhTests, buildBlockStructure4) {
    EXPECT_EQ(runI32(IMP,
        "        int32 count = 4;\n"
        "        float32[] boxes = heap float32[24];\n"
        "        int32 i = 0;\n"
        "        while (i < 4) {\n"
        "            float32 cx = i;\n"
        "            int32 b6 = i * 6;\n"
        "            boxes[b6 + 0] = cx - 0.5f;\n"
        "            boxes[b6 + 1] = 0.0f - 0.5f;\n"
        "            boxes[b6 + 2] = 0.0f - 0.5f;\n"
        "            boxes[b6 + 3] = cx + 0.5f;\n"
        "            boxes[b6 + 4] = 0.5f;\n"
        "            boxes[b6 + 5] = 0.5f;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        float32[] blk = Lbvh.build(boxes, count);\n"
        // header
        "        if (blk[0] != 1.0f) { return -1; }\n"
        "        if (blk[1] != 7.0f) { return -2; }\n"      // nodeCount
        "        if (blk[2] != 4.0f) { return -3; }\n"      // primCount
        "        if (blk[4] != 8.0f) { return -4; }\n"      // nodesOffset
        "        if (blk[5] != 71.0f) { return -5; }\n"     // primRefOffset
        "        if (blk[6] != 1.0f) { return -6; }\n"      // AABB flag
        "        if (blk[7] != 9.0f) { return -7; }\n"      // stride
        // root node (base 8): AABB = scene union, escape = nodeCount
        "        if (blk[8] != 0.0f - 0.5f) { return -8; }\n"   // minX
        "        if (blk[11] != 3.5f) { return -9; }\n"         // maxX
        "        if (blk[12] != 0.5f) { return -10; }\n"        // maxY
        "        if (blk[14] != 7.0f) { return -11; }\n"        // root escape == nodeCount
        // primRef coverage: each prim 0..3 appears exactly once in [71, 75)
        "        int32[] seen = heap int32[4];\n"
        "        int32 k = 0;\n"
        "        while (k < 4) {\n"
        "            int32 p = Lbvh.floorToInt(blk[71 + k]);\n"
        "            if (p < 0) { return -12; }\n"
        "            if (p > 3) { return -13; }\n"
        "            seen[p] = seen[p] + 1;\n"
        "            k = k + 1;\n"
        "        }\n"
        "        k = 0;\n"
        "        while (k < 4) {\n"
        "            if (seen[k] != 1) { return -14; }\n"
        "            k = k + 1;\n"
        "        }\n"
        "        return 0;\n"), 0);
}

// 3-a-3a — single-primitive edge case: count = 1 -> one leaf root. nodeCount = 1,
// primRefOff = 8 + 1*9 = 17, the root is a leaf (primCount 1, firstPrim 0,
// escape 1) whose AABB is exactly the one box.
TEST(GfxLbvhTests, buildBlockSinglePrim) {
    EXPECT_EQ(runI32(IMP,
        "        float32[] boxes = heap float32[6];\n"
        "        boxes[0] = 1.0f; boxes[1] = 2.0f; boxes[2] = 3.0f;\n"
        "        boxes[3] = 4.0f; boxes[4] = 5.0f; boxes[5] = 6.0f;\n"
        "        float32[] blk = Lbvh.build(boxes, 1);\n"
        "        if (blk[1] != 1.0f) { return -1; }\n"      // nodeCount
        "        if (blk[2] != 1.0f) { return -2; }\n"      // primCount
        "        if (blk[5] != 17.0f) { return -3; }\n"     // primRefOffset
        // root leaf node at base 8
        "        if (blk[8] != 1.0f) { return -4; }\n"      // minX
        "        if (blk[13] != 6.0f) { return -5; }\n"     // maxZ
        "        if (blk[14] != 1.0f) { return -6; }\n"     // escape == nodeCount
        "        if (blk[15] != 0.0f) { return -7; }\n"     // firstPrim
        "        if (blk[16] != 1.0f) { return -8; }\n"     // primCount = leaf
        "        if (Lbvh.floorToInt(blk[17]) != 0) { return -9; }\n"  // primRef[0] = 0
        "        return 0;\n"), 0);
}

// 3-a-3a — a larger scene (8 boxes on a 2x2x2 lattice) exercises multi-level
// recursion. Assert nodeCount = 2*8-1 = 15, the root AABB spans the whole
// lattice, the root escape = 15, and every primitive appears exactly once.
TEST(GfxLbvhTests, buildBlockCoverage8) {
    EXPECT_EQ(runI32(IMP,
        "        int32 count = 8;\n"
        "        float32[] boxes = heap float32[48];\n"
        "        int32 i = 0;\n"
        "        while (i < 8) {\n"
        "            int32 ax = i % 2;\n"
        "            int32 ay = (i / 2) % 2;\n"
        "            int32 az = (i / 4) % 2;\n"
        "            float32 fx = ax;\n"
        "            float32 fy = ay;\n"
        "            float32 fz = az;\n"
        "            int32 b6 = i * 6;\n"
        "            boxes[b6 + 0] = fx;        boxes[b6 + 1] = fy;        boxes[b6 + 2] = fz;\n"
        "            boxes[b6 + 3] = fx + 0.5f; boxes[b6 + 4] = fy + 0.5f; boxes[b6 + 5] = fz + 0.5f;\n"
        "            i = i + 1;\n"
        "        }\n"
        "        float32[] blk = Lbvh.build(boxes, count);\n"
        "        if (blk[1] != 15.0f) { return -1; }\n"     // nodeCount = 2*8-1
        "        if (blk[2] != 8.0f) { return -2; }\n"
        "        if (blk[8] != 0.0f) { return -3; }\n"      // root minX
        "        if (blk[11] != 1.5f) { return -4; }\n"     // root maxX (1.0 + 0.5)
        "        if (blk[14] != 15.0f) { return -5; }\n"    // root escape == nodeCount
        "        int32 primRefOff = Lbvh.floorToInt(blk[5]);\n"
        "        int32[] seen = heap int32[8];\n"
        "        int32 k = 0;\n"
        "        while (k < 8) {\n"
        "            int32 p = Lbvh.floorToInt(blk[primRefOff + k]);\n"
        "            if (p < 0) { return -6; }\n"
        "            if (p > 7) { return -7; }\n"
        "            seen[p] = seen[p] + 1;\n"
        "            k = k + 1;\n"
        "        }\n"
        "        k = 0;\n"
        "        while (k < 8) {\n"
        "            if (seen[k] != 1) { return -8; }\n"
        "            k = k + 1;\n"
        "        }\n"
        "        return 0;\n"), 0);
}
