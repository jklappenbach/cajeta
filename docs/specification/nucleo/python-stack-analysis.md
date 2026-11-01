# Núcleo — Analysis of the Python Scientific/ML Stack Port

> Status: draft for review (2026-06-22). This is an analysis rather than a spec. It frames
> the why, surveys each upstream library, and decides what gets ported and what gets
> dropped. The per-module specs (núcleo core, then the façades) follow from it.
>
> It reconciles and supersedes the earlier `CajetaTorch.md`, `CajetaToffee.md`, and
> `CajetaML.md` (all three since retired and deleted), which predate two facts. numpy is now
> done in stdlib `cajeta.math`, and the consolidation decision below replaces the
> "one big cajeta.ml library" framing with a consolidated core (`núcleo`) plus thin façades.

---

## 1. Purpose and positioning

### 1.1 What this document covers
We are bringing the familiar Python numerical/ML surface to Cajeta, which means torch, keras,
scipy, and pandas (numpy is already done), plus the adjacent gradient-boosting lineage.
The goal is an environment a Python developer can pick up with existing muscle memory,
while getting a categorically better engine underneath. This document analyses each
library, decides what is worth porting versus dropping, and maps the result onto a
consolidated core.

### 1.2 The competitive landscape — where *not* to fight
- **Mojo** is the Python-superset, MLIR-based AI-infrastructure play. Its proof points
  are HPC kernels (stencils, BabelStream, miniBUDE, Hartree–Fock) benchmarked against
  CUDA/HIP. Its superset promise is simultaneously its moat and its anchor, since every design
  decision must bend around duck typing, dunder semantics, a GC'd object model, and
  "existing Python just runs." That is a colossal fixed compatibility surface.
- **Julia** owns the "fast dynamic scientific" niche.

The crowded lanes are general numerical speed, AI kernels, and Python interop. Those are
exactly where incumbents have ecosystem gravity and, for Mojo, far more resources. We do not
fight there. The open ground is everywhere the AI-tensor crowd treats mathematical
structure as a nuisance.

### 1.3 Thesis — the port is the bootstrap; the type system is the moat
The port is the on-ramp. It is not the product. Familiarity gets a developer in the door, and
the reason they stay is expressiveness that exists nowhere else:

- **Type-level shape, unit, and index structure.** NumPy/torch shape errors are runtime, so
  `A @ B` mismatches blow up at execution. Cajeta's monomorphized templates and const-generic
  dimensions make those compile errors at zero runtime cost. No dynamic language can do that.
- **Language-level fusion.** `a*b + c*d` materializes every temporary in NumPy, and numexpr,
  Cython, and XLA-tracing all exist to work around it. Cajeta's zero-cost expression
  templates are the native answer.
- **Structure-aware autodiff.** PyTorch and JAX each bolt on autograd with incompatible
  rules. Differentiation that honours geometric structure exists nowhere.

The consequence for fidelity is that façades must be recognizable. They need not be faithful.
We owe the developer their muscle memory, and we do not owe bug-for-bug compatibility. Where an
upstream API encodes a genuine mistake, the core does not inherit it (see §2.5).

### 1.4 Two audiences
- **Familiarity-seekers** are Python developers who want a known surface. They are served by
  the façades (`dev.cajeta.torch / .keras / .scipy / .pandas`).
- **Greenfield** is served by the consolidated core directly (`dev.cajeta.nucleo`), which
  carries no familiarity obligation and is the best footing for new code. A first-principles
  native framework, **caramelo**, SPELA-based, is deferred. If built it would sit directly
  on the same core rather than as a façade. For now greenfield means the núcleo core directly.

Both draw from the same core. That is the whole architecture.

---

## 2. The consolidation thesis

### 2.1 Port the contracts, not the implementations
The Python scientific stack is a stack of API contracts over one array substrate, and the
five libraries are not separable implementations. The fracture comes from history (separate
projects, C/Fortran heritage) and never from design necessity:

- **numpy** is the substrate everyone speaks.
- **scipy** is "numpy + algorithms". It has the same array, more functions, plus one new type (sparse).
- **pandas** is "labeled, heterogeneous columns + IO" over numpy/Arrow buffers.
- **torch** is "numpy's array + autograd + device + nn" with a parallel API.
- **keras** is "torch.nn but higher-level", and Keras 3 is explicitly a contract over
  multiple tensor backends.

Cajeta has a one-time chance to present the familiar contracts on the outside while
leaving Python's internal fragmentation behind underneath.

### 2.2 One columnar-expression engine, many skins
A tensor, a dataframe column, and a Houdini-style geometry attribute are the same typed
columnar buffer. A fused tensor expression and a `filter → select → groupby` chain are
the same lazy expression-graph problem. So we build one columnar-expression engine
and put multiple APIs over it: tensor ops, a dataframe, geometry-attribute processing.
You do not port pandas and torch as separate efforts. You build the engine once, and the
familiar surfaces become skins.

### 2.3 The load-bearing invariant
A non-null numeric column == a 1-D tensor buffer == the same bytes. Arrow's null
representation is separable (a validity bitmap omitted entirely when null-count is zero),
so a non-nullable `Float64` column is bit-identical to a raw contiguous `f64` buffer. This
is a design invariant we build around rather than a coincidence to exploit, and it is what
makes the unified engine fall out for free. The fused and differentiable kernels operate on
dataframe columns and tensors without any conversion layer.

### 2.4 The storage contract — Arrow layout + C Data Interface, *not* the libraries
We adopt three things and reject three things, along a clean seam.

**Adopt:**
- The Arrow in-memory columnar layout, which means contiguous typed buffers, the separable
  validity-bitmap convention, and 64-byte padding.
- The C Data Interface (`ArrowSchema`/`ArrowArray`, a ~2-struct, frozen C ABI) for
  zero-copy in-process interchange with pyarrow/Polars/DuckDB. It is implemented by matching the
  structs, with no `libarrow` link.
- Arrow extension types to carry MX formats (MXFP4 etc.) as a logical type over a
  physical storage type. Interop degrades gracefully, since tools that don't know the type still
  move the bytes.

**Reject:**
- Arrow's compute kernels (ours).
- Arrow's logical type hierarchy (ours).
- Any `libarrow` dependency. A young language must not be an island, but it must also not take
  a heavy C++ dependency to avoid being one. The C Data Interface is exactly the escape.

This is how every modern columnar system is built, and it is no Frankenstein compromise.
Arrow is a format, and it won the layout war the way UTF-8 won text. Polars, DuckDB,
cuDF, and pandas-2 are engines built on top of it. We borrow the format (bytes), borrow
Polars' API shape as a design reference, and write the one layer only a typed compiled
language can: a typed, fused, differentiable engine.

> Device caveat: Arrow is host-memory-centric. The host side is the contract. The device
> representation is ours, with `ArrowDeviceArray` treated as a bridge when needed rather than
> as a foundation. Arrow's CPU-shaped assumptions must not leak into device buffer design.

### 2.5 Façades correct upstream mistakes (recognizable, not faithful)
A façade replicates an upstream API only as far as that API is sound. There is a three-tier rule:
1. **Silently correct** when the fix is observably compatible (e.g. type-based promotion
   instead of value-based, which is already true in `cajeta.math` via NEP-50).
2. **Better-by-opt-in** when the fix would diverge. Keep the familiar call working, expose
   the better path additively, and make true footguns warn or require an explicit flag.
3. **Refuse to port** the genuinely broken thing, and document why.

The appetite varies per library. It is aggressive for pandas (go Polars/Arrow rather than
classic pandas), conservative for torch (the critical surface, ported with minimal edits), and
moderate for scipy/keras.

---

## 3. Per-library analysis

For each library: *what it is · already covered · genuinely new work · port / drop ·
effort & risk · núcleo mapping.*

### 3.1 NumPy — **done** (the substrate)
- **What it is.** The n-dimensional array, dtype model, broadcasting, views/strides, and
  ufuncs. It is the lingua franca every other library speaks.
- **Status.** Complete in stdlib `cajeta.math`: `Tensor<T>`, `DType` (NEP-50 type-based
  promotion, so numpy's value-based promotion mistake is already corrected), 11 phases
  (creation/elementwise/reductions/shape/contraction/sort/fft/random/stats/linalg),
  GPU-lowered, `.npy` I/O for f32/f64/i32/i64. It is C-order only. The F-order/column-major
  creation knob (`MemoryOrder.F`/`emptyOrdered`) was dropped 2026-06-24, since we own GEMM/linalg
  and have no column-major BLAS/LAPACK dependency. Strided/transposed views and `.npy` F-read are
  kept. A type-level layout tag (annotation sugar) can reintroduce an `FContig` variant later if
  a concrete workload needs it.
- **Façade.** numpy's core is `cajeta.math` itself. A thin `dev.cajeta.numpy` `np.*`-named skin
  over it is optional and deferred, to be added only if Python-porting ergonomics demand the
  literal `np.` surface.
- **What it gives núcleo.** The tensor substrate. `Storage<T>` is already a single
  contiguous, C-order, dense and null-free buffer, which is already the non-null-column case
  of §2.3. `TensorProtocol` is a DLPack-style strided seam.
- **Only retrofit (additive):** 64-byte aligned allocation plus a C-Data-Interface
  export/import seam alongside `TensorProtocol`. It lands in stdlib `cajeta.math`.
- **Effort / risk:** done / n.a.

### 3.2 PyTorch — the critical surface
- **What it is.** A numpy-shaped tensor plus autograd, nn, optim, device, and serialization.
  It is the framework the field defaults to, and the single most important surface to capture.
- **Already covered.** Tensor + nearly all ops (`cajeta.math`); device model (`cajeta.xpu`).
- **Genuinely new.** The autograd engine (VJP/JVP rules, the MIR-pass driver, and an eager
  tape, see §4.4); the `nn.Module`/`Parameter` system; `optim` (SGD/Adam/AdamW + schedulers);
  a fiber-backed `DataLoader`; `state_dict`/`.pt` serialization compatibility; `amp`/autocast.
- **Drop (mistakes not to inherit).** Mutable global state (`set_default_dtype`/`device`,
  global grad toggles); `.grad` accumulation requiring `zero_grad`; the `.data` autograd
  escape hatch; in-place-op/autograd version-counter footguns; untyped shapes and
  runtime-only dtype/device. The better way is shape/dtype/device as type-level
  contracts (mismatch → compile error), and functional grad as the first-class path
  with the eager-accumulate style available as the familiar skin.
- **Fidelity stance.** Conservative. `torch.*` names, signatures, and argument order stay
  recognizable so a script ports with minimal edits, and the engine underneath is núcleo
  (compiled, fused, typed AD). The port is recognizable, and it is not faithful.
- **Effort / risk:** high / med-high (autograd correctness, `.pt` compat, the long op tail).
  It is mitigated because the tensor substrate already exists.
- **núcleo mapping:** `nucleo.autograd`, `nucleo.nn`, `nucleo.optim`; façade `dev.cajeta.torch`.

### 3.3 Keras — the high-level contract
- **What it is.** The high-level model/training API: `Model`, `Layer`, Sequential /
  Functional / subclassing, `compile`/`fit`/`evaluate`, callbacks, metrics. Keras 3 is by
  design a multi-backend contract over TF/JAX/PyTorch, and it owns no tensor or autograd.
- **Already covered.** Nothing directly. It is entirely gated on the nn/autograd core.
- **Genuinely new.** Thin: `Model`/`Layer`/`fit`/`compile`/callbacks over `nucleo.nn`.
- **Drop (mistakes not to inherit).** The v1/v2/v3 API churn; the three-way
  Sequential/Functional/subclassing split (collapse to one coherent model API); lazy
  `build()`/late shape inference (make shapes explicit and early via the type system); global
  backend state.
- **Strategic value.** Built as a high-level contract over the one núcleo core, keras becomes
  the high-level front door for the torch-shaped world. It is the cheapest high-value win once
  the core exists.
- **Effort / risk:** low-med (gated on core+nn) / low.
- **núcleo mapping:** façade `dev.cajeta.keras` over `nucleo.nn`/`nucleo.optim`, straight onto
  the one core (the same core the torch façade skins), with no backend choice.

### 3.4 SciPy — algorithms over the array
- **What it is.** numpy plus scientific algorithm modules plus one new core type (sparse).
  Modules: `linalg`, `sparse` + `sparse.linalg`, `optimize`, `signal`, `interpolate`,
  `integrate`, `special`, `spatial`, `stats`, `fft`, `ndimage`, `cluster`, `io`, `constants`.
- **Already covered.** A meaningful slice. `cajeta.math` shipped `fft`, `linalg`
  (solve/det/inv, more planned), `stats` (histogram/bincount/cov/quantile), and `random`. SciPy's
  fft/linalg/stats overlap is partly done.
- **Genuinely new.** `optimize` (minimize, root, least_squares, curve_fit, linprog);
  `signal` (filters, convolution, spectral, resampling); `interpolate` (splines, interp1d,
  griddata); `integrate` (quad, `solve_ivp` ODE solvers); `special` (gamma/beta/bessel/erf…);
  `spatial` (KDTree, distance, ConvexHull/Delaunay, where KDTree's kNN/radius queries ride the
  `cajeta.xpu` RT-as-compute spatial index, a compute primitive shared with robotica);
  `sparse` (CSR/CSC/COO + sparse linalg), the one new core type; `ndimage`; `cluster`.
- **Drop (mistakes not to inherit).** Ship sparse arrays only (scipy is itself
  deprecating the `np.matrix`-based sparse matrix); normalize inconsistent return
  conventions (ad-hoc tuples / `OptimizeResult` bags); keep Fortran-order/LAPACK out of the
  surface.
- **Character.** The least framework-y of the set, mostly pure functions over `Tensor`. It is
  bulk work, parallelizable, each module independently shippable, with low conceptual risk.
- **Priority note.** SciPy is second-tier (physics/engineering-adjacent) except for the
  pieces graphics/ML need now, which are spatial structures, signal, and `optimize` (training).
  Pull those forward and defer the rest.
- **Effort / risk:** med-high (large surface) / low.
- **núcleo mapping:** `nucleo.sparse` (type), `nucleo.linalg` (extends `cajeta.math`),
  algorithm modules; façade `dev.cajeta.scipy`.

### 3.5 pandas — the dataframe (→ Polars-shaped)
- **What it is.** `DataFrame`/`Series` (labeled, heterogeneous columns) plus IO plus groupby /
  join / reshape / time-series. It is the tabular workhorse.
- **The reframe.** The tabular abstraction of heterogeneous named columns with relational
  operations is necessary and fundamental. pandas' specific DataFrame is neither. The
  reference designs to crib from are Polars (no index; lazy; expression-based; a query
  optimizer that plans the chain before executing, which is the same fuse-the-expression-graph
  machinery as the tensor engine), Arrow (the layout, §2.4), and DuckDB (logical
  plan → optimizer → vectorized engine as the execution model).
- **Already covered.** The codec lib reads/writes Parquet/ORC/CSV/JSON (pyarrow-validated),
  but there is no DataFrame, and `ColumnVector<T>` was specified yet never built (the
  readers decode straight to raw arrays). So the column type is greenfield, Arrow-conformant
  from day one.
- **Genuinely new.** The Polars-shaped lazy typed dataframe (`nucleo.frame`) over
  `nucleo.column` + `nucleo.expr`; a groupby / join / reshape engine; time-series
  resampling (genuinely valuable, poorly served elsewhere); the index interface (§4.5);
  real nullable types.
- **Port (the relational/analytical core).** Projection, predicate filter,
  groupby-aggregate, joins, sort, window/rolling, reshape (pivot/melt), time-series resample.
- **Drop (the sins).** Index/MultiIndex implicit alignment; `inplace=`; the object-dtype zoo;
  NaN-as-missing (→ real nullable types); the copy/view ambiguity and `SettingWithCopyWarning`
  (Polars eliminated mutation entirely).
- **The distinctive version (only a typed language can).** Put the schema in the type via a
  named record rather than an anonymous structural type (see `records-spec.md`): `record Tick {
  Instant ts; float64 price; Symbol venue; }` then `Table<Tick>`. Column names and types known
  at compile time turn every `KeyError` and silent dtype coercion into a compile error and make
  `ticks.price` autocomplete. Haskell's *Frames* proves the idea. Nobody has done it in a language
  aimed at scientific users. Derived and intermediate frames are gradually typed, as `Table<?>`
  re-bound with `.as<R>()` (see `nucleo-frame-spec.md` §4.3).
- **Fidelity stance.** The most aggressive correction of any library. The primary API is
  Polars-shaped, and a `dev.cajeta.pandas` skin is an optional recognizability layer rather than
  the main surface.
- **Effort / risk:** med (the groupby/join engine perf is where dataframes live or die) / med.
- **núcleo mapping:** `nucleo.frame`, `nucleo.column`, `nucleo.index`.

### 3.6 The gradient-boosting lineage (XGBoost) — a third, independent core
- **What it is.** Tree-based tabular ML. Its "gradients" are closed-form first- and
  second-order derivatives of the loss evaluated pointwise (the grad and Hessian it grows
  trees against). They are never reverse-mode AD over a compute graph. Architecturally it shares
  almost nothing with backprop and never touches autodiff.
- **Relationship to the table.** It consumes a dataframe as an ingestion source rather than a
  working store. It iterates column-wise with dtype/null awareness, then it quantizes
  features up front into a bin-indexed histogram (`QuantileDMatrix`) and operates on that
  compact representation. The dataframe's job ends at ingestion, and the
  non-null-column==tensor-buffer invariant lets us feed the histogram builder with zero
  marshalling.
- **Reuse.** Its per-feature quantile binning is the same primitive as the
  zone-map / quantile-sketch in the index backlog (§4.5). That is one primitive in two roles.
- **Priority.** Off to the side. It is a faster, self-contained standalone win, independent of
  autodiff and graphics. It is lower priority than the DL+graphics spine, but cheap.
- **núcleo mapping:** `nucleo.trees` plus the shared quantile-sketch; the façade folds into a
  scikit/xgboost-shaped skin.

> **scikit-learn** (classical ML, meaning clustering, kNN/SVM/NB, trees, ensembles, PCA) is the
> tabular-ML cousin. It is a large but principled, consistent estimator API that maps mechanically
> onto the dataframe. It is lower priority, since it rides the second-tier dataframe path. It is
> noted here and not deepened.

---

## 4. What gets ported — the consolidated module map

### 4.1 núcleo core (`dev.cajeta.nucleo`)
```
nucleo.column    Arrow-laid-out columnar buffer (validity bitmap · offsets+data ·
                 64-byte align · C-Data-Interface export/import · MX extension types)
nucleo.expr      lazy expression graph + fusion engine (shared by tensor AND dataframe ops)
nucleo.autograd  one VJP/JVP rule-set + two drivers (MIR-pass engine · eager-tape skin)
nucleo.nn        module / parameter core      (skinned by torch.nn + keras)
nucleo.optim     optimizers + schedulers      (shared)
nucleo.frame     Polars-shaped lazy typed dataframe over column + expr
nucleo.index     pluggable index interface (+ zone-maps · in-memory B+ · Z-order)
nucleo.sparse    sparse arrays (scipy's one new type; sparse SOLVERS now external — §4.7)
nucleo.linalg    ~superseded (§4.7): factorizations live in cajeta.math.linalg, bags not records~
nucleo.trees     ~shipped EXTERNAL as cajeta-xgboost (§4.7)~
nucleo.geometry  geometry-attribute tables + splat tables (+ BVH over tensor buffers, not rows)
        builds on → stdlib cajeta.math.Tensor (numpy, done) + cajeta.xpu (device model)
```

### 4.2 The three ML lineages (distinct cores, shared substrate)
1. **Deep learning** is `Tensor` + `nucleo.autograd`, used directly or via façades `torch`/`keras`
   (a native `caramelo` framework is deferred).
2. **Gradient-boosting** is columnar ingest + histogram + trees (`nucleo.trees`), with no autodiff.
3. **Graphics** is BVH + differentiable rendering (`nucleo.geometry` + autograd + GPU).

They share the Arrow table and the quantile-sketch primitive. They do not share the
autodiff spine.

### 4.3 The façades (`dev.cajeta.*`)
`dev.cajeta.torch` (+ `.nn`, `.optim`, `.utils.data`, `.amp`, `.io`) · `dev.cajeta.keras` ·
`dev.cajeta.scipy` (split by submodule) · optional `dev.cajeta.pandas` skin over the
Polars-shaped `nucleo.frame`. A native **caramelo** framework (deferred) would sit directly on
núcleo rather than being a façade. For now greenfield code targets the núcleo core directly.

### 4.4 Autodiff placement — the keystone (decided: MIR-pass primary, tape skin)
There are three placements, and they are sharply different:
- **LLVM-IR level (Enzyme)** is post-optimization and language-agnostic, but the structure is
  gone. A contraction is just loops and loads, and the shape/index information that makes
  efficient reverse-mode possible has been erased. **Rejected.**
- **Source/AST level (Zygote)** sees all the structure, but it re-differentiates code the
  optimizer is about to rewrite. **Rejected.**
- **Mid-level IR pass (the cajeta MIR analog)** keeps contractions as contractions, keeps
  shapes and index variance visible, and sits above loop-and-load noise. The
  backward pass is emitted as ordinary IR that then fuses, DCEs, and remats like any
  other code. **Chosen.**

The decision is to build one set of VJP/JVP rules and two drivers over them. The MIR-pass
is the engine, where all optimization effort and benchmarks go, and a thin eager tape
is a compatibility/debug skin (the torch façade's define-by-run `.backward()` feel; dynamic
models). That is one rule-set with no duplicated autodiff. This is a compiler commitment and
not only a library one. It constrains the MIR so the AD pass sits where structure is still legible.

Why this and not pure eager? The flagship (§4.6) is a static compute graph over millions
of splats, and an eager tape dies by per-op dispatch before it renders. Compiled and fused AD is
load-bearing for the #1 deliverable. Furthermore, when an AI writes the code, the historical
ergonomic advantage of eager (define-by-run) weakens while cost-to-run, where compiled AD
wins, dominates.

### 4.5 Indexing — one interface is the only commitment
The pluggable index interface is the single retrofit-expensive commitment. Every
implementation is a deferrable backlog behind it. A column advertises what it can be indexed
by, a query asks for a capability, and the structure is swappable. Because Arrow batches are
immutable, indexes are bulk-loaded (bottom-up B+, STR-packed R-tree) and never
dynamic-insert.
- **Near-term:** zone-maps (per-chunk min/max, the cheapest, serving the common analytical
  scan), in-memory B+ (scalar sorted keys), Z-order-over-B+ (low-dimensional spatial table
  queries by reusing the B+ tree, so there is no second tree type).
- **Deferred, correctly placed:** R-tree (only when extended-object box queries are a
  demonstrated workload); BVH, which lives in the geometry subsystem over tensor buffers rather
  than table rows (a category line, since coupling it to the dataframe index would be an error);
  HNSW/IVF for high-dimensional NN (arrives with the embedding/Torch work).

### 4.6 The flagship — differentiable rendering on Gaussian splats
A 3D Gaussian splat scene is a table of millions of rows (position, scale, rotation
quaternion, opacity, spherical-harmonic colour coefficients, dozens of float columns).
Optimizing a splat scene is differentiable rendering, which is gradient descent over the columns
of a table. By §2.3 those columns are simultaneously Arrow columns (filter/slice/stream)
and tensor buffers (the fused differentiable kernels run over them directly, no marshalling).
The splat is where dataframe, tensor, autodiff, and rendering land on a single set of
bytes, one columnar engine viewed through four APIs. It is the most concrete proof that
the architecture earns its keep, and it hits both top-tier priorities (graphics + ML) at
once. It is on the roadmap as far-field LOD ("distant geometry collapses to splats").

> Boundary: the columnar table is the CPU-side authoring/optimization/storage form. At draw
> time, SoA columns transform to packed GPU vertex/index buffers, which is mechanical because SoA
> Arrow maps onto planar GPU buffers. Topology (mesh connectivity is a graph rather than a
> column) and acceleration structures (BVH over geometry) stay out of the table.

### 4.7 Amendment (2026-07-29) — the externalization precedent
Practice since this analysis was written (2026-06-23) amended the module map, and the
amendment is now doctrine (decided with Julian, 2026-07-29):

- **What happened.** The map placed gradient boosting at `nucleo.trees` (stdlib). It
  actually shipped as external `cajeta-xgboost` (`dev.cajeta.xgboost` 0.1.0, with its own
  repo, own CI, Olla publish, and own release cadence), and that model won. There is no toolchain
  coupling and no stdlib-digest churn, and the ecosystem machinery (CI templates, signed
  Olla publish, tour/docs conventions) made an external library cheap.
- **The rule going forward.** Core = types + numeric primitives, in the stdlib:
  `cajeta.math` (tensor, linalg, fft, poly, stats, random, special) and the núcleo
  mechanisms (`column`/`expr`/`frame`/`autograd`/`nn`/`optim`, and the sparse array
  type). Algorithm families and estimators = external `cajeta-*` libraries,
  commissioned per consumer. They are never monolith ports of an upstream package.
- **SciPy dissolves** along that line. Its one core type (sparse) → `nucleo.sparse`;
  dense factorizations/solvers → `cajeta.math.linalg` (`specs/linalg-solvers-spec.md`);
  `special` + small stats → `cajeta.math`; the algorithm families
  (`optimize`/`signal`/`spatial`/`interpolate`/`integrate`/`cluster`/`ndimage`) and the
  sparse solvers (`spsolve`/`cg`/Krylov) → per-domain external libs, on demand. The
  `dev.cajeta.scipy` façade stays as specced, an external thin skin over whichever
  engines exist. There is no `cajeta-scipy` monolith, because SciPy's module boundaries are
  historical accident rather than design.
- **scikit-learn → external `cajeta-ml`**, which owns the estimator protocol
  (fit/predict/score, metrics contracts); `cajeta-xgboost` conforms via a thin adapter.
  This resolves the §Open "unified estimator façade" question.
- **`nucleo.linalg` (record façade package) is superseded.** The stdlib keeps positional
  bags, and typed-record returns ride the external façades.
- **Pinned references** for porting fidelity: scipy v1.18.0 at `code/ml/scipy-ref`,
  scikit-learn 1.9.0 at `code/ml/sklearn-ref`, xgboost 3.1.2 at
  `code/cpp/xgboost-ref`.

---

## 5. Sequencing and priorities

### 5.1 Priority
Graphics and deep-learning ML come first, and physics/engineering comes second. Graphics and
DL share the tensor + autodiff + GPU + fusion substrate, and differentiable rendering is their
literal intersection and the flagship.

### 5.2 Build order
1. **Substrate (done):** `cajeta.math.Tensor`, `cajeta.xpu`, codec IO.
2. **núcleo core:** `column` (Arrow layout + C Data Interface) → `expr` (fusion) →
   `autograd` (rules + MIR pass + tape) → `nn`/`optim`. In parallel, the Tensor Arrow
   retrofit (alignment + C-Data-Interface seam).
3. **Flagship spike:** differentiable rendering on splats, to validate the shared engine.
4. **Façades:** `torch` (critical surface) → `keras` (cheap once core+nn exist).
5. **Dataframe track:** `frame` + `index` (zone-maps/B+/Z-order) → `scipy` modules
   (parallelizable; pull spatial/signal/optimize forward) → optional `pandas` skin.
6. **Independent:** `trees` (gradient boosting), a standalone win whenever convenient.

### 5.3 Effort & risk summary
| Surface | Effort | Risk | Gating dependency |
|---|---|---|---|
| Tensor Arrow retrofit | low (additive) | low | — |
| núcleo.column / .expr | med | low-med | — |
| núcleo.autograd (MIR pass) | high | med-high | compiler MIR |
| torch façade | high | med-high | autograd, nn, optim |
| keras façade | low-med | low | núcleo.nn |
| scipy modules | med-high | low | sparse, linalg |
| frame + index | med | med | column, expr |
| trees (XGBoost) | med | low | column, quantile-sketch |

---

## 6. Decision log

**Settled**
- Consolidated core `dev.cajeta.nucleo` ("núcleo") plus recognizable façades over it. Everyone
  (familiarity-seekers and greenfield) targets the one core. A native **caramelo** framework is
  deferred. It is not v1, and it would sit directly on núcleo if built.
- Port the contracts and leave the implementations. Façades are recognizable rather than faithful.
- One columnar-expression engine, with the non-null-column == tensor-buffer == same bytes invariant.
- Arrow layout + C Data Interface (no `libarrow`); MX as extension types; own engine/types.
- The dataframe is Polars-shaped (lazy, no index, typed schema, nullable types). pandas is an
  optional skin.
- Autodiff = MIR-pass primary, eager-tape skin, one shared rule-set (a compiler commitment).
- Index = one pluggable interface. zone-maps/B+/Z-order are near-term; R-tree/BVH/HNSW are deferred
  and correctly placed.
- Three ML lineages (DL / gradient-boosting / graphics), with the priority graphics + ML first.
- Flagship = differentiable rendering on Gaussian splats.
- Correction appetite: aggressive pandas, conservative torch, moderate scipy/keras.

**Open**
- Naming of the dataframe's primary surface (Polars-shaped native API name vs. leading with a
  façade name).
- Exact scipy submodule cut for v1 (which of optimize/signal/interpolate/integrate/special/
  spatial/ndimage/cluster ship first vs. defer).
- ~~Whether `nucleo.trees`/scikit get a unified estimator façade or separate skins.~~
  **RESOLVED 2026-07-29 (§4.7):** external `cajeta-ml` owns the estimator protocol;
  `cajeta-xgboost` conforms via a thin adapter.
- *(Deferred, not v1)* A native **caramelo** framework's relationship to `nucleo.autograd`. If
  built, whether its SPELA-style forward-only training consumes the MIR pass or bypasses autodiff
  (likely both paths). The nn/optim optimizer protocol is already general enough to host it.

---

*Next deliverable: the núcleo core spec set, split across the
column/expr/autograd/nn-optim/frame/sparse-linalg specs indexed by
`docs/specification/nucleo/README.md`, then per-façade specs.*
