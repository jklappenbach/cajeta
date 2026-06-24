# Cajeta Acceleration Structures — strategy-based BVH for GPU/CPU ray + collision queries

> Status: DRAFT (authored with the design skill). Goal: turn the single binary
> software BVH (`cajeta.gpu.Lbvh` + `SoftwareRayQuery`) into a **strategy-based**
> acceleration-structure facility — one logical contract, two encoding strategies
> (`Exact` / `Quantized`), with **width lowered from capability** and **backend
> orthogonal** — that builds **and** queries on the GPU, serves ray *and* broad-phase
> collision queries, and lets the developer **override and benchmark** to find what
> runs fastest on their hardware.

## 1. Definition

### 1.1 Purpose
Cajeta today has one acceleration structure: a binary, full-float32, threaded-DFS BVH
built on the host (`cajeta.gpu.Lbvh.build`/`buildSah`, native
`runtime/native/cajeta_bvh.c`) and traversed on-device by the single `@Device`
`cajeta.gpu.SoftwareRayQuery.step`. It is correct and portable but tuned for nothing in
particular. This feature reorganizes acceleration structures around **three orthogonal
axes** so the same source runs at the maximum capacity of each chip:

- **Strategy** — the bound *representation*, the only thing that defines a distinct
  encoding: **`Exact`** (full-float AABBs) or **`Quantized`** (8-bit compressed AABBs).
- **Width** — the branching factor (2/4/8/16). **Not a name** — a *lowering parameter*
  resolved from the target's SIMD/subgroup capability (or pinned by annotation).
- **Backend** — where it runs (GPU, CPU device-backend). **Orthogonal**: any strategy at
  any width runs on any backend; only the *economics* differ.

User code talks to one logical contract; the strategy/width are chosen by a
capability-driven lowering (overridable per site with an `@Accel` annotation), and the
GPU does both build and query.

### 1.2 Context — what exists, what's missing
- **Exists.** The frozen threaded-DFS float32 block (8-word header / 9-word binary nodes
  / primRef table); host builders (`Lbvh`, `cajeta_bvh.c`); the single device traversal
  `SoftwareRayQuery.step` (+ `slabHit`, value-type `SwRayCursor`); the
  reinterpret/quantization codec `cajeta.gpu.BvhCodec`; direction encoding
  `cajeta.gpu.Octahedral`; the algebra `cajeta.math` (`Aabb`, `Ray`, `Sphere`,
  `Rotation`, `Transform`, `Vector<float32,N>`). The capability machinery:
  compile-time traits (`cajeta.gpu.Capabilities`, resolved at codegen per `--xpu-arch`)
  + runtime `cajeta.gpu.Capability` / `Device.supports` (today's `RayQueryNative`
  dispatch), with the tiering precedent that `RayQuery` and `CooperativeMatrix` already
  auto-select a native vs software tier.
- **Missing.** (a) Any **wide** node format/traversal. (b) Any **GPU-side** build —
  construction is host-only and recursive. (c) Any **collision/overlap** query. (d) Any
  **strategy/width abstraction** or **override annotation** — the format is hard-wired.
  (e) Any **benchmark/exploration** facility.

### 1.3 Solution overview
1. **A logical contract, not a shared byte layout** (§2). Build primitives into an opaque
   structure; query it; the encoding is an encapsulated detail. The shared currency
   between strategies is the **query result** (identical hits/overlaps), not a block
   format. A self-describing header tags the strategy + width so dispatch is automatic.
2. **Two encoding strategies** (§3): `Exact` and `Quantized`. The `Exact` strategy pinned
   to width 2 is the **`Reference` role** — the correctness oracle and portable floor.
3. **Width as a lowering parameter** (§4): the node format + traversal are written once,
   generic over the branching width `N`; each concrete `N` monomorphizes to a fully
   specialized kernel (never a runtime-variable arity loop). `N` is resolved from the
   target.
4. **Capability detection + selection across execution models** (§5): the (strategy,
   width) pair is chosen from the **compile-time traits** baked into the binary × the
   **runtime `Device.supports`** probe — AOT pins or multi-versions the CPU path, the GPU
   path is always runtime-probed, JIT specializes to the host. An **`@Accel` annotation**
   overrides the auto-default per site.
5. **GPU-side parallel construction** (§6): Morton → parallel radix sort → Karras-2012
   fully-parallel radix tree → HLBVH coarse-grid + SAH top (Pantaleoni-2010) → parallel
   bottom-up AABB refit → Ylitie widening → optional DOBB post-process.
6. **Ray + broad-phase collision queries** (§7): closest-hit / any-hit rays, and
   AABB-vs-BVH / sphere-vs-BVH / BVH-vs-BVH overlap, all per-thread parallel.
7. **The DOBB optional tier** (§8): a bottom-up post-process giving `Quantized` interior
   nodes a shared discrete-rotation OBB — a per-node rotation index + a ray-transform in
   the existing slab loop, gated by a per-node bit.
8. **Verification + benchmark/exploration harness** (§9): every (strategy, width,
   backend) cell is cross-checked against the `Reference` oracle; the same scene builds
   through every enabled cell; a benchmark mode reports build time, throughput, and
   traversal-step counts so the developer can pick.

### 1.4 Constraints
- **Codegen discipline.** All `@Device` and host cajeta obeys the recorded traps (no
  int32 `>>>`; arithmetic `>>` only on non-negatives; float→int via the
  float64→int64→int32 chain or `BvhCodec`; no int64 multiply by a sign-bit constant;
  array-subscript-as-call-arg loaded into a local first). SIMD width is expressed via
  portable `Vector<float32,N>` so the lane count follows the target.
- **One specialized monomorphization per concrete width — never a runtime arity loop.**
  Width is a compile-time constant per instantiation; the wide inner loop is fully
  specialized/unrolled. Dispatch among instantiations happens at the *host/launch* level
  off the header tag, not per-ray inside a warp.
- **Strategy is backend-agnostic; fit is backend-dependent.** Every strategy at every
  width is *correct* on every backend (conservative quantized bounds still enclose the
  true box). Only the cost economics differ; the auto-selector matches strategy/width to
  the backend's capability profile.
- **Structure blocks are not byte-interchangeable** — only logically. Switching strategy
  or width means rebuilding the scene.
- **GPU-portable.** Device code targets the existing backends (Vulkan/SPIR-V, NVPTX, AMD,
  CPU device-backend); it must not depend on a vendor ray-tracing extension (those remain
  the *native* `RayQuery` tier, orthogonal to this software facility).
- **Exactness limits.** Integer fields use `BvhCodec` reinterpret where exactness beyond
  2²⁴ primitives matters.

### 1.5 Non-goals
- **A general strategy-override annotation mechanism.** `@Accel` is specified here as a
  concrete, BVH-specific annotation and the deliberate *prototype* for a future general
  "annotation-driven strategy selection for capability-tiered components" (unifying
  `CooperativeMatrix`, `RayQuery`, numeric kernels, and BVH). That generalization is a
  separate language-feature spec, to be designed once ≥3 instances exist — **out of scope
  here**.
- **Narrow-phase collision** (exact closest-point / signed distance / contact /
  penetration). This spec stops at broad-phase overlap candidates; narrow-phase is
  geometry-specific and layers on top later.
- **Dynamic / refit / incremental rebuild.** v1 is full rebuild per invocation
  (HLBVH-style). Refit and temporal reuse are a future spec.
- **Hardware-RT interop.** Consuming/emitting vendor BVH formats, or lowering these
  strategies onto hardware RT cores, stays out — the native `RayQuery` tier owns HW.
- **A third encoding strategy.** Only `Exact` and `Quantized`; OBB is the DOBB *tier* on
  `Quantized`, not a peer strategy.
- **A grid sort-and-sweep broad phase.** v1's broad phase is BVH-vs-BVH (§7.5). A uniform-grid
  spatial-subdivision broad phase — cell-ID/object-ID pairs, **parallel radix sort by cell
  ID**, then sweep equal-cell runs for candidate pairs, with the home-cell/phantom-cell
  scheme + multi-pass cell coloring to dedupe pairs and avoid write hazards (Le Grand,
  *GPU Gems 3* ch. 32) — is a **peer broad-phase facility**, not a BVH encoding: it is a
  sibling acceleration structure selected by scene/query shape, **not** a `Strategy` (which
  names only the BVH bound representation, §3). Its sweet spot is the BVH's blind spot —
  many small, similarly-sized, uniformly-distributed **dynamic** movers (particles, cloth,
  granular), where a grid rebuild is just re-hash + re-sort and a tree's refit/quality
  doesn't pay — while BVH-vs-BVH keeps the heterogeneous-size / sparse / mostly-static
  cases. It reuses the §6 parallel-radix-sort primitive directly and is a natural entry in
  the benchmark harness (§9) head-to-head against BVH-vs-BVH. **Out of scope for v1** (the
  13-unit path is correctness-first on the BVH); factored in here as the next broad-phase
  direction.

---

## 2. The acceleration-structure contract (logical interface)

The facade that hides strategy/width from user code: *build → opaque structure* and
*query → results*, plus the self-describing header. Requirements: query code is identical
across strategies/widths/backends; the structure carries its own strategy+width tag;
selection is automatic unless overridden.

**Use cases**
- **2.1** — *As a developer building a scene*, when I call `Bvh.build(boxes, count)`
  without annotation, then I receive an opaque structure whose strategy and width were
  chosen for the target, ready to query — with no strategy-specific code.
- **2.2** — *As a developer issuing queries*, when I run the same closest-hit ray query
  against structures built under different strategies/widths, then I get the **same hit**
  (within FP tolerance) and my query code is unchanged.
- **2.3** — *As the dispatch layer*, when a query is issued, then the structure's header
  strategy+width tag selects the matching specialized kernel — no caller involvement, no
  per-query branch in the inner loop.
- **2.4** — *As a developer switching encodings*, when I rebuild the same primitives under
  a different strategy or width, then I get a structure that answers identically but is
  encoded differently — the prior block is not reinterpreted.
- **2.5** — *As a tooling/debug consumer*, when I inspect a structure, then I can read its
  strategy, width, node count, and primitive count from the header without decoding nodes.

---

## 3. Encoding strategies — `Exact` and `Quantized`

The strategy is the **only** provider identity, and names exactly one thing: the bound
representation. Requirements: a `Strategy` enum with `Exact` and `Quantized`; `Exact`@2 is
the `Reference` role (oracle + portable floor); every strategy is correct on every
backend; the auto-selector lands on the strategy that fits the backend, and `@Accel`
(§5) can force any cell.

| | CPU-only | GPU |
|---|---|---|
| **`Exact`** (full-float AABBs) | natural fit — full-float into SIMD lanes, no decode | correct; larger nodes = more bandwidth |
| **`Quantized`** (8-bit AABBs) | correct; pays per-node decode, usually loses to `Exact` | natural fit — ~cache-line nodes, bandwidth win |
| **auto (no `@Accel`)** | → `Exact` @ SIMD width | → `Quantized` @ subgroup width |

**Use cases**
- **3.1** — *As a developer targeting a GPU*, when the auto-selector runs, then it picks
  `Quantized` (decoded via `BvhCodec`, stackless wide traversal) — the bandwidth-optimal fit.
- **3.2** — *As a developer targeting an AVX-512 CPU*, when the auto-selector runs, then it
  picks `Exact` (full-float bounds into one `Vector<float32,N>` slab test) — no decode.
- **3.3** — *As a developer forcing the off-diagonal*, when I pin `@Accel(Strategy.Quantized)`
  and run on a CPU, then it traverses correctly but pays an 8-bit→float decode per node
  (a deliberate memory-for-compute trade I can measure, §9).
- **3.4** — *As a correctness check*, when any (strategy, width) structure is queried, then
  its results match the `Reference` (`Exact`@2) structure built from the same primitives.
- **3.5** — *As a developer needing the floor*, when I target an unknown/portable backend
  or pin `Reference`, then I get `Exact`@2 — always works, byte-stable.

---

## 4. Width as a lowering parameter

Width is never a name; it is resolved at compile or JIT time and monomorphized.
Requirements: the node format + traversal are generic over a compile-time width `N`; each
`N` instantiates a fully specialized kernel; `N` comes from the target capability (§5) or
an `@Accel(width=…)` / `@Accel(maxWidth=…)` pin/cap; a runtime-variable arity loop is
forbidden.

**Use cases**
- **4.1** — *As a JIT execution*, when I run on the actual host, then the traversal is
  instantiated at exactly the host's best width (SIMD lanes / subgroup), nothing wider or
  narrower compiled.
- **4.2** — *As an AOT-pinned build* (`cpu=native`/`--xpu-arch`), when I compile, then the
  CPU path is monomorphized to the target's single width with no runtime width branch.
- **4.3** — *As a developer capping width*, when I annotate `@Accel(maxWidth=4)`, then the
  lowering never exceeds BVH-4 at that site even if BVH-8 would max the chip (e.g. memory
  budget).
- **4.4** — *As a performance engineer*, when I compare widths in the harness
  (`@Accel(width=4)` vs `width=8`), then each is a separate specialized kernel, so the
  comparison reflects real per-width cost, not a generic loop.
- **4.5** — *As a maintainer*, when a new width is needed, then it is a new instantiation
  of the one generic source — no second hand-written traversal.

---

## 5. Capability detection & selection across execution models

Selection of the (strategy, width) pair reuses cajeta's existing dual model — compile-time
traits (which variants exist in the binary) × runtime `Device.supports` (what the chip
advertises) — generalizing today's `RayQueryNative` dispatch. An `@Accel` annotation is
the explicit override. Requirements: an auto-default (max-capacity strategy/width whose
capability is satisfied); the three execution models below; the GPU-always-runtime
asymmetry; `@Accel` overriding per site.

**Execution models**
- **AOT, target-pinned** (`cpu=native`/`--xpu-arch`): compile-time trait bakes one CPU
  variant; no CPU probe; runs only on that microarch class.
- **AOT, portable exe**: several CPU variants compiled in; a **startup CPUID probe**
  dispatches the best (function multi-versioning) — one binary at "max capacity of each".
- **JIT**: probe the host and specialize directly; no multi-versioning.
- **GPU asymmetry**: the GPU is **always** runtime-probed via `Device.supports` (the
  physical device is enumerated at app start) — AOT or JIT alike; compile-time traits only
  decide which kernels were baked in to choose among.

**`@Accel` annotation (override)**
`@Accel(Strategy.Exact | Strategy.Quantized [, width=N | maxWidth=N] [, obb=true])`,
attached to an acceleration-structure declaration/build site; `@Accel(Reference)` is the
convenience alias for `@Accel(Strategy.Exact, width=2)`. It is the prototype for a future
general mechanism (§1.5 non-goal).

**Use cases**
- **5.1** — *As a developer shipping one portable exe*, when it launches on different CPUs,
  then a startup probe selects the widest `Exact` variant the CPU supports — AVX-512→16,
  AVX2→8, scalar→2.
- **5.2** — *As any exe on any machine*, when it initializes the GPU, then `Device.supports`
  picks the GPU strategy/width at runtime regardless of how it was compiled.
- **5.3** — *As a JIT user*, when I run, then the structure lowers to exactly the host's
  best (strategy, width) with no dead variants compiled.
- **5.4** — *As a developer needing determinism*, when I annotate `@Accel(Reference)`, then
  every machine builds the identical `Exact`@2 block and returns bit-reproducible results.
- **5.5** — *As a developer with a memory budget*, when I annotate `@Accel(Strategy.Quantized,
  maxWidth=4)`, then the structure is compressed and capped at BVH-4 on every target.
- **5.6** — *As a developer dodging a regression*, when a strategy is slow on a specific
  chip, then I pin the alternative at that site without touching query code.

---

## 6. GPU-side parallel construction pipeline

Construction runs device-side and in parallel. The pipeline composes the build literature
into one flow; quality is a requirement (SAH within a stated factor of a full sweep;
strict depth-first output for locality).

**Pipeline stages (requirements)**
- Morton codes per centroid (reusing `Lbvh.quantize`/`expandBits`/`morton3D`).
- **Parallel radix sort** of Morton codes (carrying primitive indices), GPU-side.
- **Karras-2012 fully-parallel binary radix tree**: every internal node computed by one
  independent thread (range direction + length by binary search; split by binary search;
  children by the index-coincides-with-range-end layout), no level-by-level sequencing,
  depth-first output; duplicate codes tie-broken by index.
- **HLBVH coarse + SAH top** (Pantaleoni-2010): a coarse `m`-bit Morton grid groups
  primitives into clusters; the **top levels** use an SAH sweep over clusters (overlap
  matters most there — touched by nearly all queries), the **bottom levels** the fast
  Morton/radix-tree path within clusters.
- **Parallel bottom-up AABB refit**: each thread walks leaf→root via parent pointers; an
  atomic per-node visit counter computes a node's box once both children are ready.
- **Ylitie widening** of the binary tree to the target width, SAH-driven, into the
  `Quantized` (or `Exact`) wide layout.
- **Optional DOBB post-process** (§8) as a final bottom-up pass.

**Use cases**
- **6.1** — *As a developer with a large/dynamic scene*, when I build on the GPU, then the
  whole hierarchy is constructed in parallel (no sequential top-level bottleneck) and
  scales with core count.
- **6.2** — *As a quality-sensitive consumer*, when I build with the SAH-top option, then
  the tree's SAH cost is within the stated factor of a full sweep, better than pure-Morton
  at the top.
- **6.3** — *As a traversal consumer*, when a structure is built, then its nodes are in
  strict depth-first order for cache-friendly traversal.
- **6.4** — *As a correctness check*, when a GPU-built structure is queried, then its
  results match a host `Reference` build of the same primitives (§3.4), proving the
  parallel build equivalent.
- **6.5** — *As a memory-constrained consumer*, when I build `Quantized` wide, then the
  node arena has no singleton holes and per-node footprint meets the cache-line target.

---

## 7. Query surface — rays + broad-phase collision

Rays + broad-phase overlap; narrow-phase is a non-goal. All query kinds run per-thread in
parallel on the GPU and on the CPU device-backend. v1's broad phase is BVH-based
(BVH-vs-BVH, §7.5); a uniform-grid sort-and-sweep broad phase is a future *peer* facility
for dense uniform mover sets — see §1.5 non-goals.

**Query kinds (requirements)**
- **Ray closest-hit** — nearest primitive along `[tMin, tMax]` (today's `SoftwareRayQuery`
  semantics), extended to all strategies/widths.
- **Ray any-hit / occlusion** — boolean with early-out (shadow rays).
- **AABB-vs-BVH overlap** — primitives whose boxes overlap a query AABB.
- **Sphere-vs-BVH overlap** — primitives whose boxes overlap a query sphere.
- **BVH-vs-BVH overlap** — candidate primitive **pairs** between two structures (broad-phase
  collision between two objects/sets).

**Use cases**
- **7.1** — *As a path tracer*, when I cast a closest-hit ray, then I get the nearest
  primitive and distance — identical across strategies/widths.
- **7.2** — *As a shadow-ray consumer*, when I cast an any-hit ray, then traversal
  early-exits on the first hit and returns a boolean.
- **7.3** — *As a GPU collision system*, when I test a mover's AABB against a scene BVH (one
  thread per mover), then I get its overlapping primitive candidates for narrow-phase.
- **7.4** — *As a GPU collision system*, when I test a bounding sphere against the BVH, then
  I get the overlapping primitive candidates.
- **7.5** — *As a GPU collision system*, when I run BVH-vs-BVH overlap between two objects,
  then I get candidate colliding primitive pairs (broad-phase), in parallel.
- **7.6** — *As any query consumer*, when I run the same query under `Quantized` and the
  `Reference` oracle, then the candidate set/hit is the same (§3.4).

---

## 8. DOBB optional tier (oriented-box quality on `Quantized`)

An **optional** post-process (DOBB-BVH, Kern et al. 2025) that tightens `Quantized` for
thin/elongated/rotated geometry by giving each interior node a **shared orientation** from
a small **discrete rotation set**, encoded in a few bits, children stored as AABBs in that
rotated space. Requirements: a toggle (`obb=true`), not the default; preserves query
results (pure speed); traversal stays the wide slab loop **plus** a per-node ray transform
gated by a has-OBB bit.

**Use cases**
- **8.1** — *As a developer with hair/foliage/cable geometry*, when I enable the DOBB tier,
  then a bottom-up pass assigns shared discrete-rotation OBBs where they reduce SAH cost,
  encoded as a per-node rotation index (~7 bits) + a small LUT.
- **8.2** — *As a traversal consumer*, when traversal reaches an OBB-bit node, then the ray
  is transformed by the decoded rotation before the existing slab test against the
  rotated-space child AABBs — one extra mat-vec, same inner loop.
- **8.3** — *As a correctness check*, when a DOBB structure is queried, then results match
  the non-DOBB `Quantized` and the `Reference` oracle (purely a traversal-cost optimization).
- **8.4** — *As a benchmark user*, when I toggle DOBB on a scene, then the harness reports
  the traversal-step reduction and the net time delta (transform cost vs steps saved),
  since the win is workload- and backend-dependent.
- **8.5** — *As an axis-aligned-scene developer*, when DOBB yields no SAH improvement at a
  node, then that node keeps its AABB (no OBB bit, no transform cost).

---

## 9. Verification and the benchmark / exploration harness

The developer-facing facility to **select, override, and switch** strategies/widths to find
what runs fastest, plus the oracle-based correctness net under it. Requirements: a single
benchmark entry builds the same scene through every enabled (strategy, width) cell on the
available backends; `Reference` is both oracle and baseline; comparable metrics reported.

**Use cases**
- **9.1** — *As a developer exploring options*, when I run the benchmark over a scene +
  query workload across enabled cells, then I get per cell: build time, query throughput,
  and avg/max traversal-step counts — so I can choose.
- **9.2** — *As a correctness gate*, when the harness builds every cell from one primitive
  set, then each cell's query results are cross-checked against `Reference` and any
  divergence fails the run.
- **9.3** — *As a developer selecting a default*, when I pick a strategy/width for a target,
  then my production query code uses it unchanged via the §2 contract (and `@Accel` if pinned).
- **9.4** — *As a DOBB evaluator*, when I include DOBB-on and DOBB-off `Quantized` in the
  matrix, then the harness quantifies DOBB's net effect on that workload (§8.4).
- **9.5** — *As a regression guard*, when strategies exist, then the host test suite runs
  each against the brute-force analytic oracle (extending the current `GfxLbvhTests`
  nearest-hit cross-check) for rays and overlaps.
