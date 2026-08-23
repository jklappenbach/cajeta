// threaded-forward-path plan Unit 2 — DEFECT REPRO, disabled.
//
// `dotAccum` produces a WRONG ANSWER inside a kernel body. Measured
// 2026-08-22 with an AOT probe (`--emit=exe --xpu-backend=cpu`), 8 groups of
// 64 uint8 weights against int8 activations, accumulator seeded from an
// int32 array:
//
//     host path    vpdpbusd            lane0 = -1240   (correct)
//     kernel body  vpmaddwd + vpaddd   lane0 =  6440   (wrong)
//
// 6440 is exactly the sum with the ACTIVATIONS treated as unsigned:
//     5*166 + 10*203 + 15*240 - 20 = 6440     (a[i] read as u8)
//     5*-90 + 10*-53 + 15*-16 - 20 = -1240    (a[i] read as i8, correct)
//
// ROOT CAUSE — the device lowering seam models signedness SYMMETRICALLY and
// cannot express what dotAccum means. KernelLowering.cpp routes dotAccum
// through `LoweringTarget::integerDot4x8(b, m, a, c, acc, bool isSigned)`,
// whose base implementation is `vecops::idotWiden`, and that widens BOTH
// operands with the same flag:
//
//     ai = CreateIntCast(lane(a, i), i32, isSigned)
//     ci = CreateIntCast(lane(c, i), i32, isSigned)
//
// But dotAccum's contract is ASYMMETRIC — unsigned weights x SIGNED
// activations. That asymmetry is the whole point of the instruction family
// it exists to reach (vpdpbusd, usdot, vqdotsu); the spec calls out that two
// ISAs chose it independently because it is the shape quantized inference
// needs. With an unsigned receiver the seam passes isSigned=false and
// zero-extends the activations too.
//
// The host path is correct because it does NOT go through this seam:
// MethodCallExpression passes `wUnsigned` to `vecops::dotAccum`, which
// zext/sext's the weights per that flag and ALWAYS sext's the activations.
//
// BLAST RADIUS — every device backend, not just the CPU one. The Vulkan
// override picks OpSDot/OpUDot and the AMDGPU override picks
// amdgcn_sdot4/udot4 off the same single `isSigned`; both instruction pairs
// are symmetric, so neither can express unsigned x signed either. This was
// found on the CPU backend only because that is the one being routed first.
//
// FIX SHAPE — integerDot4x8 needs two signedness flags (or an explicit
// "mixed" mode), and each backend override needs a mixed-signedness path:
// on AMDGPU that means not sdot4/udot4 but a widen-and-multiply, on Vulkan
// OpSUDot where available. Until then dotAccum must not be used in a kernel.
//
// IMPACT ON THE PLAN — contained. Of cajeta-llama's packed mat-vecs only
// `q4kMatVecIntoQ8` uses dotAccum; the four f32 kernels that `matvecInto`
// actually routes today do not, so Unit 3 proceeds on those (plan 2.3.2) and
// the q8_K routing waits on this fix.
//
// Disabled rather than deleted: it documents a live defect and should be
// enabled by whoever fixes the seam. It cannot be written against the JIT
// harness — CajetaJit does not wire an XPU backend manifest, so a launch
// there silently no-ops ("cajeta.xpu: no available backend among {}") and the
// buffers read back as zero. The repro is an AOT build; see the spec.

#include "gtest/gtest.h"

// Enable when LoweringTarget::integerDot4x8 can express unsigned x signed.
// Needs an AOT harness (see the note above), not CajetaJit.
TEST(XpuCpuDotAccumTierTests,
     DISABLED_kernelBodyDotAccumMustMatchUnsignedTimesSigned) {
    GTEST_SKIP() << "known defect: the device dot seam is symmetric-signedness "
                    "only; dotAccum in a kernel body zero-extends its signed "
                    "operand (measured 6440, want -1240)";
}
