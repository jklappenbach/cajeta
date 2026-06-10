# Cajeta Toffee — ML / compute framework (seed)

**Status: seed / early.** This is the first real content for Toffee, the separate
plan promised when the gpu/xpu/gfx specs were split. It is **not** a foundation
spec — Toffee is a *consumer* that sits on top of `cajeta-gpu` (foundation) and
`cajeta-xpu` (compute execution), the way the numpy/scipy/keras/torch/ete ports do.
It is sequenced **after** those, and gated on specific `cajeta-gpu` Part C items
(ray query, cooperative matrix). Most of this plan is forward placeholder; the one
**research-grounded** part is P1 (RT-as-compute), distilled from a verified deep
research pass (appendix).

Checkbox legend: `[x]` landed+tested · `[~]` partial · `[ ]` not started.
**Working agreement:** one increment at a time, tests + docs; commit only when asked;
**no attribution trailer**; stage files explicitly.

---

## What Toffee is

A new ML / scientific-compute framework written in Cajeta. Per the locked direction:

- **Similar to torch**, a sibling of torch/keras — not a layer they sit under.
- **Sits on top of torch conceptually**: borrows what works, **innovates / changes
  the API where it matters**, and **takes from whatever makes sense** (numpy/JAX/torch),
  builds on it, and **alters it to fit Cajeta's computing paradigm** (borrow-checked
  memory, single-language host+device, the `cajeta-xpu` kernel model).
- **Primary focus: the SPELA forward training algorithm** (`ml/spela-training`) —
  single-forward-pass, per-layer local-loss training with **no global backprop**.
  This is the defining design constraint and Toffee's main differentiator (see P2).

### Dependency position
```
cajeta-gpu (foundation: value types, math, textures, memory, + Part C: ray query, cooperative matrix)
   ▲                         ▲
cajeta-xpu (compute)     ┌────┴─────┐
   ▲                     │          │
   └─────────────────────┤  Toffee   │  ← this plan (consumer, like the ports)
                         └──────────┘
```
- Toffee does **not** belong in the foundation; it builds on it.
- The numpy/scipy/keras/torch/ete ports are **siblings** of Toffee, not parts of it —
  they remain their own separate plan(s). Toffee may *reuse* a ported ndarray/BLAS
  surface but is free to diverge.

---

## Part P1 — RT-as-compute spatial primitive (research-grounded)

The most distinctive thing Toffee can offer that torch/JAX do not: **hardware ray
tracing repurposed as a general-purpose spatial-index accelerator** — kNN/radius
search, clustering, Monte-Carlo transport, point-in-mesh, range queries — built on
the `cajeta-gpu` ray-query primitive (Part C / Stage C3.3). The deep-research pass
(appendix) makes the design concrete and flags the real risks.

**Core mental model: a `SpatialIndex`, not "rays."** Users build an index over points
/ AABBs and run massively-parallel queries; the "ray tracing" (degenerate rays,
AABB-as-index-entry, custom intersection) is **library-internal** and never surfaced.

### Stage P1.0 — `SpatialIndex` primitive
- [~] `SpatialIndex` over points / AABBs with query verbs: `knn(k)`, `radius(r)`, `contains(p)`, `range(box)`, `countWithin(r)` — **`countWithin` (fixed-radius, L-inf box neighbourhood) landed**; the other verbs are follow-ups.
- [x] Internal encoding (hidden): wrap each datum in an AABB; issue a degenerate near-zero-length ray; visit candidate AABBs — the verified RTNN pattern. **Landed** in `src/toffee/spatial/SpatialIndex.cajeta` (build BVH over half-extent boxes; internal `countWithinKernel` casts the degenerate ray + counts candidates; `countWithin` is the only public surface — rays never leak).
- [~] Lowers to `cajeta-gpu` ray query (Vulkan) / OptiX (NVIDIA) / compute fallback — **Vulkan ray-query path done + exec-verified on a real RADV device** (via the cajeta-gpu 3a/3b foundation); OptiX + compute fallback are P1.3 follow-ups.

> **P1.0 landed (2026-06-03):** first real Toffee code. `SpatialIndex.cajeta` + `cajeta-toffee` repo seed (README). Exec-verified through the cajeta JIT harness (`cajeta/test/xpu/ToffeeSpatialIndexDeviceTests.cpp`) on an **AMD Radeon 8060S (RADV STRIX_HALO)** — `idx.countWithin(...)` returns correct fixed-radius neighbour counts with the ray-tracing entirely hidden. Built on cajeta-gpu Part C inc 3a/3b (ray-query lowering + host BVH build), both now complete.

### Stage P1.1 — Custom-predicate callback
- [~] **Enabling primitive landed (2026-06-04):** candidate **primitive index** wired end to end — `RayQuery.candidatePrimitiveIndex()` → `OpRayQueryGetIntersectionPrimitiveIndexKHR` (cajeta-llvm fork, opcode 6023) → a concrete `SpatialIndex.radiusExact` exact-L2 verb, exec-verified on a real RADV device (`exactL2RefinementOnDevice`) + a GPU-free spirv-val guard test. This proves the candidate step can recover the indexed datum and apply a true-distance predicate.
- [ ] Still open — the **general** ergonomic form: a kernel-lambda visitor for the candidate step (gather / count / accumulate), so users supply a predicate without hand-writing a new kernel per query. `radiusExact` is currently one hardcoded predicate; generalize it. The intersection/anyhit analog, surfaced ergonomically; the encode-as-rays trick stays hidden.

### Stage P1.2 — Acceleration-structure lifecycle (first-class tunable)
- [ ] `build` / `refit` / `rebuild` with an **auto refit-vs-rebuild amortization policy** (research: a real-time update/rebuild "gradient" optimizer gave up to 3.4×)
- [ ] Dynamic-data ergonomics: per-timestep (MD neighbor lists) / per-epoch (training graphs) rebuild scheduling

### Stage P1.3 — Auto compute fallback (no silent cliff)
- [ ] Grid/cell-list kNN compute kernel as a fallback path
- [ ] Heuristic/auto-selector RT-vs-compute keyed on radius / density (research: large radius → cell-list wins; extreme density → RT slower than a 64-core CPU + OOM)

### Stage P1.4 — Precision safety
- [ ] Conservative / "watertight" query mode (ε-inflated AABBs; optional exact re-check pass) — research: single-precision intersection **leaks** with no built-in detection

### Stage P1.5 — RT → tensor handoff
- [ ] Spatial query emits index/neighbor buffers consumable directly by a cooperative-matrix (tensor-core) op — RT builds the kNN graph, tensor cores do the GNN/aggregation matmul. Ties `cajeta-gpu` Part C ray-query + cooperative-matrix together; directly relevant to SPELA/GNN workloads.

### Stage P1.6 — Honest dimensionality + dual execution modes
- [ ] Document that BVH-as-index is native 3-D; >3-D kNN needs projection/tiling adapters or a different path — don't overpromise high-dim
- [ ] Expose both **query-in-kernel** (default, ergonomic) and **full RT-pipeline** (when events map to stages, e.g. MC material-boundary hits) — the latter via `cajeta-gfx` RT-pipeline lowering

---

## Part P2 — SPELA forward training (primary focus) — stub

The canonical implementation is `ml/spela-training/src/` (PyTorch `spela_train.py`,
TF/Keras `spela_train_tf.py`). SPELA(O) trains each layer with a **local per-layer
cosine-similarity loss** against fixed per-layer class embeddings ("symmetric
vectors" on the unit sphere), **detaching each layer's input** so the whole pass is
forward-only (no global backprop). Implications for Toffee's design:
- [ ] **No reverse-mode autodiff required for the SPELA path** — a major simplification vs torch; the per-layer local loss + gradient is closed-form / single-layer. (Reverse-mode is only needed for torch-parity workloads — a separate, optional axis.)
- [ ] Per-layer eval + early-exit inference (`from_layer=k`); online personalization / drift adaptation as first-class API
- [ ] Map the per-layer local update onto the `cajeta-xpu` kernel + `cajeta-gpu` cooperative-matrix path
- [ ] Define the "symmetric vector" / unit-sphere embedding type on top of `Vector<T,N>`

## Part P3 — Core tensor/array & interop — stub
- [ ] N-D array / tensor type (reuse or diverge from the ported ndarray surface)
- [ ] Op set + a Cajeta-paradigm API (borrow-checked, host+device unified) — the "innovate where it matters" surface
- [ ] Optional reverse-mode autodiff (torch-parity path; SPELA does not need it)
- [ ] torch/numpy interop / import path for borrowing what works

---

## Research appendix — RT-as-compute (verified deep-research pass, 2026-06-02)

Method: 5 angles → 25 sources → 119 claims → 25 adversarially verified (23 confirmed,
2 refuted). **Headline caveat: all verified evidence is OptiX / CUDA RT-core based;
Vulkan ray-query — Cajeta's actual target — is *unconfirmed by the literature* and is
the #1 design risk** (being de-risked by the Vulkan ray-query SPIR-V probe,
`test/gpu/GpuRayQueryProbeTests.cpp`).

**Confirmed (high confidence):**
- **Core pattern = BVH-as-spatial-index with degenerate near-zero-length rays**; only origins inside a point's AABB trigger a custom intersection. (RTNN, PPoPP'22 — horizon-lab.org/pubs/ppopp22.pdf; github.com/horizon-research/rtnn)
- **kNN / fixed-radius (RTNN): 2.2×–44× over optimized CUDA libs, 65× over naive RT. RT-DBSCAN: 1.3×–4×.** (arxiv 2303.09655)
- **Monte-Carlo transport is the flagship:** OpenMC/Turing +2–20× (SC19 PMBS); RT-MMC 1.5–4.5× over OpenCL (arxiv 2511.22779); **RT2: 150–300× photon/electron, up to 135× neutron vs FLUKA** (iopscience 10.1088/1361-6560/adfda7).
- **Refit-vs-rebuild amortization is a first-class tunable** — a real-time gradient optimizer gave up to 3.4× (FRNN, arxiv 2601.15633).

**Refuted (0-3) — both consequential:**
- ❌ "Full raygen/closest-hit/miss + SBT pipeline is mandatory; only triangle geometry is RT-eligible." → **False.** Ray-query-in-an-ordinary-kernel with custom AABB primitives is viable — validates the smaller `cajeta-gpu` foundation path.
- ❌ "No-payoff regime is narrow (only big triangle-mesh line tracing)." → **False — broader.** RT loses in more cases than the optimistic story → Toffee must never force RT (P1.3).

**Failure modes (confirmed):** single-precision intersection leaks with no built-in
detection (→ P1.4); large radius favors GPU cell-list; extreme density → RT slower
than a 64-core CPU + OOM (→ P1.3).

**Named but UNVERIFIED this pass (treat as soft):** Sionna RT (wireless), room
acoustics, seismic tomography, CT line-integral projection, robotics motion planning,
GNN / point-cloud nets, differentiable rendering. NVIDIA **3dgrut** (RT-based Gaussian-
splat trainer, github.com/nv-tlabs/3dgrut) surfaced as a concrete ML-pipeline datapoint.

**Open questions (carry forward):**
- Vulkan/SPIR-V ray-query vs OptiX — which encodings work *without* the full pipeline? (probe targets this)
- Are RT-core and tensor-core workloads ever fused in one kernel? (relevant to P1.5)
- How far does BVH-as-spatial-index extend beyond 3-D? (relevant to P1.6 / ML kNN)
- RT-core speedups for the unverified domains above?

**Framework abstractions to study:** NVIDIA OptiX, **OWL** (node-graph layer over
OptiX — ingowald.blog / github.com/owl-project/owl), Vulkan ray-query +
acceleration-structure spec (docs.vulkan.org).

---

## Sequencing & dependencies
- [ ] **Blocked on `cajeta-gpu` Part C**: P1 needs ray query (C3.3); P1.5/P2 need cooperative matrix. P1 cannot start before the C0 fork pipeline + ray-query lowering land.
- [ ] **Blocked on `cajeta-xpu`** for the kernel/dispatch execution model.
- [ ] First concrete step already taken: the Vulkan ray-query SPIR-V expressibility probe (de-risks the headline caveat before Toffee commits to the encoding).

*Toffee is a consumer of the foundation, focused on SPELA forward training, with an
RT-as-compute spatial primitive as its distinctive early capability. This is a seed —
P2/P3 fill in once the foundation's Part C items are real.*
