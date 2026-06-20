# numpy → cajeta — `cajeta.math` porting surface & placement spec

> **Status: spec (requirements + classification + placement decision).** The "porting
> report": a survey of numpy's full API surface, classified by where each subsystem lands
> in cajeta, with the stdlib boundary **resolved** (not just surveyed). The `Tensor`
> keystone + the core op layers get built per the companion plan
> (`numpy-porting-plan.md`); `cajeta.math.{linalg,fft,random}` are later phases there;
> scipy/sklearn (`cajeta.sci`/`cajeta.learn`) are out of scope (separate efforts).

## 1. The decision (what this resolves)

cajeta is a numerical/ML-first language: the GPU substrate (`cajeta.gpu` + `cajeta.gpu.xpu`)
exists to be the thing numpy/torch/keras-style work runs on. numpy is the **foundational
n-dimensional array** the whole scientific/ML ecosystem is built on. The question is not
*whether* to provide it but *how much is stdlib and where the rest lives*. Resolved:

- **`cajeta.math` (bundled stdlib) = the full numpy-equivalent.** The canonical `Tensor`
  (ndarray) + the core ops **and** the `cajeta.math.{linalg, fft, random, stats}`
  submodules — mirroring numpy's own module layout (`numpy.linalg`/`numpy.fft`/
  `numpy.random` are numpy submodules, not separate packages).
- **`cajeta.sci` / `cajeta.learn` (separate official libs) = the scipy / sklearn tier.**
  The huge, churny, opinionated, domain-specific surface — built on the stdlib `Tensor`,
  versioned independently.
- **`cajeta.gpu.xpu` = the GPU compute-primitive substrate** the math layer *lowers onto*
  (`CooperativeMatrix`/`CoopStage`, elementwise/reduction/scan kernels). Not an API home.
- **torch / keras = separate opinionated frameworks** (autograd, `nn`). Out of scope.

### Why the whole numpy-equivalent is stdlib (the two arguments that moved the line)
Earlier reasoning kept only "Tensor + core" in stdlib and pushed `linalg`/`fft`/`random`
out. Two facts overturn that:

1. **Optimal linkage removes the bloat objection.** cajeta's output is dead-code-eliminated
   (`-Wl,--gc-sections`; the JIT compiles only referenced code) **and whole-program
   optimized across the stdlib/user boundary**. So **unused stdlib costs nothing in the
   binary, and used stdlib inlines across the boundary.** The historical reason to keep a
   numerical library external (dependency weight — the Rust/Python concern) does not apply.
2. **The BLAS/LAPACK/numpy API is frozen.** BLAS has been a fixed standard since the 1980s;
   LAPACK's backward-compat is sacrosanct (existing signatures essentially never change;
   releases are ~yearly bug-fixes + *added* routines), and numpy's own API is decades-stable.
   A stdlib is a forever-API commitment — and committing to an API that is *already* a
   forever-API in the field is safe. So the **churn / API-stability objection does not apply**
   to the numpy-equivalent. (It *does* apply one tier up — sklearn churns; scipy moves
   moderately — which is exactly why those stay separate.)

What's left after those two fall away is **governance, not footprint** — and on governance
the numpy-equivalent is safe (frozen API), while scipy/sklearn is not (churn). Hence the line.

### The one real cost that remains: the LAPACK *backend form*
The LAPACK-class **factorizations** (`svd`/`eig`/`qr`/`cholesky`/`lu`/`solve`) are
host-orchestrated, numerically delicate, and traditionally a **Fortran + per-arch assembly**
backend (OpenBLAS bundles LAPACK; reference LAPACK is ~1,700 Fortran routines, ~10–20 MB
compiled — *less than the `libLLVM` cajeta already links*, so size is not the issue). The
issue is **form**: bundling a real LAPACK means the cajeta build carries a Fortran/asm
dependency, OR cajeta reimplements LAPACK natively (a large, multi-phase numerical effort).
**BLAS itself is a non-issue — cajeta already owns GEMM** (`CooperativeMatrix`/tiled) and
Level-1/2 are trivial kernels. So the factorization backend is the single piece whose
*how* (wrap vs. reimplement) is deferred to its plan phase; everything else in the
numpy-equivalent is native-cajeta and kernel-expressible.

### The gating prerequisite: lazy stdlib parsing
The stdlib is currently **embedded and eagerly parsed at compiler startup**
(`CAJETA_STDLIB_DIRS`). Bundling the whole numpy-equivalent there would slow *every*
`cajeta` invocation and bloat the compiler binary — a *compiler-side* cost that optimal
*linkage* (of the output) does not address. **Lazy / on-import stdlib parsing is therefore
a prerequisite** for landing `cajeta.math` at scale (plan Phase 1).

## 2. Placement buckets
- **M — `cajeta.math` (bundled stdlib).** The numpy-equivalent: the `Tensor` + core ops +
  `cajeta.math.{linalg, fft, random, stats}`. Two execution tiers inside M (see §3.12):
  *kernel-expressible* (the bulk — elementwise/reductions/scans/contraction/sort/fft/RNG)
  and *host-orchestrated* (the LAPACK factorizations — the foreign-backend piece).
- **S — separate official lib.** `cajeta.sci` (scipy-shaped: optimize/integrate/interpolate/
  sparse/special/advanced-signal) and `cajeta.learn` (sklearn-shaped estimators). Churny,
  huge, opinionated; built on the stdlib `Tensor`.
- **X — `cajeta.gpu.xpu` substrate.** GPU compute primitives the M layer lowers onto. Not
  a numpy-API home.
- **N — not applicable.** numpy-isms cajeta replaces or omits (`object` dtype, `vectorize`,
  `numpy.testing`, the `np.lib` Python helpers).

## 3. The full numpy surface, classified

Bucket in **bold**; `M:sub` names the `cajeta.math` submodule.

### 3.1 Array object & dtype — **M** (this *is* the `Tensor`)
| numpy | bucket | notes |
|---|---|---|
| `ndarray` (shape, strides, data ptr, views) | **M** | The keystone `Tensor`. |
| dtype: `bool`, `int8/16/32/64`, `uint8/16/32/64`, `float16/32/64`, `complex64/128`, `bfloat16`/`float8_*` | **M** | Numeric + bool core (cajeta already has fp4/fp6/fp8/bf16 native types). |
| dtype: `float128`/`complex256` | **S** | Extended precision; niche, not on GPU. |
| dtype: `datetime64`/`timedelta64` | **S** | `cajeta.time`-adjacent subsystem. |
| dtype: structured / record | **S** | Separate concern. |
| dtype: `object` | **N** | Boxed-heterogeneous; un-cajeta. |
| views/`base`/C-F order/strides/`.T` | **M** | Core view semantics. |
| array/buffer interop protocol (`__array_interface__`, DLPack) | **M** | The **lingua-franca seam** — the whole reason `Tensor` is canonical-in-stdlib. |

### 3.2 Array creation — **M**
`array`/`asarray`, `zeros`/`ones`/`empty`/`full`(+`_like`), `arange`, `linspace`/`logspace`/
`geomspace`, `eye`/`identity`, `diag`/`diagflat`, `meshgrid`, `fromfunction`, `frombuffer`/
`fromiter`, `copy`. (`loadtxt`/`genfromtxt`/`fromfile` → file I/O, **S**.)

### 3.3 Elementwise (ufuncs) — **M** (kernel-expressible)
Arithmetic, comparison, logical, bitwise, trig/hyperbolic/inverse, `exp`/`log` family,
`sqrt`/`cbrt`/`square`/`hypot`, `abs`/`sign`/`copysign`, rounding, `clip`, `isnan`/`isinf`/
`isfinite`, 3-arg `where`. Plus ufunc machinery (`reduce`/`accumulate`/`outer`/`reduceat`/`at`).
Special functions (`erf`/`gamma`/`beta`/Bessel) → **S** (`cajeta.sci`).

### 3.4 Reductions & scans — **M** (kernel-expressible)
`sum`/`prod`, `min`/`max`, `argmin`/`argmax`, `mean`, `std`/`var`, `cumsum`/`cumprod`,
`any`/`all`, `count_nonzero`, `ptp`, `nan*` variants. `median`/`percentile`/`quantile` →
**M:stats** (need selection + interpolation policy).

### 3.5 Shape manipulation — **M** (kernel-expressible)
`reshape`, `ravel`/`flatten`, `transpose`/`swapaxes`/`moveaxis`, `squeeze`/`expand_dims`,
`broadcast_to`/`broadcast_arrays`, `concatenate`/`stack`/`hstack`/`vstack`/`dstack`,
`split`/`hsplit`/`vsplit`, `tile`/`repeat`, `flip`/`roll`/`rot90`, `pad`, `block`.

### 3.6 Indexing — **M** (kernel-expressible)
Basic slicing, `newaxis`, ellipsis; boolean-mask; fancy/advanced; `take`/`put`/
`take_along_axis`, `compress`, `choose`, `select`, `diagonal`, `tril`/`triu`,
`indices`/`ix_`/`ogrid`/`mgrid`, `fill_diagonal`.

### 3.7 Sorting / searching / counting — **M** (kernel-expressible)
`sort`/`argsort`, `partition`/`argpartition`, `lexsort`, `searchsorted`, `unique`,
`where`/`nonzero`/`flatnonzero`, `count_nonzero`, `extract`. Set ops beyond `unique`
(`intersect1d`/`union1d`/`setdiff1d`/`isin`) → **M:stats**-adjacent or **S**.

### 3.8 Linear algebra — **M:linalg** (two execution tiers)
| op | tier | notes |
|---|---|---|
| `matmul`/`@`/`dot`/`vdot`/`inner`/`outer`/`tensordot`/`kron`/`trace`/`einsum`/`matrix_power` | **M, kernel-expressible** | GEMM/contraction — native cajeta via `CooperativeMatrix`. BLAS is a non-issue. |
| `solve`/`inv`/`pinv`/`lstsq` | **M:linalg, host-orchestrated** | LAPACK-class; foreign backend OR native reimpl (deferred decision). |
| `det`/`slogdet`/`matrix_rank`/`norm` | **M:linalg** | `norm` (1/2/inf) is light; the rest ride factorizations. |
| `qr`/`cholesky`/`lu`/`svd`/`eig`/`eigh`/`eigvals` | **M:linalg, host-orchestrated** | The factorization beast — the foreign-LAPACK-backend question. |

### 3.9 FFT — **M:fft** (kernel-expressible)
`fft`/`ifft`/`fft2`/`fftn`/`rfft`/`hfft`/`irfft`, `fftfreq`/`fftshift`. Butterfly kernels;
native cajeta, GPU-accelerable. (Cohesive subsystem → its own submodule, still in `cajeta.math`.)

### 3.10 Random — **M:random** (kernel-expressible)
`Generator`/`BitGenerator` (PCG64, **Philox/counter-based** — the GPU-friendly ones), the
distribution set (uniform/normal/binomial/poisson/exponential/gamma/beta/dirichlet/…),
`permutation`/`shuffle`/`choice`, seeding/parallel streams.

### 3.11 The rest
| numpy area | bucket | notes |
|---|---|---|
| `numpy.polynomial` | **M:poly** or **S** | Small + stable → could be M:poly; lean S if it grows. |
| stats: `histogram`/`bincount`/`cov`/`corrcoef`/`digitize`/`quantile`/`median` | **M:stats** | `bincount` is atomic-scatter (kernel). The estimator-heavy stats → **S** (`cajeta.learn`). |
| `numpy.ma` (masked arrays) | **S** | A parallel array model. |
| signal: `convolve`/`correlate`/`interp`/`gradient`/`trapz` | `diff`→**M**; rest **S** (`cajeta.sci`) | |
| I/O: `save`/`load`/`.npy`/`.npz`/`loadtxt`/`memmap` | **M:npio** (formats) | The `.npy`/`.npz` interop is worth bundling; text parsing leans S. |
| `vectorize`/`frompyfunc`/`apply_along_axis` | **N** | cajeta `@Kernel`/map idioms replace these. |
| optimize / integrate / interpolate / sparse / special | **S** (`cajeta.sci`) | scipy tier. |
| ML estimators (regression/clustering/trees/…) | **S** (`cajeta.learn`) | sklearn tier — the churny one. |

### 3.12 Execution model — kernel-expressible vs host-orchestrated
Inside `cajeta.math`, two regimes (this is *implementation*, both still in the package):
- **Data-parallel — *is* a kernel.** elementwise, reductions, prefix scans,
  `matmul`/contraction (tensor cores), gather/scatter/mask indexing, sort, fft butterflies,
  counter-based RNG. The bulk of M — rides the `cajeta.gpu` substrate directly, with a CPU
  floor (cajeta.math is **CPU-first**: every op also has a portable path).
- **Host-orchestrated algorithm — *not* a kernel.** The LAPACK factorizations: host control
  flow, pivoting, convergence loops, many kernel launches (cuSOLVER/MAGMA shape), possibly
  a foreign BLAS/LAPACK backend. The lone non-kernel tier — and the only deferred *how*.

## 4. What the `Tensor` must nail (the forever-API)
Because the stdlib `Tensor` is a permanent commitment, settle up front:
1. **dtype model** — native numeric set (incl. fp4/fp6/fp8/bf16) + **type-based** promotion
   (NEP-50 style, not value-based) + `RoundingMode`-aware cast (cajeta already has the brick).
2. **shape / strides / views** — view-vs-copy contract, C/F order, broadcasting rules,
   aliasing/overlap rules.
3. **indexing** — basic + boolean + fancy, and the view-vs-copy each yields.
4. **device placement** — **CPU-first**; default host storage; lowers to `cajeta.gpu` for
   GPU execution; fully usable with **no GPU** (the reason it's `cajeta.math`, not `gpu.*`).
5. **interop protocol** — a `Tensor` protocol/trait (array-protocol / DLPack analogue) so
   external libs + the torch port share one array, zero-copy. The anti-fragmentation seam.
6. **lowering seam** — how a `cajeta.math` op picks CPU vs `cajeta.gpu` and lowers `matmul`
   onto `cajeta.gpu.xpu.CooperativeMatrix`.

## 5. Goals / Non-goals
**Goals:** the classified surface (§3); the resolved boundary (§1–§2); the `Tensor`
forever-API constraints (§4); the prerequisite (lazy stdlib parse) and the one deferred
*how* (LAPACK backend form).
**Non-goals:** implementation (the plan); scipy/sklearn (`cajeta.sci`/`cajeta.learn`,
separate); the torch/keras frameworks.

## 6. Acceptance criteria
1. Every major numpy subsystem appears in §3 with a bucket.
2. The `cajeta.math` boundary is unambiguous: the numpy-equivalent (core + `linalg`/`fft`/
   `random`/`stats`) is **M**; scipy/sklearn is **S**.
3. The two arguments for bundling (optimal linkage; frozen API) and the two residual
   concerns (LAPACK backend form; lazy-parse prerequisite) are stated.
4. `cajeta.gpu.xpu`'s role is the acceleration substrate, not an API home.
5. The `Tensor` forever-API (§4) is enumerated as the gate for the plan's Phase 2.

## 7. Recommendation (the answer to "should numpy live in math?")
**Yes — the whole numpy-equivalent lives in `cajeta.math`** (bundled stdlib: `Tensor` +
core + `linalg`/`fft`/`random`/`stats`), because optimal linkage removes the bloat cost and
the frozen BLAS/LAPACK/numpy API removes the churn cost. **scipy/sklearn do not** — they are
`cajeta.sci`/`cajeta.learn`, separate official libs on the stdlib `Tensor`. Two follow-ups
gate execution, not placement: **lazy stdlib parsing** (prerequisite) and the **LAPACK
factorization backend form** (wrap vs. native reimpl — decided in its plan phase). Build
order is in `numpy-porting-plan.md`.
