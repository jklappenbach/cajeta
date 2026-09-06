# XPU cooperative-tile abstraction: one algorithm, optimally shaped per architecture

**active** — filed 2026-09-04, approved 2026-09-05; Phase A (units 0–6) merged
to main 2026-09-05, reviewed and amended 2026-09-06 (§12). Authored with the developer across the MXFP4/
coop-kernel work that motivated it (this session): coopQ8 beat llama.cpp on
gfx1151, but only as one of four hand-written shapes of the same math (scalar /
fast3 / coop / coopQ8), gated per backend, wave32-only, and silently wrong on
wave64. The lesson generalized: the *algorithm* is written once, but the
*shape* it must take to run well differs by hardware architecture, and today
that difference is paid in hand-maintained kernel variants and backend-gated
dispatch.

## 1. Definition

### 1.1 Purpose
A general `cajeta.xpu` surface for writing a compute kernel **once**, as a pure
algorithm over abstract cooperative groups and tiles, which the compiler then
**organizes into the optimal shape for the target architecture** — wave width,
the multiply-accumulate primitive (tensor-core / SIMD integer-dot / scalar), the
reduction primitive, and the launch geometry. This is the Halide
*algorithm / schedule* separation applied to cooperative GPU kernels, with a
schedule vocabulary built for wavefronts and tensor cores rather than image
stencils.

### 1.2 Why (the measured motivation)
- coopQ8 vs coop vs fast3 vs scalar are the SAME dot, reshaped: it is
  wasteful and error-prone to hand-write and hand-dispatch each.
- The wave-width literal (`gid/32`, `&31`, `+32L`) is silently wrong on
  wave64 (CDNA); a whole-wave reduce over a quant block is wrong when the wave
  is wider than the block. These are architecture facts the author should not
  be hand-encoding per kernel.
- The portable *software tile* is correct everywhere but ~87–2800× slower than
  native (measured, gfx1151, this session): "correct-everywhere" must not mean
  "abandon the hardware." The abstraction must select the fast native primitive
  where it exists and fall to software only as a floor.

### 1.3 Scope
In scope: the author-facing surface (`Group`, `Tile`, `mac`, `reduce`,
work-partitioning), its per-target lowering, a single `TargetDescriptor` device
model that both this surface and future consumers read, and the extensibility
**seams** a later multi-kernel scheduler will plug into.

### 1.4 Non-goals (explicit)
- **The multi-kernel / utilization scheduler itself** — fusion passes,
  concurrency, graph capture, occupancy packing. This spec RESERVES its seams
  (§7) and builds none of its machinery.
- **Autotuning / auto-derivation of the optimal schedule.** Schedules are
  defaulted and author-declarable here; search comes later.
- **Full device introspection.** Wave width is reportable today; CU count,
  LDS budget, and bandwidth are not (the `geometry-is-literals` gap). The
  `TargetDescriptor` names them but may serve stubbed/estimated values with a
  TODO until the device can report them.
- **Domain concepts.** No token / layer / attention / expert vocabulary enters
  this surface. It is general compute; the LLM engine, `cajeta.ml`, and a
  Vulkan-compute game are all CLIENTS, none privileged.

### 1.5 Placement (load-bearing)
The surface lives in the general **`cajeta.xpu`** layer (compiler builtins +
stdlib), NOT in `cajeta-llm`. This is the one decision that is cheap now and an
expensive extraction later: a general abstraction hosted inside the LLM engine
would accrete LLM assumptions. Validation therefore requires **two clients from
day one** (§8): an LLM kernel and a non-LLM kernel.

### 1.6 Relationship to `nvptx-coopmatrix-native-ops-incomplete`
The 5 residual nvptx failures in that spec are WMMA quant kernels that name the
native-only lane-column ops (`fromWords`, `scaledAccumIntoS`,
`scaledAccumInto2S`) directly, which nvptx cannot lower. Once `mac` exists
(§4.1), those kernels express `mac` instead, and the native-op availability
becomes an INTERNAL lowering choice (WMMA where native, dp4a/scalar otherwise) —
so the failures close as a *consequence of adopting this surface*, not as a
per-op nvptx port. That spec's tactical fix (the `--strict-device` net plus a
native nvptx path, verified on the 4090) remains the near-term bridge; this
surface is the durable subsumption. The two are complementary, not competing.

## 2. The algorithm / schedule split

- **2.1** When a kernel is expressed on this surface, the algorithm body
  contains only the math and abstract collective ops — no wave-width literal, no
  choice of tensor-core-vs-dot, no launch geometry.
- **2.2** When the same algorithm is compiled for a different target, it
  produces a different concrete shape with identical results (to within the
  documented numeric tier, e.g. Q8-activation error), with no change to the
  algorithm source.
- **2.3** When no schedule is declared, the compiler derives a default shape
  from the algorithm and the `TargetDescriptor`.
- **2.4** When a schedule attribute is declared (§6), it overrides the default
  for that target, and the compiler verifies it is legal for the algorithm.

## 3. The cooperative group

A `Group` is an abstract cooperative unit of `width` lanes. `width` resolves to
a per-target compile-time constant (folds from `waveWidth()` / `@Wave`), so it
is free on the hot path.

- **3.1** When an algorithm asks for `group.rows(n)`, the compiler maps the `n`
  outputs onto groups (one row per group, or a tile of rows), sizing the launch
  from the descriptor — the author never writes grid/block.
- **3.2** When an algorithm asks for `group.stripe(n)`, the group's lanes stride
  the `n` work items (lane L takes L, L+width, …), coalesced by construction.
- **3.3** When an algorithm calls `group.reduce(op, x)`, it reduces across the
  group's lanes via the target's native primitive (DPP butterfly / shuffle /
  identity on width-1 CPU).
- **3.4** When the logical reduction span is a block SMALLER than the group's
  width, `group.reduce` MUST reduce per aligned segment (the segmented reduce),
  so a wave wider than the block never mixes two blocks' results. A whole-group
  reduce that silently spans two blocks is the failure this forbids.
- **3.5** When compiled for the CPU backend (width 1), the group ops degrade to
  scalar/serial equivalents and remain correct.

## 4. The tile and the multiply-accumulate

A `Tile<T, R, C>` is an abstract fragment; `group.mac(acc, a, b)` is the abstract
tile multiply-accumulate. The author never names WMMA, `fromWords`, `dotSum`, or
a scalar loop.

- **4.1** When the target exposes a native tensor-core config for the tile's
  dtype/shape, `mac` lowers to that WMMA path (and any lane-relative fragment ops
  it needs — e.g. `fromWords`, `scaledAccumIntoS` — are INTERNAL to that
  lowering, never author-visible).
- **4.2** When the target has no tensor-core config for the dtype but has a SIMD
  integer dot (dp4a / `v_dot4`), an int8 `mac` lowers to that (the coopQ8 path).
- **4.3** When the target has neither, `mac` lowers to the portable
  scalar/vector tile — correct, and flagged with the existing `[mma-tiering]`
  note, so "slow but correct" is never silent.
- **4.4** When two tiles in one kernel would straddle tiers, the kernel is
  demoted as a GROUP (the existing `scanCoopMatrixTiers` rule), never
  per-tile — one GEMM agrees on one tier.
- **4.5** When an algorithm needs an activation in a target-specific form (f32
  vs Q8-quantized), the schedule chooses it; the algorithm requests an abstract
  activation tile and the numeric tier is a documented property of the shape.

## 4A. The numeric-tier contract

A shape's numeric fidelity is part of its contract — declared and tested, never
an accident. The one algorithm may take shapes at different tiers, and a shape's
tier must be explicit so a schedule change cannot silently move the numbers.

- **4A.1** When a shape is bit-exact (an f32 / fp32-activation shape), it is
  tested bit-exact against the reference — a tolerance is NOT accepted.
- **4A.2** When a shape uses a lossy form (Q8-quantized activations, a
  reduced-precision accumulate), its tolerance vs the f32 reference is a DECLARED
  property tested against that bound — e.g. the MXFP4 Q8 shape: ≤0.5% relative,
  0 rows beyond 2% (measured, this session).
- **4A.3** When a schedule changes the activation/accumulate form (f32 → Q8), it
  changes the shape's declared tier, and that change is explicit in the schedule,
  not a side effect of picking a faster `mac`.
- **4A.4** When two shapes of the same algorithm are compared, they are compared
  at the WEAKER shape's declared tier — a lossy shape is never asserted
  bit-exact against an exact one (the trap that would make the Q8 shape look
  "wrong" against the f32 scalar).
- **4A.5** When a shape's realized error exceeds its declared tier, that is a
  test FAILURE, not a re-tuning of the bound — the bound is the contract.

## 5. The target descriptor (device model)

- **5.1** When any code needs a hardware fact (wave width, and — as they become
  reportable — CU count, max waves/SIMD, LDS budget, native MAC configs), it
  reads a single `TargetDescriptor`, never an inline literal.
- **5.2** When a fact is not yet reportable by the device, the descriptor
  serves a documented estimate/stub, so callers compile against the real
  interface now and gain accuracy later without a call-site change.
- **5.3** When the wave-width work (the coop kernels) is authored, it reads
  width from the descriptor — this spec is where `geometry-is-literals` starts
  being paid down, once, for every consumer.

## 6. Schedule specification

- **6.1** When the author declares `@Tile(...)` / `@Wave(width=...)` /
  `@Schedule(...)` on a kernel, those pin the shape for that target and the
  compiler verifies legality.
- **6.2** When the author declares nothing, §2.3's default applies.
- **6.3** The schedule carries a **policy** parameter (throughput | latency |
  frame-budget) — RESERVED as a seam (games need frame-time, training needs
  throughput). This spec accepts the parameter and defaults it to throughput;
  it does not implement alternate policies.

## 7. Extensibility seams for the future multi-kernel scheduler (reserved, thin)

The multi-kernel utilization scheduler (fusion / overlap / graph capture /
occupancy packing) is out of scope (§1.4), but its plug-in points are reserved
now because they are cheap to reserve and expensive to retrofit.

- **7.1** When a kernel is scheduled, it emits a machine-readable **resource
  descriptor** (VGPR / LDS / wave count / grid shape / memory-vs-compute
  character). The per-kernel schedule already computes these to lower; this
  requires only that it also RECORD them. Nothing consumes them yet.
- **7.2** When a kernel is launched, the invocation goes through a **submit
  seam** (kernel + buffers + grid + read/write sets) that today launches
  immediately. A later scheduler intercepts it to reorder / fuse / graph-capture
  / overlap, with no change to call sites.
- **7.3** When a kernel's read/write buffer sets are recorded at submit, the
  inter-kernel dependency DAG is derivable later for free.
- **7.4** The resource descriptor (7.1) must carry arithmetic character
  (compute-bound vs memory-bound), so one future cost model serves both
  underfilled decode/games and saturated training GEMMs.

## 8. Validation (two clients, day one)

- **8.1** When the surface is first proven, it is exercised by the **MXFP4
  matvec** (LLM client) and MUST reproduce coopQ8's numerics and its ~43 µs on
  gfx1151 — the abstraction cannot cost the win it is meant to generalize.
- **8.2** When the surface is proven, it is ALSO exercised by ONE **non-LLM**
  kernel — a plain tiled GEMM or a reduction/stencil standing in for a
  game / general-ML shape — correct on AMD and CPU. This is the guard that stops
  decode's assumptions from becoming the abstraction.
- **8.3** When compiled for AMD wave32 with no schedule override, a coop kernel
  on this surface emits ISA equivalent to the hand-written hardcoded-32 kernel
  (architecture portability is ~free — confirm, don't assert).

## 9. Design forks — SETTLED (Unit 0, 2026-09-04, measured on `q4kQ8MmqKernel`)

- **9.1 — SETTLED: stdlib composition first.** Every primitive the surface needs
  already exists (`Vector.dot`/`dotSum`/`lut4`, `Wave.reduceSumF32` /
  `reduceSumF32Segmented`, `waveWidth()` / `@Wave`, `CooperativeMatrix`), so the
  spike needs no new builtins. Promote a construct to a builtin only where the
  compiler must own the shape.
- **9.2 — SETTLED: split by kernel class, not one answer.** `q4kQ8MmqKernel` is
  NOT wave-width-coupled — its LDS is sized by the 64×64 TILE, it does per-thread
  `dotSum` with no `Wave.reduce`, and its `32`s are tile geometry (correct on
  wave64). So:
  - **wave-collective (`Group`) kernels** — `Wave.reduce` over a block, no
    LDS (the coopQ8 family) — use **runtime `waveWidth()` + segmented reduce**.
    No compile-time constant, no per-width monomorphization.
  - **tiled (`Tile`) kernels** — LDS sized by the tile — use **compile-time
    `@Tile` constants**; wave width there is an occupancy/schedule knob, not a
    correctness constant.
- **9.3 — SETTLED: reuse the `scanCoopMatrixTiers` / `ImplTier` seam** for
  `mac`. `Tile` is `CooperativeMatrix` generalized; the existing group-demote
  and `[mma-tiering]` machinery is the right home for `mac`'s tier selection.

## 10. Development phasing (forced by the verification boundary)

A shape is not "done" until the target's suite runs on it (the
`cpu-oracle-passed-a-uaf` rule: a lowering change is unverified until the device
suite confirms it). So each phase lands where its hardware is — this is a
constraint, not a preference.

- **11.1 Phase A — AMD + CPU** (authored and verified on gfx1151 + CPU, this
  box). The surface (`TargetDescriptor`, `Group`, `Tile`/`mac`), both witnesses
  (MXFP4 matvec reproducing coopQ8's ~43 µs on AMD + correct width-1 on CPU; a
  non-LLM kernel correct on AMD + CPU), the reserved seams, and the
  architecture-portability confirmation (wave32 ISA == hand-32). Full acceptance
  (§11) is achievable here. This is the plan's Units 1–7.
- **11.2 Phase B — NVIDIA (nvptx)** (emit-checkable here; numerics verified on
  the 4090, owned by that session). `mac`'s WMMA/dp4a selection on nvptx —
  native where a config exists, dp4a/scalar where the native-only lane ops are
  absent (§1.6). Intersects `nvptx-coopmatrix-native-ops-incomplete`: adopting
  the surface is what closes those 5 failures durably. A follow-on plan, gated
  on Phase A landing.
- **11.3 Phase C — later clients/backends.** Vulkan/SPIR-V (games-adjacent;
  `dot4add` exists) and `cajeta.ml` training as a second heavy client. Not this
  arc; named so the surface is not shaped in a way that precludes them.
- **11.4** The multi-kernel utilization scheduler (§1.4, §7) is a SEPARATE arc
  with its own spec, after the surface exists on at least Phase A + B — it
  consumes the seams this arc reserves.

## 11. Acceptance

- The MXFP4 matvec expressed once on the surface reproduces coopQ8 numerics and
  ~43 µs on gfx1151 (§8.1), and a non-LLM kernel is correct on AMD + CPU (§8.2).
- Architecture portability is confirmed ~free by ISA/measurement (§8.3), not
  asserted.
- The surface lives in `cajeta.xpu` with no domain vocabulary; `cajeta-llm` is a
  client (§1.5).
- The three seams (§7.1 resource descriptor, §7.2 submit, §5 target descriptor)
  exist as thin interfaces with no scheduler machinery behind them.
- No regression on the existing AMD suite; the software-tile floor still lowers
  where no native MAC exists.

## 12. Amendments — 2026-09-06 review

Phase A (units 0–6) is merged to main; its 25 device and CPU tests were
re-verified on a fresh build on 2026-09-06. The review found the following,
which the `xpu-tile-*` family now owns rather than this spec:

- **12.1** §6 (schedule declarations `@Tile` / `@Wave` / `@Schedule`, legality
  checking) and §4.5 (schedule-chosen activation form) were never planned in
  Phase A or B. They are specified by
  [`xpu-tile-manifest`](xpu-tile-manifest-spec.md) §5 and §11.
- **12.2** §7's seams shipped thin as intended, but the resource descriptor is
  author-filled (LDS typed at the launch site, VGPR a zero stub). The manifest
  spec replaces those fields with compiler-emitted values (§3, §12.2) and
  retires the author-facing constructor parameters.
- **12.3** The thunk-form submit was blocked by the kernel-launch-in-lambda
  crash; [`xpu-tile-scheduling`](xpu-tile-scheduling-spec.md) §2.1 specifies a
  value-typed submission instead, so the scheduler does not depend on that
  defect being fixed.
- **12.4** Witness A's launcher in cajeta-llm still computes threads from a
  literal 32 and documents itself as wave32-only; the kernel body is portable,
  the launch is not (manifest spec §13.1).
- **12.5** `TargetDescriptor.waveWidth()` on the host returns 1 on every
  backend; `Group.rowId()` is correct only for one-group-per-block launches; a
  `Tile` in two roles silently keeps the first; the `[mma-tiering]` note names
  `CooperativeMatrix` for a `Tile` (manifest spec §13.2–13.5).
- **12.6** No user documentation mentions `Group`, `Tile`, `TargetDescriptor`
  or `Scheduler`; the XPU specification's §6.3 still describes only the Vulkan
  subgroup surface. Docs are owed with the manifest spec's plan.
- **12.7** The Phase A and B plans live on the agents branch
  `worktree-cajeta-llama-unit-1`, not on agents/main, so the INDEX links dangle
  on main until that branch merges.
