# SIMD numeric kernels — matmul & dot-product

> Status: DRAFT (authored with the design skill). Goal: close the numeric-kernel
> performance gaps the benchmark suite exposes, using the `Vector<T,N>` SIMD path
> that XXHash3 already proved beats C++.

## 1. Definition

### 1.1 Purpose
The benchmark suite shows Cajeta's numeric kernels far off the leaders — **matmul ~36×**
(2.834 ms vs numpy/BLAS 0.079 ms) and **dot-product ~35×** (vs numpy). These are NOT a codegen
limitation: `XXHash3` — written with Cajeta's `Vector<T,N>` types — runs at **0.007 ms and beats
C++** on the same machine. The gap is that the numeric kernels are written as **scalar loops**
(0 Vector lines) that lean on the autovectorizer, which is hit-or-miss. This feature rewrites
matmul and dot-product the *xxhash3 way* — explicit `Vector<float64,N>` + FMA — to recover the
performance the compiler can already deliver.

### 1.2 Problem (per kernel, with evidence)
- **matmul (MatMulBench, 200×200 float64).** Two compounding faults: (a) **`ijk` loop order** —
  the inner `k` loop reads `B[k*n+j]` **column-wise** (stride n=200), a cache miss per access at
  a ~960 KB working set; (b) **no SIMD** (scalar `acc`). The standard fix is `ikj` order (the
  inner loop becomes a *contiguous* row update `C[i,:] += A[i,k]·B[k,:]`) + vectorize.
- **dot-product (DotProductBench).** A floating-point **reduction**, which the autovectorizer
  refuses to vectorize (it won't reorder FP adds across the serial `acc` dependency without
  fast-math). xxhash3 sidesteps exactly this by being explicit with independent lanes.
- **Infrastructure gap.** The compiler exposes **int64** vector load/store intrinsics
  (`Cajeta.vload8i64`/`vstore8i64`, what xxhash3 uses) but **no float64 equivalents**, and
  `Vector<float64,N>` is currently unused. Float64 vector load/store (+ FMA) is the shared
  prerequisite.

### 1.3 Solution overview
1. **Shared:** add float64 vector load/store (and, if absent, fused-multiply-add) so a
   `Vector<float64,N>` can be loaded from / stored to a `float64[]` — mirroring `vload8i64`.
2. **matmul:** reorder to `ikj`, then vectorize the contiguous inner row update with
   `Vector<float64,N>` + FMA; add cache blocking if the reorder+SIMD leaves a material gap.
3. **dot-product:** accumulate in `N` independent `Vector<float64,N>` lanes (FMA), horizontal-sum
   at the end — a source-level reassociation the programmer chooses, so it vectorizes.

### 1.4 Constraints
- **Correctness within FP semantics.** matmul here is exact (A = identity ⇒ each
  `C[i,j] = B[i,j]` regardless of summation order ⇒ `ikj` is bit-exact; the `cSum==bSum` check
  holds). **dot-product reassociates** the sum, so the result differs in the low bits — the
  benchmark's check must tolerate FP reassociation (a relative epsilon), NOT exact equality.
- **`cpu=native` + ThinLTO.** The SIMD must survive the release build (the xxhash3 path already
  does, via the `target-features` fn-attr fix). Verified in release, not only JIT.
- **Portability.** Use `Vector<float64,N>` (portable) so the lane width follows the target
  (AVX2 N=4 / AVX-512 N=8); avoid target-specific intrinsics in user code.

### 1.5 Non-goals
- Full BLAS parity for matmul (multi-threaded, hand-tuned microkernels). The target is a
  *good* single-threaded blocked+SIMD kernel — competitive with optimized scalar C++ and within
  a small factor of BLAS, not matching numpy's threaded BLAS outright.
- A general autovectorization pass / `@SIMD` auto-lowering — out of scope; this is explicit
  `Vector` kernels.

## 2. Feature: float64 vector infrastructure (shared prerequisite)
- 2.1 As a kernel author, I can load a `Vector<float64,N>` from a `float64[]` at an element
  offset and store one back — the float64 analogue of `Cajeta.vload8i64`/`vstore8i64`.
- 2.2 As a kernel author, element-wise `+`, `*` on `Vector<float64,N>` and a scalar-broadcast
  multiply/add lower to packed AVX (and FMA where the pattern `a + b*c` appears), under
  `cpu=native` + ThinLTO.
- 2.3 As a kernel author, I can horizontal-sum a `Vector<float64,N>` (or extract lanes via the
  existing `Cajeta.vlane`) to finish a reduction.
- 2.4 As the compiler, these intrinsics are registered alongside the existing int64 ones and
  carry the same release/ThinLTO codegen guarantees.

## 3. Feature: matmul vectorization
- 3.1 As a developer, `C = A·B` computes the same result as today (checksum `cSum==bSum` holds)
  but **much faster**, via `ikj` ordering + a vectorized contiguous inner update.
- 3.2 As the compiler/kernel, the inner loop is `C[i, j:j+N] += splat(A[i,k]) * B[k, j:j+N]` with
  `Vector<float64,N>` + FMA — contiguous loads/stores, no column striding.
- 3.3 As a kernel author, a tail loop handles `n % N` columns scalar-ly; correctness is
  independent of N.
- 3.4 As a kernel author, optional cache blocking (tile i/k/j) is applied if step 3.2 alone
  leaves a material gap at n=200 (measured, not assumed).
- 3.5 As a developer, the matmul benchmark's `check=true` holds and the median improves by a
  large factor over the 2.834 ms baseline (target: within a small factor of BLAS; concretely
  ≤ ~0.5 ms is a strong result, ≤ ~0.25 ms excellent).

## 4. Feature: dot-product vectorization
- 4.1 As a developer, `dot(a,b) = Σ a[i]·b[i]` is computed with `N` independent
  `Vector<float64,N>` accumulators (FMA) and a final horizontal sum — vectorized, not scalar.
- 4.2 As a kernel author, a scalar tail handles `len % N`.
- 4.3 As a developer, the result differs from the strict left-to-right sum only by FP
  reassociation; the benchmark check uses a relative-epsilon tolerance (updated if it currently
  demands exact equality).
- 4.4 As a developer, dot-product's median improves by a large factor over the ~0.395 ms
  baseline (the autovectorizer could not touch the reduction; explicit lanes should close most
  of the 35× — a memory-bandwidth-bound kernel at large N).

## 5. Feature: cross-cutting requirements
- 5.1 **Release/ThinLTO gate.** Both kernels measured in the release build; SIMD confirmed
  present (disassembly or the speedup itself).
- 5.2 **Correctness.** matmul exact; dot-product within tolerance; both `check=true`.
- 5.3 **Portability.** Lane width via `Vector<float64,N>`; the same source runs AVX2/AVX-512.

## 6. Acceptance themes
- Float64 vector load/store/FMA available and release-safe.
- matmul: `ikj`+SIMD (+blocking if needed) — large measured speedup, exact result.
- dot-product: vectorized reduction — large measured speedup, within FP tolerance.
- Both verified under ThinLTO; the xxhash3 SIMD-kernel template is shown to generalize.
