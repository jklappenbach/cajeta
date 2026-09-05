# XPU cooperative-tile abstraction: one algorithm, optimally shaped per architecture

**draft** — filed 2026-09-04. Authored with the developer across the MXFP4/
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

## 9. Open design forks (for the developer)

- **9.1** `mac`/`Group`/`Tile` as NEW compiler builtins (like
  `CooperativeMatrix`, `Vector.dotSum`) vs a stdlib abstraction composed over
  the existing builtins. Builtins give the compiler full shape control; stdlib
  is cheaper to land. Recommendation: stdlib composition first for the spike,
  promote to builtins where the compiler must own the shape.
- **9.2** Wave width as a runtime `waveWidth()` value (one binary, no
  recompile, but can't size compile-time extents/unroll) vs `@Wave(width=N)`
  compile-time constant (static extents, per-width instantiation). The tiled
  kernels' LDS extents likely force compile-time; settle on `q4kQ8MmqKernel`.
- **9.3** How `mac`'s tier selection relates to the existing
  `CooperativeMatrix` `ImplTier` — generalize the same seam, or a new one.

## 10. Acceptance

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
