# núcleo — specification index

`dev.cajeta.nucleo` is the consolidated core for porting the Python scientific and ML stack
to Cajeta. It is one substrate (tensor, columnar and GPU) with familiar façades (torch, keras,
scipy, pandas) over it. A greenfield native framework, caramelo, is deferred, so everyone
targets the núcleo core directly. The thesis is to port the contracts and leave the
implementations behind, to correct the upstream mistakes, and to make the result feel familiar
while being categorically better.

This directory holds the design docs and the spec set. Specs are requirements and use cases,
the why and the what. Build decisions, the how, are deferred as `> **TBD (plan-time):**`
markers and surfaced in each spec's Open Questions section, to be resolved when each spec
becomes a plan.

## Read first — design docs (the rationale)
1. **`python-stack-analysis.md`** is the landscape. It records what gets ported and what gets
   dropped per library, and why.
2. **`target-experience.md`** is the destination in code. It shows what finished Cajeta looks like.
3. **`language-foundations.md`** covers the two foundational language pieces, records and the
   annotation-synthesis and transform mechanism. It is audit-grounded and presents the safe
   two-tier model.

## The spec set, by layer (build order: top to bottom)

**Layer 0 — Substrate (DONE, not specced here):** `cajeta.math.Tensor` (numpy), `cajeta.xpu`
and codec IO. `cajeta.xpu` is the multi-target compute and kernel model for CPU, NVPTX, AMDGPU
and SPIR-V. `cajeta.gfx` is the sibling graphics layer and núcleo does not use it. The layer also
includes the shipped language enablers: named args, operator overloading, monomorphization,
first-class function types, closures, closure specialization, method-level templates,
intrinsic dispatch and reflection.

**Layer 1a — Language enablers** (small; leverage the shipped machinery):
| Spec | Covers |
|---|---|
| `records-spec.md` | Named immutable value-aggregates → typed schemas (`Table<Tick>`), typed returns. *(template-setter)* |
| `source-synthesis-spec.md` | Tier-A reusable source-synthesis facility + registry (the safe `@Logged`/codec pattern generalized). Carries `@Einsum`, `Table<T>` accessors. |
| `transform-intrinsics-spec.md` | Tier-B trusted transform intrinsics `Grad`/`Jit`/`Vmap`/`Pmap` (value-level combinators; `@`-annotations are sugar) + the VJP registry. |
| `syntax-sugar-spec.md` | The cheap ➕ tier: `@` matmul, `**`, unary `-`, bracket slicing, `a[i,j]`, `...`/newaxis, destructuring, type aliases, tensor masks. |

**Layer 1b — núcleo core** (on L1a + substrate):
| Spec | Covers |
|---|---|
| `nucleo-column-spec.md` | Arrow-laid-out columnar buffer + C Data Interface (no libarrow); the column == tensor-buffer invariant; Tensor Arrow retrofit. |
| `nucleo-expr-spec.md` | Lazy expression graph + fusion engine — shared by tensor ops AND dataframe ops. |
| `nucleo-autograd-spec.md` | The autograd engine — tensor-op VJP rules, forward/backward contract, eager tape, `Diff<T>`, `@NoGrad`/`@Checkpoint`. Consumes transform-intrinsics. |
| `nucleo-nn-optim-spec.md` | RETIRED — the neural spine moved to `dev.cajeta.ml` (`ml.nn` / `ml.optim`) and the stdlib packages were removed in v0.14.0. Kept for the design record. |
| `nucleo-frame-spec.md` | Polars-shaped lazy typed dataframe (`Table<T>`) + the pluggable index interface (zone-maps/B+/Z-order; R-tree/BVH/HNSW deferred). |
| `nucleo-sparse-linalg-spec.md` | Sparse *arrays* (CSR/CSC/COO) + sparse linalg; extended factorizations (qr/cholesky/lu/svd/eig) returning typed records. |

**Layer 2 — Verticals** (on the foundation):
| Spec | Covers |
|---|---|
| `torch-facade-spec.md` | The critical surface — recognizable PyTorch over núcleo; footguns corrected; `.pt` state-dict compat. |
| `keras-facade-spec.md` | High-level Model/`fit`/`compile` contract over the one núcleo core. |
| `scipy-facade-spec.md` | scipy by submodule (optimize/signal/spatial pulled forward); typed result records; sparse-arrays-only. |
| `pandas-facade-spec.md` | Optional, demoted recognizability skin over `nucleo.frame` (the primary API). |
| `flagship-splat-spec.md` | Differentiable Gaussian-splat rendering — the integration proof (df + tensor + autograd + render on one set of bytes). |
| `trees-spec.md` | Gradient-boosted trees (XGBoost lineage) — closed-form grad/Hessian, never touches autodiff; rides the frame. |

## Cross-cutting conventions (all specs)
- **Namespaces:** `dev.cajeta.nucleo.*` is the core and `dev.cajeta.{numpy,torch,keras,scipy,pandas}` are the façades. Stdlib `cajeta.math.*` is the substrate, and numpy's core lives there (curated, C-order-only). `dev.cajeta.numpy` is a thin `np.*`-named recognizability skin over it, consistent with the other façades, and is deferred until Python-porting demands it.
- **Annotations:** Cajeta PascalCase, as in `@Grad`, `@Jit`, `@Vmap`, `@Pmap`, `@NoGrad`, `@Checkpoint`, `@Autocast` and `@Einsum`. Transforms are value-level combinators first, and annotations are sugar.
- **Storage:** Arrow in-memory layout plus the C Data Interface, with no `libarrow` and no Arrow compute. A non-null column is a tensor buffer.
- **Dataframe:** Polars-shaped (lazy, expression, no implicit index, typed schema, nullable types). It is deliberately unlike pandas.
- **Façades:** recognizable rather than faithful. They correct upstream mistakes (see each spec's "drop the mistakes" section).
- **Autodiff:** a mid-level-IR pass, which is the moat. There is one VJP rule-set and two drivers, the compiled `Grad` and the eager tape.

## Status & next step
The foundation-first progression (`language-foundations.md` §3) sets the order. Records and
source-synthesis come first, then the transform intrinsics, then the núcleo core (column → expr →
autograd → nn/optim → frame → sparse/linalg), then the façades, with the splat flagship as the
first integration milestone. Each spec becomes a plan (`agents/cajeta/nucleo/<name>-plan.md`)
via the design skill, at which point its `TBD (plan-time)` markers are resolved.

Progress:
- **records** — ✅ complete (`records-plan.md`, 48/48).
- **source-synthesis** — ✅ complete (`source-synthesis-plan.md`, 42/42).
- **transform-intrinsics** — ✅ complete (`transform-intrinsics-plan.md`, 63/63, 2026-07-19).
  The ML-spine slice shipped the VJP registry, `Grad` (Tier-A backward), `@NoGrad`, `Vmap`,
  `Jit` and the `@Grad`/`@Vmap`/`@Jit` sugar. `Pmap`, Tier-B fusion, `@Checkpoint`/`@Autocast`
  and the higher-order rule slot are deferred to follow-ups. The real tensor-op rule content and
  the eager tape are `nucleo-autograd-spec.md`.
- **nucleo-autograd** — ✅ v1 increment complete (`nucleo-autograd-plan.md`, 2026-07-19).
  It widened the VJP rules (div/exp/log/sqrt/mean, scalar and tensor spellings), added the scalar
  eager tape (`cajeta.nucleo.autograd.{Tape,Var}`, which is define-by-run with runtime-bounded
  loops and stopGrad), and met the eager==compiled agreement bar. Deferred: tensor tape ops,
  literal registry sharing (generate tape source from the registry), `Diff<T>`, `@Checkpoint`
  remat, conv/softmax rules.
- **nucleo-expr** — ✅ v1 tensor increment complete (`nucleo-expr-plan.md`, 2026-07-20).
  The `Fuse` intrinsic and the `@Fuse` sugar fuse an N-op elementwise tensor chain into ONE loop
  allocating only the result, which is the headline bar and was measured. Reductions stage with
  the elementwise tail fused. The CALL is the force point, and there is no `.eval` because X5
  narrowed it away. The autograd seam is closed (`Grad(Fuse(f))` / `@Grad @Fuse` build one DAG
  for two consumers, and the backward is ordinary Jit-fusable IR). The column seam is pinned
  (`elementExpr` is the single dense-buffer site, contract-tested for the [X7] null-aware
  variant). Deferred: operator-spelled bodies (`a * b` needs a type-system decision, plan 4.2.2),
  the relational half (§4 pushdown, §7 column sharing, X7 nulls) with nucleo-column/frame, GPU
  lowering (X6), and the §3.4 fuse-vs-materialize heuristic.
- **nucleo-column** — ✅ v1 complete (`nucleo-column-plan.md`, 2026-07-21). This is the
  Arrow-laid-out columnar substrate. `Column<T>`/`NullableColumn<T>`/`StringColumn`/
  `MxColumn` are tensor-bit-identical when non-null, with zero-copy views both ways. Owned
  buffers are 64-byte-aligned (aligned start offset). The C Data Interface is matched-struct with
  no `libarrow`, with zero-copy export (live-buffer borrow, shells-only release) and zero-copy
  import (foreign-backed columns, releases exactly once, `materialize()` at the compute
  boundary). utf8 ("u") works in both directions, and the `cajeta.mxfp4` extension carries with
  physical-bytes degradation. The package is LAZY (it pulls cajeta.math). Deferred, recorded
  in-plan: Storage native mode (zero-copy tensor over foreign memory), boolean columns, 64-bit
  offsets, null-carrying utf8 import, nullable narrowing, the live pyarrow probe (needs the
  embedding seam; C-ABI conformance is consumer-tested in-tree), MX scales/kernels, codec readers,
  device story.
- **nucleo-nn-optim** — 🗑️ REMOVED in v0.14.0. The neural spine (Module/
  Parameter, Linear, Dropout, Losses, Modes; SGD/Adam/AdamW, schedules)
  now lives in the standalone `dev.cajeta.ml` library as `ml.nn` /
  `ml.optim`. That library is also where the tensor autograd, the module zoo,
  both trainers, checkpoints and LoRA live. The stdlib keeps only the
  foundational pieces, `nucleo.autograd`'s scalar tape and the
  compile-time `Grad` transform, which are compiler-integrated and have
  consumers beyond neural nets.
- **nucleo-frame** — 🔨 ACTIVE (`nucleo-frame-plan.md`, approved 2026-07-21, COMPLETE
  scope with no milestone split). It covers typed `Table<T>` with accessor/DSL synthesis, the
  lazy relational plan (filter/select/with/groupBy/agg/sort/join/resample/rolling/pivot/melt),
  pushdown optimization, gradual `Table<?>` typing, SQL/Polars nulls, the index interface
  with zone-maps, B+ and Z-order, CSV/Arrow boundaries, and full-fidelity parquet via
  cajeta-codec. Two compiler spikes lead (operator cross-type returns; wildcard-receiver
  member resolution).
- Everything below nucleo-frame is draft (specs written 2026-06-23; no plan yet).
