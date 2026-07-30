# SciPy Façade — Specification

> Status: draft for review (2026-06-23). A **Layer-2 façade** (`dev.cajeta.scipy`) — a
> thin, recognizable skin of SciPy's algorithm modules over the núcleo core, the stdlib
> tensor substrate (`cajeta.math.Tensor`, numpy, done), and the sparse/linalg engine.
> Companion analysis: `python-stack-analysis.md` §3.4 (the port/drop decision) and §5
> (priority: pull spatial/signal/optimize forward, defer the rest); design context:
> `target-experience.md` §5. Engine cross-refs: `nucleo-sparse-linalg-spec.md` (sparse
> arrays + sparse linalg — this façade re-specifies none of it), `nucleo-column-spec.md`
> (the columnar substrate), `records-spec.md` (the typed-return-record machinery this
> façade's result types stand on).
>
> This is a **spec** — requirements and use cases (the *why/what*). Build decisions (the
> *how*) are deferred as `> **TBD (plan-time):**` markers and collected in §13, to be
> resolved when this spec is turned into a plan. Outline-numbered for addressability.
>
> **Amendment (2026-07-29, python-stack-analysis §4.7):** the façade ships as an
> **external library** (the cajeta-xgboost model), not stdlib, and the engines it skins
> are distributed per §4.7 — sparse type in núcleo, factorizations in
> `cajeta.math.linalg` (`specs/linalg-solvers-spec.md`), algorithm families in
> per-domain external `cajeta-*` libs commissioned on demand. There is no `cajeta-scipy`
> monolith; this façade is the muscle-memory *skin* only. Reference source pinned:
> scipy v1.18.0 at `code/ml/scipy-ref`.

## 1. Definition

### 1.1 Purpose
`dev.cajeta.scipy` gives a SciPy user their **muscle memory** — `optimize.minimize`,
`signal.butter`, `spatial.KDTree`, `integrate.solve_ivp`, `special.gamma` — over a
categorically better engine: pure functions over typed `Tensor` values, **typed result
records** in place of SciPy's `OptimizeResult` grab-bags and ad-hoc positional tuples,
and a sparse *array* type with no Fortran-order / LAPACK leakage. The façade is the
on-ramp; the typed surface underneath is the reason to stay. It is **recognizable, not
faithful** (analysis §1.3, §2.5): where an upstream convention encodes a genuine mistake,
the façade corrects it rather than inheriting it.

### 1.2 Scope
- A submodule-organized façade mirroring SciPy's module layout, each submodule a class of
  **static methods** (Cajeta has no global functions): `optimize`, `signal`,
  `interpolate`, `integrate`, `special`, `spatial`, `stats`, `ndimage`, `cluster`.
- A **typed result record** for every algorithm that returns more than one value
  (§3, §6) — declared via the `record` feature (`records-spec.md`), not returned as a
  tuple bag or attribute grab-bag.
- A thin binding only: each method is a pure function over `Tensor` (and, where relevant,
  the sparse array type from `nucleo-sparse-linalg-spec.md`); the actual numerics live in
  núcleo / `cajeta.math`, not here.
- **Named arguments + defaults** as the call convention (`minimize(f, x0:, method:)`),
  replacing SciPy's positional+`**kwargs`+`options=dict` sprawl.
- **Priority cut (§12):** `spatial`, `signal`, and `optimize` are pulled forward (graphics
  and ML need them now); `interpolate`, `integrate`, `special`, `stats`, `ndimage`,
  `cluster` are specified here but sequenced behind.

### 1.3 Non-goals
- **Re-specifying engine internals.** Sparse storage (CSR/CSC/COO), sparse-linalg solvers,
  dense factorizations, FFT, and the columnar substrate are owned by
  `nucleo-sparse-linalg-spec.md`, `nucleo-column-spec.md`, and `cajeta.math`. This façade
  is a *skin* that names and types them; it does not define how they compute.
- **Bug-for-bug fidelity.** SciPy's `OptimizeResult` attribute bags, sparse *matrix*
  (`np.matrix`-based, itself deprecated upstream), Fortran-order array contracts, and
  inconsistent return conventions are explicitly **not** reproduced (analysis §3.4 drop
  list).
- **The full SciPy surface in v1.** `io`, `constants`, and the long algorithm tail are out
  of v1 scope; the cut is §12.
- **A new tensor or sparse type.** The façade reuses `cajeta.math.Tensor` and the núcleo
  sparse array; it introduces only *result records*, not new data containers.

### 1.4 Relationship to existing constructs
- Sits **on top of** the núcleo core and `cajeta.math`; it is a Layer-2 sibling of
  `dev.cajeta.torch` / `.keras` / `.pandas` (analysis §4.3).
- Every multi-value return is a **record** (`records-spec.md` §5.2 names this exact use:
  "replacing scipy-style positional tuple bags").
- Sparse inputs/outputs are the **sparse array** of `nucleo-sparse-linalg-spec.md` — sparse
  *arrays only*, never the deprecated sparse *matrix* (analysis §3.4).
- Pure functions over `Tensor` (analysis §3.4 "the least framework-y" library) means the
  façade has **no global state** — no `set_default_*`, no module-level configuration.

> **TBD (plan-time):** [S1] Is each submodule a single class of static methods
> (`optimize.minimize(...)`), a namespace package, or a value object the user constructs?
> Lean: a class of static methods per submodule, matching `optimize.minimize` muscle memory.

## 2. Surface conventions (shared across submodules)

**Use cases**
- **2.1** As a SciPy user, when I call any façade method, then I pass arguments **by name**
  with defaults (`minimize(rosenbrock, x0: start, method: BFGS)`) rather than positionally
  with a trailing `options` dict — the call reads the same but the arguments are typed.
- **2.2** As a SciPy user, when a method would in SciPy return an `OptimizeResult` /
  attribute bag / positional tuple, then the façade returns a **named, typed record** whose
  fields I access by name (`fit.success`, `fit.x`, `fit.nIterations`) with full
  compile-time typing and typo-as-compile-error (§3, §6; `records-spec.md` §5.2).
- **2.3** As a developer, when a method returns a typed record, then I may destructure it
  where the sugar exists (`var (q, r) = linalg.qr(a)`) **or** access fields by name — both
  are equivalent (`records-spec.md` §5.3; destructuring is separable sugar).
- **2.4** As a developer, when I pass a `Tensor` to any façade method, then no Fortran-order
  / column-major contract is imposed or leaked — the C-order `Tensor` substrate is the only
  array contract (analysis §3.4 drop list).
- **2.5** As a developer, when an algorithm fails to converge or receives a malformed input,
  then it **fails loud** — a typed error or a result record whose `success` field is
  explicitly `false` with a reason — never a silent NaN-laced array.

> **TBD (plan-time):** [S2] Convergence failure surface — a `success: bool` + `message`
> field on the result record (SciPy-recognizable) vs. a thrown typed error vs. both
> (record for soft non-convergence, error for malformed input). Lean: record field for
> non-convergence, error for malformed input.

## 3. optimize — minimize, root, least_squares, curve_fit, linprog (priority)

Pure functions over a user callable and an initial `Tensor`; every result is a **typed
record**, not `OptimizeResult`.

**Use cases**
- **3.1** As an ML/graphics developer, when I minimize a scalar objective
  (`optimize.minimize(rosenbrock, x0: start, method: BFGS)`), then I get a
  `MinimizeResult { Tensor<float64> x; float64 fun; bool success; int32 nIterations;
  String message; }` and read `fit.success` / `fit.x` by name (analysis §3.4;
  `target-experience.md` §5).
- **3.2** As a developer, when I supply an analytic gradient (`jac:`) or let the façade
  finite-difference it, then `minimize` accepts the gradient as a named argument and the
  result record reports whether the gradient was user-supplied.
- **3.3** As a developer, when I solve `f(x) = 0` (`optimize.root(f, x0:, method:)`), then
  I get a `RootResult { Tensor<float64> x; Tensor<float64> residual; bool success; ... }`.
- **3.4** As a developer, when I solve a nonlinear least-squares problem
  (`optimize.leastSquares(residualFn, x0:)`), then I get a typed record with the solution,
  the residual vector, the cost, and convergence status.
- **3.5** As a developer, when I fit a model to data
  (`optimize.curveFit(model, xdata:, ydata:, p0:)`), then I get a `CurveFitResult`
  carrying the fitted parameters **and** the covariance matrix as named, typed fields —
  not a bare positional `(popt, pcov)` tuple.
- **3.6** As a developer, when I solve a linear program
  (`optimize.linprog(c:, aUb:, bUb:, bounds:)`), then I get a typed `LinprogResult` with
  the optimum, the objective value, and feasibility/optimality status.

> **TBD (plan-time):** [S3] Method selection — an enum (`Method.BFGS`) vs. a string
> (`method: "BFGS"`). Lean: a typed enum so an unknown method is a compile error, with the
> SciPy string names as the enum-constant spelling.
> **TBD (plan-time):** [S4] Whether the objective callable is an ordinary function value,
> a `@Grad`-transformed function (so the gradient is compiler-derived, not finite-
> differenced — `target-experience.md` §2/§5), or both paths. Lean: both, gradient
> auto-supplied when the callable is `@Grad`-annotated.

## 4. signal — filters, convolution, spectral, resampling (priority)

**Use cases**
- **4.1** As a signal-processing developer, when I design a Butterworth filter
  (`signal.butter(order: 4, cutoff: 0.2)`), then I get a typed filter record (e.g.
  second-order-sections), and applying it reads like SciPy
  (`signal.sosfiltfilt(sos, raw)` — `target-experience.md` §5).
- **4.2** As a developer, when I convolve two signals (`signal.convolve(a, b, mode:)`) or
  correlate them, then I operate over `Tensor` with a named `mode` argument and a typed
  return tensor.
- **4.3** As a developer, when I take a spectrogram / periodogram / STFT
  (`signal.spectrogram(x, fs:)`), then I get a **typed record** of the frequency axis, the
  time axis, and the power matrix — not a positional 3-tuple I must remember the order of.
- **4.4** As a developer, when I resample a signal (`signal.resample(x, num:)` /
  `signal.resamplePoly(x, up:, down:)`), then the operation is a pure function over `Tensor`
  with named rate arguments.

> **TBD (plan-time):** [S5] Filter representation — expose `sos` (second-order sections)
> as the primary, recognizable filter record and demote `(b, a)` transfer-function tuples,
> or carry both. Lean: `sos`-first (SciPy itself now recommends it), `(b, a)` available.

## 5. spatial — KDTree, distance, ConvexHull/Delaunay (priority)

`cajeta.xpu` provides an **RT-as-compute spatial index** (RTNN: hardware ray-query repurposed as
a kNN / radius / range accelerator — a compute primitive, graduated from the prism/caramelo
project into the xpu foundation, device-verified). It is the **GPU backend** for these queries,
**shared with robotica** (ICP / point-cloud nearest-neighbour). The façade names the SciPy
surface; the xpu spatial index backs the nearest-neighbour/radius queries (a CPU KD-tree remains
the fallback), invisibly to the user.

**Use cases**
- **5.1** As a graphics/ML developer, when I build a KD-tree over a point cloud
  (`spatial.KdTree(points)`) and query nearest neighbours
  (`tree.query(q, k: 8)`), then I get a typed `KnnResult { Tensor<int64> indices;
  Tensor<float64> distances; }` — not a positional `(d, i)` tuple.
- **5.2** As a developer, when I query all points within a radius
  (`tree.queryBall(q, radius: r)`), then I get a typed result of neighbour indices.
- **5.3** As a developer, when I compute pairwise distances
  (`spatial.distance.cdist(a, b, metric:)` / `pdist`), then I pass the metric by name and
  get a typed distance `Tensor`.
- **5.4** As a developer, when I compute a convex hull or Delaunay triangulation
  (`spatial.convexHull(points)` / `spatial.delaunay(points)`), then I get a typed record of
  the hull/triangulation topology (vertices, simplices) with named fields.

> **Resolved [S6] (2026-06-24):** `spatial.KdTree`'s nearest-neighbour/radius queries ride the
> **`cajeta.xpu` RT-as-compute spatial index** as the GPU backend (a CPU KD-tree is the
> fallback); the choice is **invisible** to the user (same recognizable surface either way). The
> index is an xpu compute primitive shared with robotica — not a scipy-private structure.

## 6. interpolate — splines, interp1d, griddata

**Use cases**
- **6.1** As a developer, when I build a 1-D interpolant
  (`interpolate.interp1d(x, y, kind:)`), then I get a callable interpolant object I invoke
  by `operator()` (`f(xnew)`), evaluating over `Tensor`.
- **6.2** As a developer, when I fit a spline (`interpolate.cubicSpline(x, y)` /
  `splrep`/`splev` analogue), then I get a typed spline record I evaluate and differentiate.
- **6.3** As a developer, when I interpolate scattered N-D data
  (`interpolate.griddata(points, values, query:, method:)`), then I operate over `Tensor`
  with a named method, returning a typed `Tensor`.

## 7. integrate — quad, solve_ivp (ODE)

**Use cases**
- **7.1** As a developer, when I numerically integrate a scalar function
  (`integrate.quad(f, a:, b:)`), then I get a `QuadResult { float64 value; float64
  errorEstimate; }` — value and error as named fields, not a positional `(y, err)` tuple.
- **7.2** As a developer, when I solve an initial-value ODE
  (`integrate.solveIvp(rhs, tSpan:, y0:, method: RK45)`), then I get a typed
  `IvpResult { Tensor<float64> t; Tensor<float64> y; bool success; String message; }` and
  read the trajectory by name (`target-experience.md` §5; analysis §3.4).
- **7.3** As a developer, when the ODE right-hand side is `@Grad`-differentiable or I request
  dense output, then those are named options on `solveIvp` — recognizable, additively typed.

## 8. special — gamma/beta/bessel/erf

**Use cases**
- **8.1** As a developer, when I evaluate a special function
  (`special.gamma(x)`, `special.erf(x)`, `special.betaln(a, b)`, `special.jv(v, x)`), then
  it is a pure elementwise function over `Tensor` returning a typed `Tensor`.
- **8.2** As a developer, when I evaluate a special function on a non-null numeric column,
  then it operates on the same bytes as a tensor (the column==tensor-buffer invariant,
  `nucleo-column-spec.md`) with no marshalling.

## 9. stats — distributions, tests

**Use cases**
- **9.1** As a developer, when I construct a distribution (`stats.normal(mean:, std:)`) and
  evaluate `pdf`/`cdf`/`ppf`/`sample`, then it is a typed distribution object with methods,
  not a frozen-`rv` grab-bag.
- **9.2** As a developer, when I run a statistical test (`stats.ttestInd(a, b)` /
  `ksTest` / `pearsonr`), then I get a **typed record** (e.g. `TestResult { float64
  statistic; float64 pValue; }`) — not a positional `(stat, p)` tuple.
- **9.3** As a developer, when descriptive statistics overlap what `cajeta.math` already
  ships (histogram/quantile/cov — analysis §3.4 "already covered"), then the façade delegates
  rather than re-implementing.

## 10. ndimage

**Use cases**
- **10.1** As a developer, when I apply an image filter
  (`ndimage.gaussianFilter(image, sigma:)` / `sobel` / `median`), then it is a pure function
  over a `Tensor` image with named parameters.
- **10.2** As a developer, when I label connected components or apply a morphological
  operation, then I get a typed result tensor (and a count where SciPy returns one as a
  second positional value → a named record field).

## 11. cluster

**Use cases**
- **11.1** As a developer, when I run k-means (`cluster.kMeans(data, k:)`), then I get a
  typed `KMeansResult { Tensor<float64> centroids; Tensor<int64> labels; float64 inertia;
  }` — not a positional tuple.
- **11.2** As a developer, when I run hierarchical clustering (`cluster.linkage(data,
  method:)`), then I get a typed linkage record consumable by a typed `fcluster` analogue.

## 12. Priority cut (v1 sequencing)

This is a **spec-level priority statement** (analysis §3.4 priority note, §5.2 build order),
not a plan — the actual schedule is plan-time.

**Use cases**
- **12.1** As the núcleo team, when we sequence v1, then **`optimize`, `signal`, and
  `spatial` ship first** — graphics and ML (training, filtering, nearest-neighbour) depend
  on them now.
- **12.2** As the núcleo team, when we sequence the remainder, then `interpolate`,
  `integrate`, `special`, `stats`, `ndimage`, and `cluster` are specified (§6–§11) but
  **deferred** behind the priority three.
- **12.3** As the núcleo team, when a submodule's numerics already exist in `cajeta.math`
  (fft/linalg/stats overlap — analysis §3.4), then the façade **binds to the existing
  engine** rather than scheduling new core work.

## 13. Acceptance criteria (spec-level)
- Every multi-value algorithm returns a **named, typed record** with field-access typing;
  no `OptimizeResult` bag, no positional tuple, no attribute grab-bag is exposed.
- All methods are **static methods on submodule classes**, called with **named arguments**;
  there are no global functions and no global mutable configuration state.
- Sparse inputs/outputs are the **sparse array** of `nucleo-sparse-linalg-spec.md` — the
  deprecated sparse *matrix* is never exposed; no Fortran-order/LAPACK contract leaks.
- `optimize`, `signal`, and `spatial` are the v1 deliverables; the rest are specified and
  deferred.
- Non-convergence and malformed input **fail loud** (§2.5).
- The façade re-specifies **no** engine internals — sparse, linalg, fft, and the columnar
  substrate are cross-referenced, not redefined.

## 14. Open questions (resolve at plan time)
- **[S1]** Submodule shape — class of static methods vs. namespace vs. value object (§1.4).
- **[S2]** Convergence-failure surface — record `success` field vs. thrown error vs. both
  (§2.5).
- **[S3]** Method selection — typed enum vs. string (§3).
- **[S4]** Objective-callable gradient — `@Grad`-derived vs. finite-difference vs. both
  (§3).
- **[S5]** Filter representation — `sos`-first vs. `(b, a)` transfer function (§4).
- **[S6] — RESOLVED:** `spatial` nearest-neighbour/radius queries ride the **`cajeta.xpu`**
  RT-as-compute spatial index (GPU backend; CPU KD-tree fallback), invisibly (§5).
- The exact field set of each result record (`MinimizeResult`, `IvpResult`, etc.) —
  recognizable-subset vs. full-SciPy-parity — resolved per submodule at plan time.
- Whether `special`/`stats` distribution objects are value records or stateful objects.
