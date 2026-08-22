//
// simd-fused-integer-madd Unit 1 — dotAccum: four adjacent int8 pairs
// multiplied, summed, accumulated into the matching int32 lane.
//
// The generalization of the DP4a `dot`, keeping the result in VECTOR space.
// cajeta-llama 15.1.18(a) measured that reduce frequency, not vector width, is
// what costs in quantized kernels: the same Q4_K kernel reducing per sub-block
// ran 70.7 ms against 24.2 ms reducing once per block. A partial reduction
// that stays in lanes is the whole point.
//
// Tiers: x86 VNNI `vpdpbusd` -> the portable `llvm.vector.partial.reduce.add`
// -> a scalar lane loop. Callers never branch on target.
//

#include "gtest/gtest.h"
#include "../jit/JitTestHelper.h"

#include "llvm/TargetParser/Host.h"

#include <cstdint>
#include <cstdlib>
#include <string>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src, bool captureIr = false,
               std::string* irOut = nullptr, const std::string& cpu = "") {
    CajetaJit::Options o;
    o.captureIr = captureIr;
    o.cpu = cpu;
    auto jit = CajetaJit::compile(src, "test.D", o);
    if (irOut) *irOut = jit->getModuleIr();
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// True when THIS machine implements an int8 dot-accumulate. The VNNI test
// compiles for the host CPU and then RUNS the result, so it needs the real
// instruction; a machine without it reports a skip rather than a pass.
// A NAMED VNNI cpu this host can actually execute. Named CPUs are the point:
// their TargetMachine carries an EMPTY explicit feature string (measured:
// znver4 / znver5 / cascadelake all report empty), which is exactly the case a
// feature-string search gets wrong.
const char* namedVnniCpu() {
    llvm::StringRef host = llvm::sys::getHostCPUName();
    if (host.starts_with("znver")) return "znver4";
    return "cascadelake";
}

bool hostHasVnni() {
    for (const auto& f : llvm::sys::getHostCPUFeatures()) {
        if (!f.second) continue;
        if (f.first() == "avx512vnni" || f.first() == "avxvnni") return true;
    }
    return false;
}

// Weights in the Q4_K nibble range against activations spanning the signed
// byte range, and a NON-ZERO incoming accumulator — a zero accumulator would
// hide an operand that is dropped rather than added into.
const char* DOTACC =
    "package test;\n"
    "public final class D {\n"
    "    public static int32 run() {\n"
    "        uint8[] w #= heap uint8[64];\n"
    "        int8[] a #= heap int8[64];\n"
    "        int32 i = 0;\n"
    "        while (i < 64) {\n"
    "            w[i] = (uint8) ((i * 5) % 16);\n"
    "            a[i] = (int8) (((i * 37) % 255) - 127);\n"
    "            i = i + 1;\n"
    "        }\n"
    "        int32[] seed #= heap int32[16];\n"
    "        i = 0;\n"
    "        while (i < 16) { seed[i] = i * 3 - 20; i = i + 1; }\n"
    "        Vector<uint8,64> wv = w.vload<64>(0);\n"
    "        Vector<int8,64> av = a.vload<64>(0);\n"
    "        Vector<int32,16> acc = seed.vload<16>(0);\n"
    "        Vector<int32,16> r = wv.dotAccum(av, acc);\n"
    "        int32 lane = 0;\n"
    "        while (lane < 16) {\n"
    "            int32 want = seed[lane];\n"
    "            int32 k = 0;\n"
    "            while (k < 4) {\n"
    "                want = want + (int32) w[lane * 4 + k]\n"
    "                             * (int32) a[lane * 4 + k];\n"
    "                k = k + 1;\n"
    "            }\n"
    "            if (r[lane] != want) { return 100 + lane; }\n"
    "            lane = lane + 1;\n"
    "        }\n"
    "        return 1;\n"
    "    }\n"
    "}\n";

// 1.1.4 — the floor `dot` (Unit 3) is built on: a zero accumulator, and an
// all-zero weight vector against non-zero activations. Both must come back
// exactly zero, and the zero accumulator must not be confused with "no
// accumulator" by any tier.
const char* DOTACC_ZERO =
    "package test;\n"
    "public final class D {\n"
    "    public static int32 run() {\n"
    "        uint8[] w #= heap uint8[64];\n"
    "        int8[] a #= heap int8[64];\n"
    "        int32[] z #= heap int32[16];\n"
    "        int32 i = 0;\n"
    "        while (i < 64) {\n"
    "            w[i] = (uint8) 0;\n"
    "            a[i] = (int8) (((i * 37) % 255) - 127);\n"
    "            i = i + 1;\n"
    "        }\n"
    "        i = 0;\n"
    "        while (i < 16) { z[i] = 0; i = i + 1; }\n"
    "        Vector<int32,16> zero = z.vload<16>(0);\n"
    "        Vector<int32,16> r = w.vload<64>(0).dotAccum(a.vload<64>(0), zero);\n"
    "        int32 lane = 0;\n"
    "        while (lane < 16) {\n"
    "            if (r[lane] != 0) { return 200 + lane; }\n"
    "            lane = lane + 1;\n"
    "        }\n"
    "        // Non-zero weights against a zero accumulator: the products must\n"
    "        // land, so an all-zero result here would mean the operands were\n"
    "        // dropped rather than the arithmetic being right above.\n"
    "        i = 0;\n"
    "        while (i < 64) { w[i] = (uint8) 3; i = i + 1; }\n"
    "        Vector<int32,16> r2 = w.vload<64>(0).dotAccum(a.vload<64>(0), zero);\n"
    "        lane = 0;\n"
    "        while (lane < 16) {\n"
    "            int32 want = 0;\n"
    "            int32 k = 0;\n"
    "            while (k < 4) {\n"
    "                want = want + 3 * (int32) a[lane * 4 + k];\n"
    "                k = k + 1;\n"
    "            }\n"
    "            if (r2[lane] != want) { return 300 + lane; }\n"
    "            lane = lane + 1;\n"
    "        }\n"
    "        return 1;\n"
    "    }\n"
    "}\n";

// 3.1.1 — `dot` and `dotAccum` are the same operation. dot(w4, a4) collapses
// one 4-lane group to a scalar; dotAccum keeps N of them in lanes. Lane k of
// dotAccum over a zero accumulator must equal dot() of the k-th group, or the
// two spellings mean different things and Unit 3's rewrite is unsound.
const char* DOT_AGREES =
    "package test;\n"
    "public final class D {\n"
    "    public static int32 run() {\n"
    "        int8[] w #= heap int8[64];\n"
    "        int8[] a #= heap int8[64];\n"
    "        int32[] z #= heap int32[16];\n"
    "        int32 i = 0;\n"
    "        while (i < 64) {\n"
    "            w[i] = (int8) ((i * 5) % 16);\n"
    "            a[i] = (int8) (((i * 37) % 255) - 127);\n"
    "            i = i + 1;\n"
    "        }\n"
    "        i = 0;\n"
    "        while (i < 16) { z[i] = 0; i = i + 1; }\n"
    "        Vector<int32,16> r =\n"
    "            w.vload<64>(0).dotAccum(a.vload<64>(0), z.vload<16>(0));\n"
    "        int32 lane = 0;\n"
    "        while (lane < 16) {\n"
    "            Vector<int8,4> wg = w.vload<4>((int64) (lane * 4));\n"
    "            Vector<int8,4>  ag = a.vload<4>((int64) (lane * 4));\n"
    "            if (r[lane] != wg.dot(ag)) { return 400 + lane; }\n"
    "            lane = lane + 1;\n"
    "        }\n"
    "        return 1;\n"
    "    }\n"
    "}\n";

// 3.1.1, the other half. The two spellings deliberately DIFFER when the
// receiver is unsigned, and that must be pinned rather than left to be
// discovered. `dot` takes BOTH operands' signedness from the receiver
// (OpSDot / OpUDot are same-sign instructions), so uint8.dot(int8) reads the
// activations as unsigned. `dotAccum` is the mixed unsigned x signed shape
// every ISA chose for quantized work (vpdpbusd, usdot, sudot4, vqdotsu — spec
// §4.3/§4.5): weights take the receiver's signedness, activations are always
// signed.
//
// Lane 0 here is w = [0,5,10,15] against a = [-127,-90,-53,-16]:
//   dotAccum  0*-127 + 5*-90 + 10*-53 + 15*-16          = -1220
//   dot       the same bytes read unsigned, 129/166/203/240 = 6460
const char* DOT_DIFFERS =
    "package test;\n"
    "public final class D {\n"
    "    public static int32 run() {\n"
    "        uint8[] w #= heap uint8[64];\n"
    "        int8[] a #= heap int8[64];\n"
    "        int32[] z #= heap int32[16];\n"
    "        int32 i = 0;\n"
    "        while (i < 64) {\n"
    "            w[i] = (uint8) ((i * 5) % 16);\n"
    "            a[i] = (int8) (((i * 37) % 255) - 127);\n"
    "            i = i + 1;\n"
    "        }\n"
    "        i = 0;\n"
    "        while (i < 16) { z[i] = 0; i = i + 1; }\n"
    "        Vector<int32,16> r =\n"
    "            w.vload<64>(0).dotAccum(a.vload<64>(0), z.vload<16>(0));\n"
    "        if (r[0] != -1220) { return 500; }\n"
    "        Vector<uint8,4> wg = w.vload<4>(0);\n"
    "        Vector<int8,4>  ag = a.vload<4>(0);\n"
    "        if (wg.dot(ag) != 6460) { return 501; }\n"
    "        return 1;\n"
    "    }\n"
    "}\n";

}  // namespace

// 1.1.1 — matches a scalar reference, with a non-zero accumulator.
TEST(VectorDotAccumTests, matchesScalarReference) {
    EXPECT_EQ(runI32(DOTACC), 1);
}

// 1.1.2 — the forced scalar fallback is BYTE-IDENTICAL, not merely close.
// Integer exactness makes this an equality assertion: there is no
// reassociation hazard of the kind float accumulation has, so taking a faster
// tier can change speed and never the answer.
TEST(VectorDotAccumTests, scalarFallbackIsBitIdentical) {
    setenv("CAJETA_SIMD_SCALAR_FALLBACK", "1", 1);
    int32_t r = runI32(DOTACC);
    unsetenv("CAJETA_SIMD_SCALAR_FALLBACK");
    EXPECT_EQ(r, 1);
}

// 1.2.1 — the portable tier is what a GENERIC target gets, and it must be the
// partial reduction rather than a widen-multiply chain. The negative half
// matters as much as the positive: this is the case where VNNI must NOT be
// reached for, so a tier check that always answered "yes" would fail here.
//
// This caught a real defect: llvm.vector.partial.reduce.add reduces STRIDED
// (result[i] = acc[i] + in[i] + in[i+N] + in[i+2N] + in[i+3N]) while vpdpbusd
// and the scalar tier sum lanes 4i..4i+3. Same operation count, same result
// shape, different lane mapping — every dimension checks out and only the
// values differ. The kernel deinterleaves first. Without the bit-identical
// requirement above, that would have shipped.
TEST(VectorDotAccumTests, genericTargetUsesThePortablePartialReduction) {
    std::string ir;
    EXPECT_EQ(runI32(DOTACC, true, &ir), 1);
    EXPECT_NE(ir.find("vector.partial.reduce.add"), std::string::npos)
        << "expected the portable partial reduction on a generic target";
    EXPECT_EQ(ir.find("vpdpbusd"), std::string::npos)
        << "generic x86-64 does not have VNNI; emitting it would not run";
}

// 1.1.3 — on a VNNI host the lowering IS `vpdpbusd`, asserted in the emitted
// IR the way 17.1.2 asserts `pshufb`.
//
// This is the check that turns a silent deoptimization into a failure. It
// already caught one: targetHasIntDotAccum() read
// TargetMachine::getTargetFeatureString(), which returns only the features
// passed in EXPLICITLY — empty for a named CPU like `--cpu=znver5`, even
// though that CPU implies VNNI. Every named-CPU build would have dropped to
// the portable tier and reported a clean run. It asks the subtarget now.
//
// Skips rather than passes on a machine without the instruction: the test
// compiles for the host CPU and then RUNS the result, so a silent pass here
// would be a check that verified nothing.
TEST(VectorDotAccumTests, vnniHostEmitsVpdpbusd) {
    if (!hostHasVnni()) {
        GTEST_SKIP() << "host has neither avx512vnni nor avxvnni; "
                        "the VNNI tier cannot be exercised here";
    }
    std::string ir;
    EXPECT_EQ(runI32(DOTACC, true, &ir, "native"), 1);
    EXPECT_NE(ir.find("vpdpbusd"), std::string::npos)
        << "expected the VNNI intrinsic in the emitted IR on a VNNI host";
}

// 1.1.3 — and through a NAMED cpu, which is where the tier check actually
// broke. `--cpu=native` expands the host's full feature list into the
// TargetMachine's explicit feature string, so a string search over it finds
// VNNI and the test above passes either way. A named cpu carries an EMPTY
// feature string while still IMPLYING vnni (measured: znver4, znver5 and
// cascadelake all report empty), so the string search answered "no" and every
// such build silently took the portable tier — a deoptimization that reads as
// a clean run. This is the assertion that fails on that.
TEST(VectorDotAccumTests, namedCpuStillReachesTheVnniTier) {
    if (!hostHasVnni()) {
        GTEST_SKIP() << "host has neither avx512vnni nor avxvnni; "
                        "the VNNI tier cannot be exercised here";
    }
    std::string ir;
    EXPECT_EQ(runI32(DOTACC, true, &ir, namedVnniCpu()), 1);
    EXPECT_NE(ir.find("vpdpbusd"), std::string::npos)
        << "a named VNNI cpu (" << namedVnniCpu() << ") must reach the VNNI "
           "tier; its explicit feature string is empty, so the tier check has "
           "to ask the subtarget";
}

// 3.1.1 — the two spellings are one operation.
TEST(VectorDotAccumTests, dotIsDotAccumOverAZeroAccumulator) {
    EXPECT_EQ(runI32(DOT_AGREES), 1);
}

// 3.1.1 — and the mixed-sign case, where they deliberately differ.
TEST(VectorDotAccumTests, dotAndDotAccumDifferOnAnUnsignedReceiver) {
    EXPECT_EQ(runI32(DOT_DIFFERS), 1);
}

// 2.1.1 — the pre-VNNI x86 tier agrees bit-for-bit with the VNNI one.
//
// haswell is AVX2 with no VNNI, and its ISA is a subset of every VNNI x86 cpu,
// so it compiles AND runs here. The bit-identity is the whole claim: the
// sequence llama.cpp uses (`vpmaddubsw`) SATURATES — 255 x -128 twice is
// -65280, which clamps to -32768 — so it is exact only within the quantized
// range, and `dotAccum` is public surface over any `Vector<uint8,4N>`. The
// exact widen-multiply-reduce form is what keeps §4.8 true (spec §4.11.1).
TEST(VectorDotAccumTests, preVnniX86IsBitIdentical) {
    std::string ir;
    EXPECT_EQ(runI32(DOTACC, true, &ir, "haswell"), 1);
    EXPECT_EQ(ir.find("vpdpbusd"), std::string::npos)
        << "haswell has no VNNI; emitting it would not run";
    EXPECT_NE(ir.find("pmadd"), std::string::npos)
        << "expected the pre-VNNI pmadd reduce, not the portable partial "
           "reduction — measured at 57 instructions against 3 on haswell";
}

// 1.1.4 — the floor: a zero accumulator, and all-zero weights. `dot` (Unit 3)
// is defined as dotAccum(w, a, zeros), so this is the case it rests on.
TEST(VectorDotAccumTests, zeroAccumulatorAndZeroWeights) {
    EXPECT_EQ(runI32(DOTACC_ZERO), 1);
}

// 1.1.4 / 1.1.2 — and the same floor through the scalar tier, since a zero
// accumulator is the input most likely to be special-cased away.
TEST(VectorDotAccumTests, zeroFloorIsBitIdenticalOnTheScalarTier) {
    setenv("CAJETA_SIMD_SCALAR_FALLBACK", "1", 1);
    int32_t r = runI32(DOTACC_ZERO);
    unsetenv("CAJETA_SIMD_SCALAR_FALLBACK");
    EXPECT_EQ(r, 1);
}
