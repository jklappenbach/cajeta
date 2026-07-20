# núcleo — specification index

`dev.cajeta.nucleo` is the **consolidated core** for porting the Python scientific/ML stack
to Cajeta — one substrate (tensor + columnar + GPU) with familiar **façades** (torch, keras,
scipy, pandas) over it. (A greenfield native framework, **caramelo**, is deferred — everyone
targets the núcleo core directly.) The thesis: *port the contracts, not the implementations;
correct the upstream mistakes; make the result feel familiar but be categorically better.*

This directory holds the design docs and the spec set. Specs are **requirements + use cases**
(the *why/what*); build decisions (the *how*) are deferred as `> **TBD (plan-time):**` markers
and surfaced in each spec's Open Questions section, to be resolved when each spec becomes a plan.

## Read first — design docs (the rationale)
1. **`python-stack-analysis.md`** — the landscape, what gets ported vs dropped per library, and why.
2. **`target-experience.md`** — the destination in code (what finished Cajeta looks like).
3. **`language-foundations.md`** — the two foundational language pieces (records + the
   annotation-synthesis / transform mechanism), audit-grounded, with the safe two-tier model.

## The spec set, by layer (build order: top to bottom)

**Layer 0 — Substrate (DONE, not specced here):** `cajeta.math.Tensor` (numpy), `cajeta.xpu`
(the multi-target compute/kernel model — CPU/NVPTX/AMDGPU/SPIR-V; `cajeta.gfx` is the sibling
graphics layer, unused by núcleo), codec IO — plus shipped language enablers (named args, operator overloading, monomorphization,
first-class function types, closures, **closure specialization**, method-level templates,
**intrinsic dispatch**, reflection).

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
| `nucleo-nn-optim-spec.md` | Module/Parameter system + optimizers (explicit grads, no global state) — skinned by the torch/keras façades. |
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
- **Namespaces:** `dev.cajeta.nucleo.*` (core) · `dev.cajeta.{numpy,torch,keras,scipy,pandas}` (façades). Stdlib `cajeta.math.*` is the substrate — numpy's **core lives there** (curated, C-order-only); `dev.cajeta.numpy` is a thin `np.*`-named recognizability skin over it (consistent with the other façades), deferred until Python-porting demands it.
- **Annotations:** Cajeta PascalCase — `@Grad`, `@Jit`, `@Vmap`, `@Pmap`, `@NoGrad`, `@Checkpoint`, `@Autocast`, `@Einsum`. Transforms are value-level combinators first; annotations are sugar.
- **Storage:** Arrow in-memory layout + C Data Interface (no `libarrow`, no Arrow compute). Non-null column == tensor buffer.
- **Dataframe:** Polars-shaped (lazy, expression, no implicit index, typed schema, nullable types) — *not* pandas.
- **Façades:** recognizable, *not* faithful — they correct upstream mistakes (see each spec's "drop the mistakes" section).
- **Autodiff:** mid-level-IR pass (the moat); one VJP rule-set, two drivers (compiled `Grad` + eager tape).

## Status & next step
The foundation-first progression (`language-foundations.md` §3) sets the order: **records +
source-synthesis** first, then the transform intrinsics, then núcleo core (column → expr →
autograd → nn/optim → frame → sparse/linalg), then the façades, with the splat flagship as the
first integration milestone. Each spec becomes a **plan** (`agents/cajeta/nucleo/<name>-plan.md`)
via the design skill, at which point its `TBD (plan-time)` markers are resolved.

Progress:
- **records** — ✅ complete (`records-plan.md`, 48/48).
- **source-synthesis** — ✅ complete (`source-synthesis-plan.md`, 42/42).
- **transform-intrinsics** — ✅ complete (`transform-intrinsics-plan.md`, 63/63, 2026-07-19).
  ML-spine slice: VJP registry + `Grad` (Tier-A backward) + `@NoGrad` + `Vmap` + `Jit` +
  `@Grad`/`@Vmap`/`@Jit` sugar. `Pmap`, Tier-B fusion, `@Checkpoint`/`@Autocast`, and the
  higher-order rule slot are deferred to follow-ups; the real tensor-op rule content + eager tape
  are `nucleo-autograd-spec.md`.
- **nucleo-autograd** — ✅ v1 increment complete (`nucleo-autograd-plan.md`, 2026-07-19):
  widened VJP rules (div/exp/log/sqrt/mean, scalar + tensor spellings), the scalar eager tape
  (`cajeta.nucleo.autograd.{Tape,Var}` — define-by-run, runtime-bounded loops, stopGrad), and
  the eager==compiled agreement bar. Deferred: tensor tape ops, literal registry sharing
  (generate tape source from the registry), `Diff<T>`, `@Checkpoint` remat, conv/softmax rules.
- Everything below nucleo-autograd — **draft** (specs written 2026-06-23; no plan yet).
