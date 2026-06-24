//
// GfxBvhCodecTests — cajeta-gfx plan §3 (3.a-rest): the bit-reinterpret +
// quantized-node codec for a compressed software BVH (cajeta.gpu.BvhCodec).
//
// The parked acceleration upgrade off Lbvh. Two pure encode/decode primitives,
// no traversal (the single shipped SoftwareRayQuery.step is untouched):
//
//   1. REINTERPRET — packInt/unpackInt carry an exact int32 in a float32 slot by
//      IEEE-754 bitcast (Cajeta.bitsToF32/f32ToBits), so the BVH block's integer
//      fields read back with a single bitcast instead of a float->int conversion
//      cast (which mis-lowers AND loses integers above 2^24). The round-trip is
//      bit-exact for every int32.
//
//   2. QUANTIZED NODES — quantizeMin rounds a child box's low corner DOWN and
//      quantizeMax rounds the high corner UP into 8-bit cells of the parent span,
//      so dequantize always returns a CONSERVATIVE box enclosing the true child
//      (correct culling), tight to within one quantum (extent/255). packBytes4 /
//      unpackByte pack the quantized bytes into int32 words.
//
// Pure scalar math, host-testable (pure host JIT, no device).
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

const char* IMP = "import cajeta.gpu.BvhCodec;\n";

} // namespace

// 3.a-rest — reinterpret round-trip is bit-exact for every int32, INCLUDING values
// above 2^24 that a float conversion would round away. 16777217 (= 2^24+1) survives
// packInt->unpackInt exactly, while routing it through a float (the float-stored
// layout) collapses it to 16777216 — the reason to reinterpret.
TEST(GfxBvhCodecTests, reinterpretRoundTripExact) {
    EXPECT_EQ(runI32(IMP,
        "        int32 a = BvhCodec.unpackInt(BvhCodec.packInt(0));\n"
        "        if (a != 0) { return -1; }\n"
        "        int32 b = BvhCodec.unpackInt(BvhCodec.packInt(1));\n"
        "        if (b != 1) { return -2; }\n"
        "        int32 c = BvhCodec.unpackInt(BvhCodec.packInt(0 - 42));\n"
        "        if (c != 0 - 42) { return -3; }\n"
        "        int32 big = 16777217;\n"                       // 2^24 + 1
        "        int32 d = BvhCodec.unpackInt(BvhCodec.packInt(big));\n"
        "        if (d != big) { return -4; }\n"
        // contrast: through a float value (the float-stored layout) it is LOST
        "        float32 f = big;\n"                            // int32 -> float32 rounds
        "        int32 viaFloat = BvhCodec.floorToInt(f);\n"
        "        if (viaFloat != 16777216) { return -5; }\n"    // proves conversion loses it
        "        if (viaFloat == big) { return -6; }\n"
        // a large prim-index-like value round-trips too
        "        int32 idx = 123456789;\n"
        "        int32 e = BvhCodec.unpackInt(BvhCodec.packInt(idx));\n"
        "        if (e != idx) { return -7; }\n"
        "        return 0;\n"), 0);
}

// 3.a-rest — the decoded box CONSERVATIVELY encloses the child: for parent span
// [0,10] and child [2.3, 7.8], dequantize(quantizeMin) <= 2.3 and
// dequantize(quantizeMax) >= 7.8, both within one quantum (10/255 ~ 0.039).
TEST(GfxBvhCodecTests, quantizeConservativeAndTight) {
    EXPECT_EQ(runI32(IMP,
        "        float32 lo = 0.0f;\n"
        "        float32 hi = 10.0f;\n"
        "        float32 cmin = 2.3f;\n"
        "        float32 cmax = 7.8f;\n"
        "        int32 qlo = BvhCodec.quantizeMin(cmin, lo, hi);\n"
        "        int32 qhi = BvhCodec.quantizeMax(cmax, lo, hi);\n"
        "        float32 dlo = BvhCodec.dequantize(qlo, lo, hi);\n"
        "        float32 dhi = BvhCodec.dequantize(qhi, lo, hi);\n"
        // conservative: decoded min at or below true min, decoded max at or above
        "        if (dlo > cmin + 0.0001f) { return -1; }\n"
        "        if (dhi < cmax - 0.0001f) { return -2; }\n"
        // tight: within one quantum (0.0393) of the true corner
        "        if (dlo < cmin - 0.04f) { return -3; }\n"
        "        if (dhi > cmax + 0.04f) { return -4; }\n"
        // quantized cells are in range
        "        if (qlo < 0) { return -5; }\n"
        "        if (qlo > 255) { return -6; }\n"
        "        if (qhi < 0) { return -7; }\n"
        "        if (qhi > 255) { return -8; }\n"
        "        return 0;\n"), 0);
}

// 3.a-rest — endpoint and clamp behaviour: the low corner at `lo` quantizes to 0
// (decodes to lo), the high corner at `hi` quantizes to 255 (decodes to hi), and
// out-of-span coordinates clamp instead of indexing past the lattice.
TEST(GfxBvhCodecTests, quantizeEndpointsAndClamp) {
    EXPECT_EQ(runI32(IMP,
        "        float32 lo = 0.0f;\n"
        "        float32 hi = 10.0f;\n"
        "        if (BvhCodec.quantizeMin(0.0f, lo, hi) != 0) { return -1; }\n"
        "        if (BvhCodec.quantizeMax(10.0f, lo, hi) != 255) { return -2; }\n"
        // dequantize the extremes reproduces the span ends
        "        float32 d0 = BvhCodec.dequantize(0, lo, hi);\n"
        "        if (d0 < -0.0001f) { return -3; }\n"
        "        if (d0 > 0.0001f) { return -4; }\n"
        "        float32 d255 = BvhCodec.dequantize(255, lo, hi);\n"
        "        if (d255 < 9.9999f) { return -5; }\n"
        "        if (d255 > 10.0001f) { return -6; }\n"
        // below-span min clamps to 0, above-span max clamps to 255
        "        if (BvhCodec.quantizeMin(0.0f - 5.0f, lo, hi) != 0) { return -7; }\n"
        "        if (BvhCodec.quantizeMax(20.0f, lo, hi) != 255) { return -8; }\n"
        "        return 0;\n"), 0);
}

// 3.a-rest — byte pack/unpack round-trips little-endian, including a top byte (200)
// that sets the int32 sign bit — the `& 255` in unpackByte cleans the arithmetic
// shift's sign-extension so the byte comes back correct.
TEST(GfxBvhCodecTests, bytePackRoundTrip) {
    EXPECT_EQ(runI32(IMP,
        "        int32 w = BvhCodec.packBytes4(10, 20, 30, 200);\n"
        "        if (BvhCodec.unpackByte(w, 0) != 10) { return -1; }\n"
        "        if (BvhCodec.unpackByte(w, 1) != 20) { return -2; }\n"
        "        if (BvhCodec.unpackByte(w, 2) != 30) { return -3; }\n"
        "        if (BvhCodec.unpackByte(w, 3) != 200) { return -4; }\n"
        // the packed word rides a float slot via reinterpret and survives
        "        float32 slot = BvhCodec.packInt(w);\n"
        "        int32 w2 = BvhCodec.unpackInt(slot);\n"
        "        if (BvhCodec.unpackByte(w2, 3) != 200) { return -5; }\n"
        "        if (BvhCodec.unpackByte(w2, 0) != 10) { return -6; }\n"
        "        return 0;\n"), 0);
}
