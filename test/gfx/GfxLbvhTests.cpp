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
