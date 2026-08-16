//
// NumpyOpsTests — numpy-porting-plan Phase 3: creation factories + elementwise
// ufuncs over the Tensor<T> keystone. Reference oracle = numpy semantics.
//
// Phase 2 already provides zeros/ones/full/arange/empty + _like (see TensorTests).
// Phase 3 adds the remaining creators (linspace/eye/meshgrid/arange-range) and the
// ufunc surface. cajeta.math is lazily parsed; importing cajeta.math.Tensor triggers it.
//

#include <gtest/gtest.h>

#include "../jit/JitTestHelper.h"
#include "cajeta/error/Exception.h"
#include "cajeta/xpu/XpuTarget.h"

#include <cstdint>
#include <string>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <cstdlib>
#include <vector>

using cajeta_test::CajetaJit;

namespace {

int32_t runI32(const std::string& src) {
    auto jit = CajetaJit::compile(src, "test.D");
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

// 3c — the GPU-lowering tests need the XPU backend enabled so @Kernel lowers and
// launches (on the portable CPU backend in-process, same discipline as
// TensorTests::runI32Xpu). The Tensor op routes on placement; the kernel runs
// through the real launch FFI either way.
int32_t runI32Xpu(const std::string& src) {
    CajetaJit::Options o;
    o.xpuBackends = {cajeta::xpu::Backend::Cpu};
    auto jit = CajetaJit::compile(src, "test.D", o);
    auto fn = jit->lookup<int32_t (*)()>("run");
    return fn();
}

const char* PRE =
    "package test;\n"
    "import cajeta.math.Tensor;\n";

} // namespace

// 3a — linspace<E>(start, stop, num): `num` evenly spaced samples over [start, stop]
// INCLUSIVE (numpy default endpoint=true). step = (stop-start)/(num-1); value[i] =
// start + i*step; value[num-1] == stop exactly. num==1 → [start].

// 3a — eye<E>(n): n x n identity — 1 on the main diagonal, 0 elsewhere.

// 3a — arange<E>(start, stop, step): half-open [start, stop) in steps of `step`
// (numpy 3-arg arange). count = ceil((stop-start)/step), clamped to >= 0; value[i]
// = start + i*step. Coexists with the 1-arg arange<E>(n) via value-param arity.

// 3a — meshgrid<E>(x, y): coordinate grids (numpy default 'xy' indexing). For x of
// length Nx and y of length Ny returns [X, Y], each shaped (Ny, Nx), with
// X[i,j] = x[j] and Y[i,j] = y[i].

// 3b/3e — elementwise binary arithmetic (add/sub/mul) over the Tensor, same-dtype,
// with right-aligned broadcasting (matches numpy). CPU floor; div/comparison/etc.
// follow in later units (they carry dtype-promotion / bool-result subtleties).

// 1.1.1 — bounded cross-cast kernel: add<A,B,R> on a MIXED dtype pair. Inputs are
// marker-bounded (A,B extends Numeric), the result is an explicit width R; each operand
// is cross-cast (R) and the op performed in R, producing a statically-typed Tensor<R>.
// int32 ⊕ float32 with R=float64 → exact (float64)ai + (float64)bf, no runtime dispatch.

// 1.1.3 — sub/mul in the explicit <A,B,R> form on a mixed pair with R = promote(A,B)
// match numpy values + dtype. int32 ⊕ float32 → float64 (NEP-50 §2.2.5).

// 2.1.1 — comparison family eq/ne/lt/le/gt/ge over same-dtype tensors yields a
// Tensor<boolean> matching numpy, with right-aligned broadcasting (spec §6, 6.2.3).

// 2.1.2 — mixed-dtype comparison compares at the promoted value (explicit compare
// width C) and returns boolean: int32 vs float32 at float64 (spec 6.2.3, 2.2.5).

// 3.1.1 — true division: int/int → float64 (NEP-50 true div, spec 6.2.1); float
// same-dtype div<E extends Floating> stays E (spec 2.2.5).

// 3.1.2 — floor-division (toward −∞) and mod (sign of divisor): numpy //, % rules
// incl. negative operands; ints stay int, floats match np.floor_divide/np.mod (6.2.2).

// 4.1.1 — bitwise and/or/xor/shift on integer tensors match numpy, with promotion to
// the wider result type for mixed-width operands (spec §6 bitwise, Integral domain).

// 4.1.2 — logical and/or/xor/not on boolean tensors match numpy (spec §6 logical).

// 4.1.3 — bitwise op on a floating tensor is a compile error (Integral domain bound,
// spec 6.2.4). Negative test: the JIT compile throws.

// 5.1.1 — unary transcendental/rounding on a float tensor match numpy; neg/abs work on
// any numeric (spec §6 transcendental/rounding). Inputs chosen for exact results.

// 5.1.2 — a transcendental on an integer tensor is a compile error (Floating bound,
// spec 6.2.5). neg/abs are NOT rejected (Numeric domain).
TEST(NumpyOpsTests, unaryDomainRejectsInt) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int32[] da = [ 1, 2 ];\n"
        "        int64[] s2 = heap int64[1]; s2[0] = 2;\n"
        "        Tensor<int32> a #= Tensor.of<int32>(da, s2);\n"
        "        Tensor<int32> r #= Tensor.sin<int32>(a);\n"                // int32 ∉ Floating
        "        return r.size() > 0 ? 1 : 0;\n"
        "    }\n"
        "}\n";
    EXPECT_THROW(runI32(src), cajeta::Exception);
}

// 6.1.1 — where(cond, a, b) selects elementwise with broadcasting; result dtype is the
// explicit promote(A,B) width (spec §6 where).

// 6.1.2 — clip(t, lo, hi) clamps elementwise, keeping t's dtype (spec §6 clip).

const char* PRE7 =
    "package test;\n"
    "import cajeta.math.Tensor;\n"
    "import cajeta.lang.Numeric;\n"
    "import cajeta.lang.Floating;\n";

// 7.1.1 — auto-promote add (no explicit R): mixed int32/float32 returns a bounded
// wildcard Tensor<? extends Numeric> whose runtime dtype is promote(int32,float32)=
// float64, capturable as (Tensor<float64>) r (spec 5.2.1).

// 7.1.2 — auto-promote div is always floating: int/int returns Tensor<? extends
// Floating> with runtime dtype float64 (spec 5.2.2).

// 7.1.3 — the bounded-wildcard result discriminates dtype cleanly via the reified
// airlock: a wrong-dtype instanceof is false (no UB), only the true dtype matches
// (spec 5.2.4).

// 8.1.1 — weak scalar of same-or-lower category adopts the tensor's dtype (NEP-50,
// spec 7.2.1/7.2.2): int8 + 5 → Tensor<int8> (value irrelevant, no widening); float32 +
// 1.0 → Tensor<float32> (not float64). Static result types confirm no widening.

// 8.1.2 — a weak float scalar over an int tensor bumps to the default float (float64):
// int32 + 2.0 → Tensor<float64> (NEP-50 category bump, spec 7.2.3). Static Tensor<float64>
// result type confirms the bump.

// 3c — elementwiseCpuGpuAgree: the stdlib elementwise lowering (cajeta.math.Ewise)
// routes on placement — both operands on-device → the float32 GPU @Kernel; host
// operands → the CPU loop — and the two paths agree (the cross-check). Proven over
// the full arithmetic family (add/sub/mul/div) against a host oracle. Values chosen
// so every op is exact in float32, so agreement is bit-exact. Runs on the cajeta.xpu
// CPU backend in-process (no GPU required); on-device validation rides device gates.

// 4a — full-array reductions to a scalar (numpy a.sum()/prod()/min()/max()/mean()
// with no axis). Explicit accumulator dtype R (numpy dtype= kwarg); walks the full
// multi-index so it is correct for n-D tensors. Values chosen exact.

// 4a — axis reductions (numpy a.sum(axis=)/prod/min/max/mean, keepdims + negative
// axis). keepdims=false removes the axis (ndim-1); keepdims=true keeps it as size 1.

// 4a — index/predicate reductions: argmin/argmax (C-order flat index, first on ties),
// count_nonzero, any/all (numeric truthiness), and boolean-mask anyTrue/allTrue.

// 4a — var/std (with ddof) and nan-variants. Textbook set [2,4,4,4,5,5,7,9]:
// mean 5, Σ(x-μ)²=32, var(ddof=0)=4, std=2 (both exact in float32). nansum/nanmean
// skip NaN; a plain sum over NaN data is NaN.

// 4b — scans: cumsum/cumprod flattened (numpy cumsum(a)) and along an axis
// (cumsum(a, axis=)). Flattened forms return 1-D; axis forms keep the input shape.

// 4c — reductionsCpuGpuAgree: the stdlib GPU reduction (Ewise.sumF32, atomic
// parallel sum) routes on placement — on-device → atomicAdd reduction; host → CPU
// loop — and the two agree, and agree with the generic CPU Tensor.sum. Integer-
// valued floats (sum 36 < 2^24) → exact regardless of atomic order. cajeta.xpu CPU
// backend in-process (no GPU required).

// 5a — reshape-family view ops: ravel/flatten (1-D), swapaxes/transposeAxes/moveaxis
// (axis permutation views), flipAll (reverse every axis). Views share storage; values
// verified through get1/get2, and moveaxis through a contiguous copy.

// 5a — join/split: concatenate (existing axis), stack (new axis), split (equal parts).

// 5a — replication/shift/pad: tile (per-axis reps), repeat (each elem n× along axis),
// pad (constant), roll (cyclic shift). All produce fresh copies.

// 5a — matrix extraction: diagonal (k-th diagonal → 1-D), tril/triu (lower/upper
// triangle with the rest zeroed).

// 5b — conditional/fancy selection: compress (keep axis slices where a 1-D mask is
// true) and choose (per-element pick from choice tensors by an index tensor). Plus a
// round-trip of the existing maskedSelect/maskedAssign compaction read/write.

// 5b/5c — gatherCpuGpuAgree: the stdlib GPU gather (Ewise.takeF32) routes on
// placement — on-device → the gatherF32 kernel (out[i]=in[idx[i]]); host → CPU
// loop — and the two agree. cajeta.xpu CPU backend in-process (no GPU required).

// 6a — 2-D matmul + 1-D dot (CPU GEMM floor). out[i,j] = Σ_p a[i,p]*b[p,j].

// 6a — vdot (flattened inner product → scalar) + outer (1-D × 1-D → 2-D).

// 6a — trace (diagonal sum), kron (Kronecker product), matrix_power (repeated matmul).

// 6a — tensordot (integer axes): contracts last `naxes` of a with first `naxes` of b.
// naxes=1 over (m,k)·(k,n) reproduces matmul; (m,k)·(k,) gives matrix-vector.

// 6d — inner: contract the LAST axis of each operand. For a (...i,k) and b (...j,k)
// (equal last axis k) → (...i,...j) with out[i,j] = Σ_k a[i,k]*b[j,k]. Two 1-D
// vectors give a 0-D scalar tensor (read via getAt with any index — ndim 0).

// 6b/6e — matmul GPU lowering: Ewise.matmulF32Op routes on placement, lowering to
// the CooperativeMatrix tiled GEMM (16x16 tiles, one workgroup per output tile) on
// device. Cross-check: the device coop-matrix result == the Tensor.matmul CPU floor,
// bit-exact (small integers exact in float32). 32x32x32 → 2x2 output tiles, 2 K-tiles.

// 6c/6f — einsum: parse the subscript spec → contraction plan → nested sum walk.
// Representative set: transpose, trace, matmul, batched matmul, diagonal, sum-all,
// row-sum, and the 1-D inner product (dot). Explicit `->` output required.

// 7a — sort + argsort along an axis (numpy default ascending). sort returns a fresh
// sorted copy; argsort returns the Tensor<int64> permutation. Both STABLE (ties keep
// original order — numpy `kind='stable'`). Default numpy axis is the last; tested here
// with an explicit axis arg for 1-D, axis 0 and axis 1.

// 7a — searchsorted (binary-search insertion indices into a sorted 1-D array; side
// 0=left → first i with a[i] >= v, 1=right → first i with a[i] > v) + partition /
// argpartition (the kth element lands in its sorted slot, smaller before / larger
// after; the CPU floor returns a fully-ordered lane, a valid stronger partition).

// 7 (deferred) — real introselect partition/argpartition: element kth is the true kth
// order statistic, smaller before / larger after (sides NOT fully ordered), across every
// kth, plus a reverse-sorted stress input. Values 0..9 ⇒ the kth order statistic == kth.

// 7a — unique (sorted distinct values over the flattened input), flatnonzero (C-order
// flat indices where != 0), nonzero (per-dimension coordinate arrays, the numpy tuple),
// extract (arr elements where a condition is nonzero). All return fresh tensors.

// 7b — GPU sort: Ewise.sortF32Op routes on placement, lowering to the bitonic-sort
// network (host-driven O(log^2 n) compare-exchange stages over the device buffer) on
// device. Cross-check: the device bitonic result == the Tensor.sort CPU floor, exact
// (distinct float32 keys). Length 8 (power of two) → 6 stages.

// 8a — fft/ifft round trip + reference-DFT spot checks (Phase 8, cajeta.math.fft).
// Complex signals are interleaved float32 ([2i]=re,[2i+1]=im). Radix-2 Cooley-Tukey,
// power-of-two length. impulse→flat ones; constant→DC only; ifft(fft(x))≈x. float32
// accumulation, so checked within tolerance.
TEST(NumpyOpsTests, fftRoundTripMatchNumpy) {
    std::string src = std::string(PRE) +
        "import cajeta.math.fft.Fft;\n"
        "public final class D {\n"
        "    public static boolean close(float32 a, float32 b) {\n"
        "        float32 d = a - b; if (d < 0.0f) { d = -d; } return d < 0.01f;\n"
        "    }\n"
        "    public static int32 run() {\n"
        // impulse [1,0,0,0] (N=4) → flat ones spectrum
        "        float32[] di = [ 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f ];\n"
        "        int64[] s8 = heap int64[1]; s8[0] = 8;\n"
        "        Tensor<float32> imp #= Tensor.of<float32>(di, s8);\n"
        "        Tensor<float32> fi #= Fft.fft(imp);\n"
        "        int64 i = 0;\n"
        "        while (i < 4) {\n"
        "            if (!D.close(fi.get1(2 * i), 1.0f)) { return -1; }\n"
        "            if (!D.close(fi.get1(2 * i + 1), 0.0f)) { return -2; }\n"
        "            i = i + 1;\n"
        "        }\n"
        // constant [1,1,1,1] (N=4) → DC = 4, rest 0
        "        float32[] dc = [ 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f, 1.0f, 0.0f ];\n"
        "        int64[] s8b = heap int64[1]; s8b[0] = 8;\n"
        "        Tensor<float32> con #= Tensor.of<float32>(dc, s8b);\n"
        "        Tensor<float32> fc #= Fft.fft(con);\n"
        "        if (!D.close(fc.get1(0), 4.0f) || !D.close(fc.get1(1), 0.0f)) { return -3; }\n"
        "        i = 1;\n"
        "        while (i < 4) {\n"
        "            if (!D.close(fc.get1(2 * i), 0.0f) || !D.close(fc.get1(2 * i + 1), 0.0f)) { return -4; }\n"
        "            i = i + 1;\n"
        "        }\n"
        // round trip ifft(fft(x)) ≈ x for an arbitrary complex signal
        "        float32[] dx = [ 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f ];\n"
        "        int64[] s8c = heap int64[1]; s8c[0] = 8;\n"
        "        Tensor<float32> x #= Tensor.of<float32>(dx, s8c);\n"
        "        Tensor<float32> fx #= Fft.fft(x);\n"
        "        Tensor<float32> rt #= Fft.ifft(fx);\n"
        "        float32[] want = [ 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f ];\n"
        "        i = 0;\n"
        "        while (i < 8) {\n"
        "            float32 wv = want[(int32) i];\n"
        "            float32 gv = rt.get1(i);\n"
        "            if (!D.close(gv, wv)) { return -5; }\n"
        "            i = i + 1;\n"
        "        }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 8a/8c — fftfreq (DFT sample frequencies, numpy order), fftshift (zero-freq to centre),
// rfft (real → half spectrum) + irfft (Hermitian reconstruct → real), real round trip.

// 6 (deferred) — batched matmul: a (2,2,2) @ b (2,2,2) per-batch. numpy oracle
// np.matmul([[[1,2],[3,4]],[[5,6],[7,8]]], [[[1,0],[1,1]],[[2,1],[0,3]]]) = [3,2,7,4,10,23,14,31].

// 8 (deferred) — fft2/ifft2 (2-D DFT), fftn (N-D per-axis walk), hfft (Hermitian → real).
// Oracles from numpy: fft2([[1,2],[3,4]]) = [10,-2,-4,0]; hfft([1+2j,3-1j,0.5],4)=[7.5,-1.5,-4.5,2.5].

// 9 (deferred) — permutation/shuffle/choice over the Philox stream: valid permutation +
// reproducible; shuffle preserves the multiset; choice(replace=false) is distinct + in-range,
// choice(replace=true) is in-range. Structural (numpy uses a different permutation algorithm).

// 11 (deferred) — GPU linalg (representative): Ewise.normFroF32 routes on placement; the
// device atomic sum-of-squares + sqrt agrees with the CPU floor. [2,3,6] → sqrt(49)=7.

// 6.6 (deferred) — coop GEMM non-f32: Ewise.matmulF64Op runs the CooperativeMatrix tiled
// GEMM for float64 (the software coop tier is dtype-generic); the device result equals the
// generic Tensor.matmul<float64> floor bit-for-bit (32x32x32, integer-valued so exact).

// 5c (deferred) — GPU structural ops: Ewise.concatF32Op / sliceF32Op route on placement;
// the device range-copies agree with the CPU loop. concat([1,2,3],[4,5,6,7])→[1..7];
// slice(that,2,3)→[3,4,5].

// 4 (deferred) — GPU prefix scan: Ewise.cumsumF32Op routes on placement; the device
// Hillis-Steele inclusive scan agrees with the CPU running sum. [1..8] → [1,3,6,10,15,21,28,36].

// 4 (deferred) — GPU min/max reduction: Ewise.minF32/maxF32 route on placement; the device
// atomic-min/max agrees with the CPU loop. data=[3,-1,4,1,-5,9,2,6] → min=-5, max=9.

// 5e (deferred) — GPU scatter: Ewise.scatterF32Op routes on placement; the device scatter
// (out[idx[i]]=in[i]) agrees with the CPU loop. values=[10,20,30,40] idx=[3,1,0,2] → [30,20,40,10].

// 10 (deferred) — GPU atomic-scatter bincount: Ewise.bincountI64Op routes on placement;
// the device atomic scatter agrees with the CPU loop. vals=[0,1,1,2,2,2,3] → counts=[1,2,3,1].

// 8b — GPU FFT: FftGpu.fftF32Op routes on placement, lowering to the butterfly-stage
// network (host-driven log2(N) stages over the device buffer). Cross-check: the device
// FFT == the Fft.fft CPU floor within a float tolerance. N=8 (interleaved length 16).

// 9a — rngReproducible: the Philox4x32-10 counter-based Generator yields the SAME stream
// for the same seed, a DIFFERENT stream for a different seed, values in [0,1), and a
// roughly-uniform mean. Counter-based ⇒ deterministic + element-independent.

// 9b — distributionMoments: sampled mean/variance match each distribution's theory.
// uniform → mean 0.5, var 1/12; normal → mean 0, var 1; integers[0,10) → mean ~4.5,
// all in range. 4096 samples → tight tolerances.

// 9c — rngCpuGpuParity: the counter-based Philox stream is bit-identical on CPU and GPU
// for the same seed (per-element kernel, no sequential state). GeneratorGpu.uniformF32
// (device) == Generator.uniform (CPU floor) exactly.

// 10a — binning stats: histogram (equal-width counts over [lo,hi]), bincount (count of
// each non-negative int), digitize (bin index vs increasing edges, right=False).

// 10a — cov (covariance matrix, ddof=1, rowvar) + corrcoef (Pearson correlation).

// 10a — quantile (linear interp), percentile, median (incl. even-length + unsorted input).

// 10c — poly: polyval (Horner), polyadd (degree-aligned), polymul (convolution).
// Coefficients highest-degree-first (numpy order).

// 11b — native linalg: solve (Gaussian elimination + partial pivoting), det, inv.

// 11b — native LU (A=P·L·U, unit-lower L, upper U) + QR (A=Q·R, orthonormal Q).
// Verified by reconstruction (matmul) + structure; pivoting/sign are free per numpy.

// 11c — conditioning / edge cases: pinv, lstsq, matrix_rank, cond, Frobenius norm
// (svd-derived), incl. a singular matrix (rank-deficient) handled gracefully.

// 10.5 — npio numpy-interop harness: numpy writes a .npy → cajeta reads + verifies →
// cajeta writes a .npy → real numpy reads + verifies. Skips if python/numpy is absent.

// 10.5 — npio: cajeta round-trip of a float32 .npy (write then read back in cajeta).

// 10.5 — npio: cajeta round-trip of a float64 .npy ('<f8', 8-byte little-endian path).

// 10.5 — npio: cajeta round-trip of an int32 .npy ('<i4', exact integer equality).

// 10.5 — npio: cajeta round-trip of an int64 .npy ('<i8', 8-byte little-endian path).
TEST(NumpyOpsTests, npyInt64RoundTrip) {
    std::string path = (std::filesystem::temp_directory_path() / "cajeta_npy_rt_i64.npy").string();
    std::replace(path.begin(), path.end(), '\\', '/');
    std::string src = std::string(PRE) +
        "import cajeta.math.npio.Npy;\n"
        "public final class D {\n"
        "    public static int32 run() {\n"
        "        int64 big = 9223372036854775807;\n"        // int64 max
        "        int64 small = 0 - big;\n"                     // -(max) = int64 min + 1 (avoid min-literal overflow)
        "        int64[] da = [ 7, -3, 0, big, small, 42 ];\n"
        "        int64[] s23 = heap int64[2]; s23[0] = 2; s23[1] = 3;\n"
        "        Tensor<int64> t #= Tensor.of<int64>(da, s23);\n"
        "        Npy.saveI64(\"" + path + "\", t);\n"
        "        Tensor<int64> r #= Npy.loadI64(\"" + path + "\");\n"
        "        if (r.ndim() != 2) { return -1; }\n"
        "        if (r.shapeAt(0) != 2 || r.shapeAt(1) != 3) { return -2; }\n"
        "        if (r.get2(0, 0) != 7 || r.get2(0, 1) != -3) { return -3; }\n"
        "        if (r.get2(0, 2) != 0 || r.get2(1, 0) != big) { return -4; }\n"
        "        if (r.get2(1, 1) != small || r.get2(1, 2) != 42) { return -5; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 10.5 — npio numpy-interop for the non-f32 dtypes: numpy writes each .npy → cajeta loads,
// verifies, and re-saves → real numpy re-verifies. Skips if python/numpy is absent.

// 10.5 — npz: cajeta round-trips a multi-array, mixed-dtype `.npz` (write with NpzWriter,
// read each named member back with the typed Npz loaders).

// 10.5 — npz numpy-interop: cajeta writes a mixed-dtype `.npz`, then REAL numpy `np.load`s
// it and verifies every member (dtype/shape/values). Proves the ZIP+CRC framing is valid,
// not merely self-consistent. Uses a plain (Linux-friendly) python invocation — unlike the
// older npy interop harness whose `cmd /c` outer-quote wrapping only works on Windows.

// 10.5 precursor — float<->raw-bits reinterpret intrinsics (Cajeta.f32ToBits/bitsToF32,
// f64ToBits/bitsToF64): LLVM bitcast, NOT a value conversion. Unblocks binary float .npy.
TEST(NumpyOpsTests, floatBitsIntrinsicRoundTrip) {
    std::string src = std::string(PRE) +
        "public final class D {\n"
        "    public static int32 run() {\n"
        // 1.0f IEEE-754 bits == 0x3F800000 == 1065353216 (a reinterpret, not (int32)1)
        "        int32 b = Cajeta.f32ToBits(1.0f);\n"
        "        if (b != 1065353216) { return -1; }\n"
        // bitsToF32 is the exact inverse
        "        float32 x = Cajeta.bitsToF32(b);\n"
        "        if (x != 1.0f) { return -2; }\n"
        // arbitrary value round-trips bit-exactly
        "        float32 y = 3.14159f;\n"
        "        int32 yb = Cajeta.f32ToBits(y);\n"
        "        float32 y2 = Cajeta.bitsToF32(yb);\n"
        "        if (y2 != y) { return -3; }\n"
        // negative value (sign bit set) round-trips
        "        float32 z = -42.5f;\n"
        "        float32 z2 = Cajeta.bitsToF32(Cajeta.f32ToBits(z));\n"
        "        if (z2 != z) { return -4; }\n"
        // f64 round-trip
        "        float64 dv = 2.5;\n"
        "        int64 db = Cajeta.f64ToBits(dv);\n"
        "        float64 dx = Cajeta.bitsToF64(db);\n"
        "        if (dx != dv) { return -5; }\n"
        // a (int32) cast CONVERTS (truncates) — must differ from the bit reinterpret
        "        int32 conv = (int32) 1.0f;\n"
        "        if (conv == 1065353216) { return -6; }\n"
        "        return 1;\n"
        "    }\n"
        "}\n";
    EXPECT_EQ(runI32(src), 1);
}

// 11 (deferred) — general nonsymmetric eigenvalues via Francis double-shift QR
// (elmhes + hqr). Complex eigenvalues emerge as conjugate pairs with real arithmetic only.
// [[2,-1,0],[1,2,0],[0,0,5]] → {2-i, 2+i, 5} (numpy eigvals, compared as a sorted set).

// 11 (deferred) — nonsymmetric eig on a DENSE matrix needing real QR convergence (not a
// trivial block read-off). [[1,2,3],[4,5,6],[7,8,10]] → {-0.9057, 0.1982, 16.7075} (all real).

// 11b — native symmetric eig (Jacobi) + SVD (via eigh of A^T A). Eigenvalues ascending,
// singular values descending; verified by spectrum + A·v=λ·v and U·diag(S)·Vt==A.

// 11b — native Cholesky: L·L^T = A for symmetric positive-definite A (lower triangular).
