//
// VectorSimdLadderTests — cajeta-llama plan Unit 17 (17.1.1–17.1.5):
// tableLookup / widen / narrow / convert / bitcast on Vector<T,N>, the
// quantized-weight unpacking toolkit Unit 18 consumes.
//
// 17.1.6 (wave → AVX-512/AVX2 lanes on the CPU backend) is ALREADY pinned
// by XpuCpuWaveSimdTests — width 16/8/4 by detected ISA, masked divergent
// variants — and is not repeated here.
//
// The pshufb lowering (17.1.2) is asserted against the EMITTED IR via the
// harness's captureIr, and the scalar fallback is the same entry point
// under CAJETA_SIMD_SCALAR_FALLBACK=1 — compiled both ways, byte-identical
// results, and the intrinsic verifiably absent from the fallback's IR.
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "../PortableEnv.h"   // setenv/unsetenv are POSIX; MinGW has neither

#include <cstdint>
#include <cstdlib>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src, bool captureIr = false,
               std::string* irOut = nullptr) {
    CajetaJit::Options o;
    o.captureIr = captureIr;
    auto jit = CajetaJit::compile(src, "test.D", o);
    if (irOut) *irOut = jit->getModuleIr();
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// The classifier program (17.1.1): a 16-entry low-nibble class table over a
// JSON-ish byte line, tableLookup vs an in-language scalar reference
// implementing pshufb's exact contract (high bit set → 0, else table[b&15]).
// Returns 1 on full agreement, else 100+lane.
const char* CLASSIFIER =
    "package test;\n"
    "public final class D {\n"
    "    public static int32 run() {\n"
    "        int8[] tbl = heap int8[16];\n"
    "        tbl[0xB] = (int8) 1;\n"     // '{' 0x7B, '[' 0x5B → class 1
    "        tbl[0xD] = (int8) 2;\n"     // '}' 0x7D, ']' 0x5D → class 2
    "        tbl[0xA] = (int8) 3;\n"     // ':' 0x3A → class 3
    "        tbl[0xC] = (int8) 4;\n"     // ',' 0x2C → class 4
    "        int8[] line = heap int8[16];\n"
    "        line[0] = (int8) 0x7B; line[1] = (int8) 0x22;\n"
    "        line[2] = (int8) 0x61; line[3] = (int8) 0x22;\n"
    "        line[4] = (int8) 0x3A; line[5] = (int8) 0x5B;\n"
    "        line[6] = (int8) 0x31; line[7] = (int8) 0x2C;\n"
    "        line[8] = (int8) 0x32; line[9] = (int8) 0x5D;\n"
    "        line[10] = (int8) 0x7D; line[11] = (int8) 0x20;\n"
    "        line[12] = (int8) 0x9B;\n"  // high bit set → class 0, not table[0xB]
    "        line[13] = (int8) 0xFF;\n"  // high bit set → 0
    "        line[14] = (int8) 0x0B;\n"  // low control byte → table[0xB]
    "        line[15] = (int8) 0x00;\n"
    "        Vector<int8,16> table = Cajeta.vload16(tbl, 0);\n"
    "        Vector<int8,16> bytes = Cajeta.vload16(line, 0);\n"
    "        Vector<int8,16> cls = table.tableLookup(bytes);\n"
    "        int32 i = 0;\n"
    "        while (i < 16) {\n"
    "            int32 b = (int32) line[i] & 255;\n"
    "            int32 want = 0;\n"
    "            if (b < 128) { want = (int32) tbl[b & 15]; }\n"
    "            if ((int32) cls[i] != want) { return 100 + i; }\n"
    "            i = i + 1;\n"
    "        }\n"
    "        return 1;\n"
    "    }\n"
    "}\n";

} // namespace

// 17.1.1 — tableLookup reproduces the scalar 16-entry table classifier.
TEST(VectorSimdLadderTests, tableLookupReproducesScalarClassifier) {
    EXPECT_EQ(runI32(CLASSIFIER), 1);
}

// 17.1.2 — on x86 the lowering IS pshufb (asserted in the emitted IR), and
// the forced scalar fallback produces identical results with the intrinsic
// verifiably absent.
TEST(VectorSimdLadderTests, tableLookupLowersToPshufbAndFallbackAgrees) {
    std::string irFast;
    EXPECT_EQ(runI32(CLASSIFIER, true, &irFast), 1);
#if defined(__x86_64__) || defined(_M_X64)
    EXPECT_NE(irFast.find("ssse3.pshuf.b"), std::string::npos)
        << "expected the pshufb intrinsic in the emitted IR";
#endif
    setenv("CAJETA_SIMD_SCALAR_FALLBACK", "1", 1);
    std::string irSlow;
    int32_t r = runI32(CLASSIFIER, true, &irSlow);
    unsetenv("CAJETA_SIMD_SCALAR_FALLBACK");
    EXPECT_EQ(r, 1);
    EXPECT_EQ(irSlow.find("ssse3.pshuf.b"), std::string::npos)
        << "the forced fallback must not emit the intrinsic";
}

// 17.1.3 — widen and narrow round-trip across the integer ladder, sext
// semantics checked against scalar, including the 1-lane boundary
// (Vector<int64,2>.widenLo() would leave the ladder; int32x4→int64x2 is the
// last rung and its 2→1 widen is the lane-count boundary exercised here).
TEST(VectorSimdLadderTests, widenNarrowRoundTripAcrossTheLadder) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] b = heap int8[16];\n"
        "        int32 i = 0;\n"
        "        while (i < 16) { b[i] = (int8) (i - 8); i = i + 1; }\n"
        "        Vector<int8,16> v8 = Cajeta.vload16(b, 0);\n"
        // one rung up: lanes must SEXT (negative values survive).
        "        Vector<int16,8> lo16 = v8.widenLo();\n"
        "        Vector<int16,8> hi16 = v8.widenHi();\n"
        "        i = 0;\n"
        "        while (i < 8) {\n"
        "            if ((int32) lo16[i] != i - 8) { return 100 + i; }\n"
        "            if ((int32) hi16[i] != i) { return 200 + i; }\n"
        "            i = i + 1;\n"
        "        }\n"
        // continue the ladder to int64x2 (the boundary rung)…
        "        Vector<int32,4> lo32 = lo16.widenLo();\n"
        "        Vector<int64,2> lo64 = lo32.widenLo();\n"
        "        if (lo64[0] != -8L) { return 300; }\n"
        "        if (lo64[1] != -7L) { return 301; }\n"
        // …and back down, narrowing pairwise; the full 16 lanes must
        // reassemble bit-exact.
        "        Vector<int8,16> back = lo16.narrow(hi16);\n"
        "        i = 0;\n"
        "        while (i < 16) {\n"
        "            if ((int32) back[i] != i - 8) { return 400 + i; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 17.1.4 — int↔float convert and bitcast match their scalar twins.
TEST(VectorSimdLadderTests, convertAndBitcastMatchScalar) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        Vector<int32,4> vi = heap Vector<int32,4>(-7, 0, 3, 1065353216);\n"
        "        Vector<float32,4> vf = vi.toF32();\n"
        "        if (vf[0] != -7.0f) { return 100; }\n"
        "        if (vf[1] != 0.0f) { return 101; }\n"
        "        if (vf[2] != 3.0f) { return 102; }\n"
        "        Vector<int32,4> rt = vf.toI32();\n"
        "        if (rt[0] != -7) { return 103; }\n"
        // bitcast: 1065353216 == 0x3F800000 == 1.0f — the scalar
        // Cajeta.bitsToF32 contract, lane-wise.
        "        Vector<float32,4> bits = vi.bitcastF32();\n"
        "        if (bits[3] != 1.0f) { return 104; }\n"
        "        Vector<int32,4> bi = bits.bitcastI32();\n"
        "        if (bi[3] != 1065353216) { return 105; }\n"
        "        if (bi[0] != vi[0]) { return 106; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 17.1.5 — the actual Unit-18 consumer: a vectorized Q4_0 nibble unpack
// (lo/hi nibble split, -8 bias, widen to int16) matches the scalar unpack
// bit for bit over a 16-byte block of every byte pattern class.
TEST(VectorSimdLadderTests, vectorizedQ4NibbleUnpackMatchesScalar) {
    std::string src =
        "package test;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int8[] blk = heap int8[16];\n"
        "        int32 i = 0;\n"
        "        while (i < 16) { blk[i] = (int8) (i * 17 + 3); i = i + 1; }\n"
        "        Vector<int8,16> v = Cajeta.vload16(blk, 0);\n"
        "        Vector<int8,16> lo = (v & 15) - 8;\n"
        "        Vector<int8,16> hi = ((v >> 4) & 15) - 8;\n"
        "        i = 0;\n"
        "        while (i < 16) {\n"
        "            int32 b = (int32) blk[i] & 255;\n"
        "            int32 wantLo = (b & 15) - 8;\n"
        "            int32 wantHi = ((b >> 4) & 15) - 8;\n"
        "            if ((int32) lo[i] != wantLo) { return 100 + i; }\n"
        "            if ((int32) hi[i] != wantHi) { return 200 + i; }\n"
        "            i = i + 1;\n"
        "        }\n"
        // and the widened halves Unit 18 multiplies at int16:
        "        Vector<int16,8> w = lo.widenLo();\n"
        "        if ((int32) w[0] != ((int32) blk[0] & 15) - 8) { return 300; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 17.3.1 — the microbenchmark record: vectorized Q4_0 nibble unpack vs the
// scalar unpack over 100k blocks (1.6 MB). DISABLED_: it is a MEASUREMENT
// for the reference machine (run explicitly with
// --gtest_also_run_disabled_tests), not a CI gate — timing gates flake.
// The measured margin is recorded in the cajeta-llama plan at 17.3.1.
// Returns (scalarNs * 10) / vectorNs — 25 means 2.5x.
TEST(VectorSimdLadderTests, DISABLED_q4UnpackMicrobenchMargin) {
    std::string src =
        "package test;\n"
        "import cajeta.time.Clock;\n"
        "public final class D {\n"
        "    static int64 scalarPass(int8[] buf, int32 n, int32[] sink) {\n"
        "        int64 t0 = Clock.nanoTime();\n"
        "        int32 acc = 0;\n"
        "        int32 i = 0;\n"
        "        while (i < n) {\n"
        "            int32 b = (int32) buf[i] & 255;\n"
        "            acc = acc + ((b & 15) - 8) + (((b >> 4) & 15) - 8);\n"
        "            i = i + 1;\n"
        "        }\n"
        "        sink[0] = acc;\n"
        "        return Clock.nanoTime() - t0;\n"
        "    }\n"
        "    static int64 vectorPass(int8[] buf, int32 n, int32[] sink) {\n"
        "        int64 t0 = Clock.nanoTime();\n"
        "        Vector<int8,16> accv = Cajeta.vload16(buf, 0) & 0;\n"
        "        int32 i = 0;\n"
        "        while (i < n) {\n"
        "            Vector<int8,16> v = Cajeta.vload16(buf, (int64) i);\n"
        "            Vector<int8,16> lo = (v & 15) - 8;\n"
        "            Vector<int8,16> hi = ((v >> 4) & 15) - 8;\n"
        "            accv = accv + lo + hi;\n"
        "            i = i + 16;\n"
        "        }\n"
        "        int32 acc = 0;\n"
        "        i = 0;\n"
        "        while (i < 16) { acc = acc + (int32) accv[i]; i = i + 1; }\n"
        "        sink[0] = acc;\n"
        "        return Clock.nanoTime() - t0;\n"
        "    }\n"
        "    public static int32 run() {\n"
        "        int32 n = 1600000;\n"
        "        int8[] buf = heap int8[n];\n"
        "        int32 i = 0;\n"
        "        while (i < n) { buf[i] = (int8) (i * 31 + 7); i = i + 1; }\n"
        "        int32[] sa = heap int32[1];\n"
        "        int32[] sb = heap int32[1];\n"
        // warm both, then best-of-5 each.
        "        D.scalarPass(buf, n, sa);\n"
        "        D.vectorPass(buf, n, sb);\n"
        "        int64 bs = 9223372036854775807L;\n"
        "        int64 bv = 9223372036854775807L;\n"
        "        i = 0;\n"
        "        while (i < 5) {\n"
        "            int64 ts = D.scalarPass(buf, n, sa);\n"
        "            int64 tv = D.vectorPass(buf, n, sb);\n"
        "            if (ts < bs) { bs = ts; }\n"
        "            if (tv < bv) { bv = tv; }\n"
        "            i = i + 1;\n"
        "        }\n"
        // vector sums lanes mod 256 per lane — compare mod 256 of the
        // scalar sum to keep the two passes honest about doing the work.
        "        if (((sa[0] - sb[0]) & 255) != 0 && ((sb[0] - sa[0]) & 255) != 0) {\n"
        "            return -1;\n"
        "        }\n"
        "        return (int32) ((bs * 10L) / bv);\n"
        "    }\n"
        "}\n";
    int32_t margin10 = runI32(src);
    ASSERT_GT(margin10, 0);
    printf("[17.3.1] vector nibble unpack margin: %.1fx (best-of-5, 1.6MB)\n",
           margin10 / 10.0);
    // The record lives in the plan; here only sanity — vector not slower.
    EXPECT_GE(margin10, 10);
}
