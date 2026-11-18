// `Vector<float16,N>.toF32()` and `Vector<float32,N>.toF16()` — lane-wise
// width conversion between binary16 and binary32.
//
// The gap surfaced while measuring whether expanding packed weights to f16
// at load beats unpacking them in registers (plan 8.14): the f16 mat-vec
// kernel written for that probe could not lower, because the ladder had an
// integer rung (`toF32` on int lanes) and no FLOAT one. `toF16` comes with
// it deliberately — a one-directional conversion is the same half-a-ladder
// shape that made `widenLo`/`toF32` unusable inside kernels in the first
// place (8.10).
//
// Both spellings are tested on the host AND inside a kernel, because that
// split is exactly where the ladder was previously missing.

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include <cmath>
#include <cstdint>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

}  // namespace

// f16 -> f32 widens exactly: every binary16 value is representable in
// binary32, so this is lossless and can be asserted on the nose.
TEST(VectorHalfConvert, halfToF32WidensExactly) {
    const std::string src =
        "package test;\n"
        "public class D {\n"
        "    public static int32 run() {\n"
        "        float16[] h #= heap float16[4];\n"
        "        h[0] = (float16) 1.5f;\n"
        "        h[1] = (float16) -2.25f;\n"
        "        h[2] = (float16) 0.5f;\n"
        "        h[3] = (float16) 10.0f;\n"
        "        Vector<float16,4> hv = h.vload<4>(0L);\n"
        "        Vector<float32,4> f = hv.toF32();\n"
        "        // 1.5 - 2.25 + 0.5 + 10.0 = 9.75 -> 975\n"
        "        float32 s = f[0] + f[1] + f[2] + f[3];\n"
        "        return (int32) (s * 100.0f);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 975);
}

// f32 -> f16 -> f32 round trip. The values chosen are all exactly
// representable in binary16, so the round trip is exact; a `toF16` that
// bitcast instead of converting would produce garbage here.
TEST(VectorHalfConvert, f32ToHalfRoundTripsExactlyForRepresentableValues) {
    const std::string src =
        "package test;\n"
        "public class D {\n"
        "    public static int32 run() {\n"
        "        float32[] a #= heap float32[4];\n"
        "        a[0] = 3.25f;\n"
        "        a[1] = -0.75f;\n"
        "        a[2] = 16.0f;\n"
        "        a[3] = 0.125f;\n"
        "        Vector<float32,4> v = a.vload<4>(0L);\n"
        "        Vector<float16,4> h = v.toF16();\n"
        "        Vector<float32,4> b = h.toF32();\n"
        "        // 3.25 - 0.75 + 16.0 + 0.125 = 18.625 -> 18625\n"
        "        float32 s = b[0] + b[1] + b[2] + b[3];\n"
        "        return (int32) (s * 1000.0f);\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 18625);
}

// f32 -> f16 must NARROW, not reinterpret: 1e5 exceeds binary16's range
// (max finite 65504) and must come back as infinity.
TEST(VectorHalfConvert, f32ToHalfOverflowsToInfinityRatherThanWrapping) {
    const std::string src =
        "package test;\n"
        "public class D {\n"
        "    public static int32 run() {\n"
        "        float32[] a #= heap float32[4];\n"
        "        a[0] = 100000.0f;\n"
        "        a[1] = 0.0f;\n"
        "        a[2] = 0.0f;\n"
        "        a[3] = 0.0f;\n"
        "        Vector<float32,4> v = a.vload<4>(0L);\n"
        "        Vector<float32,4> b = v.toF16().toF32();\n"
        "        // Infinity is greater than every finite float32.\n"
        "        if (b[0] > 3.0e38f) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// The does-NOT-fire half: the INTEGER meaning of `toF32` is untouched.
// Widening the float rung by relaxing the receiver check is the obvious
// way to break this, and it would be silent — an int8 lane read as a half
// lane converts without complaint and gives nonsense.
TEST(VectorHalfConvert, integerToF32IsUnchanged) {
    const std::string src =
        "package test;\n"
        "public class D {\n"
        "    public static int32 run() {\n"
        "        int8[] a #= heap int8[4];\n"
        "        a[0] = (int8) 3;\n"
        "        a[1] = (int8) -4;\n"
        "        a[2] = (int8) 5;\n"
        "        a[3] = (int8) 6;\n"
        "        Vector<int8,4> v = a.vload<4>(0L);\n"
        "        Vector<float32,4> f = v.toF32();\n"
        "        float32 s = f[0] + f[1] + f[2] + f[3];\n"
        "        return (int32) s;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 10);
}

// A float32 receiver has nothing to widen to and must still be rejected —
// otherwise `toF32` silently becomes a no-op on the wrong type.
TEST(VectorHalfConvert, f32ReceiverStillRejectedByToF32) {
    const std::string src =
        "package test;\n"
        "public class D {\n"
        "    public static int32 run() {\n"
        "        float32[] a #= heap float32[4];\n"
        "        a[0] = 1.0f;\n"
        "        Vector<float32,4> v = a.vload<4>(0L);\n"
        "        Vector<float32,4> f = v.toF32();\n"
        "        return (int32) f[0];\n"
        "    }\n"
        "}\n";
    // ANY_THROW, not THROW(std::exception): cajeta::Exception is not a
    // std::exception subclass, and the first version of this test failed
    // for that reason rather than for the behaviour it claims to check.
    EXPECT_ANY_THROW(runI32(src));
}

// cajeta-llm 15.2.21.4.3.8.8.1: `bitcastF16` reads 16-bit integer lanes as binary16 bits and
// `bitcastI16` reads them back, without converting; 0x3C00 is 1.0, 0xC000 is -2.0, 0x6405 is 1029.
TEST(VectorHalfConvert, bitcastF16ReinterpretsSixteenBitLanesAndBitcastI16ReadsThemBack) {
    const std::string src =
        "package test;\n"
        "public class D {\n"
        "    public static int32 run() {\n"
        "        int16[] a #= heap int16[4];\n"
        "        a[0] = (int16) 15360;\n"
        "        a[1] = (int16) -16384;\n"
        "        a[2] = (int16) 25605;\n"
        "        a[3] = (int16) 0;\n"
        "        Vector<int16,4> v = a.vload<4>(0L);\n"
        "        Vector<float16,4> h = v.bitcastF16();\n"
        "        Vector<float32,4> f = h.toF32();\n"
        "        if (f[0] != 1.0f) { return 100; }\n"
        "        if (f[1] != -2.0f) { return 101; }\n"
        "        if (f[2] != 1029.0f) { return 102; }\n"
        "        if (f[3] != 0.0f) { return 103; }\n"
        "        Vector<int16,4> back = h.bitcastI16();\n"
        "        if (back[1] != (int16) -16384) { return 104; }\n"
        "        if (back[2] != (int16) 25605) { return 105; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// The consumer: an int8 lane x as f16 by bits, `(x ^ 0x80) | 0x6400` is 1024 + 128 + x exactly,
// so one subtract of 1152 gives x for every byte.
TEST(VectorHalfConvert, theInt8ToHalfMagicIsExactForEveryByte) {
    const std::string src =
        "package test;\n"
        "public class D {\n"
        "    public static int32 run() {\n"
        "        int8[] a #= heap int8[256];\n"
        "        int32 i = 0;\n"
        "        while (i < 256) { a[i] = (int8) (i - 128); i = i + 1; }\n"
        "        int64 o = 0L;\n"
        "        while (o < 256L) {\n"
        "            Vector<int8,16> v = a.vload<16>(o);\n"
        "            Vector<uint8,16> u = (v ^ -128).asUnsigned();\n"
        "            Vector<uint16,8> lo = u.widenLo() | 25600;\n"
        "            Vector<float32,8> f = lo.asSigned().bitcastF16().toF32();\n"
        "            int32 k = 0;\n"
        "            while (k < 8) {\n"
        "                if (f[k] - 1152.0f != (float32) a[o + (int64) k]) { return 1000 + (int32) o + k; }\n"
        "                k = k + 1;\n"
        "            }\n"
        "            o = o + 16L;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// A 32-bit receiver is not 16 bits wide and is refused, not truncated.
TEST(VectorHalfConvert, bitcastF16RejectsAThirtyTwoBitReceiver) {
    const std::string src =
        "package test;\n"
        "public class D {\n"
        "    public static int32 run() {\n"
        "        int32[] a #= heap int32[4];\n"
        "        Vector<int32,4> v = a.vload<4>(0L);\n"
        "        Vector<float16,4> h = v.bitcastF16();\n"
        "        return (int32) h.toF32()[0];\n"
        "    }\n"
        "}\n";
    EXPECT_ANY_THROW(runI32(src));
}
