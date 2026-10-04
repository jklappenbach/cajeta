# xpu-tile-shape-selection — a tile's shape chosen by rule and measurement, not by hand

Status: **active**, approved by Julian 2026-10-03 in an interactive review (decisions in §7); plan `agents/xpu-tile-shape-selection-plan.md`. Filed 2026-10-02. Requested by Julian on 2026-10-02, in
the conversation that measured the TcTile reshapes: "How can we formulate
rules that will enable the business logic to identify this configuration
and employ it without forcing this work on kernel designers. Or is this
absolutely something that could not be automated?" He agreed to a spec the
same day.

## 1. Definition

A matrix-core kernel's performance turns on its **shape**:
- the workgroup tile (tokens by rows);
- the warp grid, and the warp tile each warp owns;
- the K step staged per barrier;
- how many stages are in flight;
- whether K is split across workgroups.

Today the author picks the shape by hand, for one device, by repeated
measurement. The cost is that the shape becomes part of the kernel's source.
It is re-picked by hand for every new part, and nothing records why it was
chosen.

This spec moves the choice out of the author's hands. The author writes the
tile ONCE, over its shape parameters. The compiler instantiates candidate
shapes, and a feasibility filter discards those the device cannot run well,
using rules computed from device facts and from the compiler's own verdicts.
The survivors are timed on the device, the winner is recorded in the kernel
manifest, and a route reads the winner from there. The author still owns the
ARITHMETIC: how scales fold, where a correction term goes, and what precision
each step keeps. Those are offered as named variants that the same machinery
chooses among (§5), never invented by it.

### 1.1 The evidence that motivates it (2026-10-01/02, cajeta-llm on an RTX 4090)

- The Q4_K prefill tile (`TcTile.q4kQ8TcTileKernel`) was shaped by hand
  over a day of measurement: 128 tokens x 128 rows, 8 warps as 4x2, 32x64
  warp tiles, 128-k half blocks, double-buffered, 255 registers. It reached
  parity with llama.cpp's prefill, and every alternative was found by
  writing a variant, building it (about 2 minutes) and timing it.
- Three reshapes were measured, and all lost:
  - 16 warps with 32x32 warp tiles: 128 registers with a 52-byte spill,
    10-15% slower.
  - 128x64 tiles: 170 registers, 15-20% slower.
  - 128x64 unrolled: 255 registers and a spill, a tie at best.

  Two of the three were rejectable WITHOUT a run, because ptxas reported a
  spill. A rule would have pruned them in seconds.
- The register cap is not a knob. 255 registers a thread is NVIDIA's
  hardware limit. What binds is the register file, 64K a multiprocessor, so
  every shape trades threads against registers inside a fixed budget. The
  rules have to reason in those units.
- `@Occupancy` did nothing on nvptx until cajeta a171d180 (2026-10-02),
  because the bound never reached ptxas. Every shape decision before it
  rested on an unbounded register allocation that happened to fit. A
  selection system must read the compiler's VERDICT (registers, spill,
  shared bytes per candidate), not assume the declaration took effect.

### 1.2 Scope

- matrix-core GEMM tiles first (the TcTile family, the WMMA Mw family);
- the shape parameters named above;
- nvptx FIRST, built and proven on sm_89 where the tile exists; amdgpu next
  (64 KB LDS, the wave64 option); cpu answers "no matrix core" through the
  probe that already exists (`Linear.matrixCoresPay`).

This spec belongs to the tile family, beside `xpu-tile-manifest` (whose
footprint fields it consumes) and `xpu-tile-scheduling`. It fills the gap
`xpu-kernel-adaptor` §1.3 leaves on purpose: "ranking survivors (tile
family)".

### 1.3 Non-goals

- **Inventing arithmetic.** An integer fold against a float fold, or a
  per-block against a per-sub-block correction, is a precision-bearing
  rewrite. The author supplies variants (§5). This spec only chooses among
  them.
- **The launch block at bind.** That is `xpu-kernel-adaptor` §4. This spec
  chooses which compiled kernel to bind, not its block.
- **Scheduling among kernels.** That is `xpu-tile-scheduling`.
- **A shipped table of shapes per part.** `xpu-kernel-adaptor` §1.4 forbids
  it, and this spec inherits that. A winner is measured per machine and
  cached, never committed.

## 2. The parameterized tile

The author writes the tile once, with its shape as template parameters, and
the fragments as arrays the compiler unrolls.

```cajeta
public final class QkTile<uint32 TM, uint32 TN, uint32 WM, uint32 WN, uint32 KS> {
    @Kernel @FastMath
    @Occupancy(maxWaves = (TM / WM) * (TN / WN))
    public static void q4kQ8(KernelBuffer<float32> y, ...) {
        CooperativeMatrix<float32,16,16,2>[WM / 16][WN / 16] facc;
        ...
    }
}
```

**Measured 2026-10-02:** a `@Kernel` inside a class template with a
`uint32` parameter is instantiated per argument, but its lowering does not
bind the parameter. It fails with "unbound identifier 'TM'" (probe:
`class Tile<uint32 TM>` with a kernel reading `TM`). So this feature starts
with compiler work.

Use cases:
- 2.1 An author writes the TcTile once over (TM, TN, WM, WN, KS). The 128x128
  / 4x2 / 32x64 shape of today is one instantiation of it, and the rejected
  reshapes are three more.
- 2.2 A kernel body reads TM, TN, WM, WN and KS as compile-time constants.
  Shared array sizes, loop trips and fragment counts derive from them, and
  every loop over fragments is fully unrolled, so a fragment array lives in
  registers.
- 2.3 `@Occupancy` takes a constant expression over the template parameters
  (`maxWaves = (TM / WM) * (TN / WN)`), so the bound travels with the shape.
  Constant expressions in annotation arguments are in this spec's compiler
  work.
- 2.5 The family LISTS its candidate shapes, under a cap the family
  declares (about 6). Every listed shape is lowered at build, so an AOT
  executable carries all of them, and the compile cost is known up front.
  The compiler does not enumerate a parameter grid.
- 2.4 A shape the body cannot serve is refused at instantiation, by name
  (for example WM not a multiple of 16), rather than miscompiled.

## 3. The feasibility filter

Before anything runs, a candidate must pass rules computed from two kinds of
fact.

- **Device facts, read from the driver and never tabled:** register file per
  multiprocessor, maximum registers a thread, shared memory per block with
  and without opt-in, multiprocessor count, wave width, and the matrix-core
  shapes the backend lowers natively.
- **Kernel facts, read from the compiler's verdict for the instantiation:**
  registers a thread, spill bytes, static and dynamic shared bytes. These
  already land in the kernel manifest.

The rules:
- R1, no spill. This is the existing gate (`[xpu-kernel-spill]`), applied
  per candidate. ANY spill prunes the candidate; a spilling shape is never
  timed.
- R2, the register file holds the workgroup: threads x registers is at most
  the register file.
- R3, shared memory fits, with the opt-in limit where the backend opts in.
- R4, at least one workgroup is resident per multiprocessor, and the
  candidate reports how many.
- R5, the grid at the problem's shape fills the multiprocessors, or the
  candidate carries a split-K that does (`TcTile.splitsFor`'s rule,
  generalized).
- R6, the shape divides the problem or pads it, and the padding cost is
  stated.

Use cases:
- 3.1 On sm_89 the 16-warp, 32x32 candidate is pruned by R1 (52-byte spill)
  without a timed run.
- 3.2 On a 64 KB-LDS part (gfx1151, Vulkan) the double-buffered 128x128
  candidate is pruned by R3. A smaller one survives, where today the whole
  family is `@Unlowered` there.
- 3.3 A pruned candidate is reported by name with the rule it failed, so an
  empty survivor set is a diagnosable result, not a silent fallback.

## 4. The measured choice

The survivors are timed on the device at the shapes the model actually runs.
For cajeta-llm these are the engine's projections at the prefill chunk.

- The probe is the existing pattern: `Linear.matrixCoresPay` for the "do
  matrix cores pay at all" decision, and `Autotune` with `tuneBuildId` for a
  per-machine knob (`QuantKernel.mvRowsPerBlock` is a working example).
- The probe runs at FIRST USE, within a budget of a few seconds, and its
  answer is cached, as `matrixCoresPay` and the rows-per-block tune are
  today. An explicit warm-up (`--prewarm`) can do the same ahead of time.
- The winner is recorded per (device, kernel family, exact (M, N, K)),
  keyed by the build id, so a rebuild that changes the kernel re-measures
  it. An LLM runs a handful of fixed projection shapes, so one entry per
  shape is exact and few; buckets were rejected because a bucket edge can
  pick the wrong shape.
- Timing uses the deferred stream and the event tier with its calibrated
  clock (`KernelTimer`), and interleaves candidates, because the box's clock
  is bimodal and a single sequential sweep has read 2x off.

Use cases:
- 4.1 First use on a new machine times the survivors once (a budgeted few
  seconds) and caches the winner. Later runs read the cache.
- 4.2 A user can force a shape for a sweep (`setShapeOverride`), as
  `setSplitOverride` does today.
- 4.3 The choice is visible: the route record names the shape it took, as
  `batch-route` names the kernel today.

## 5. Variants the author supplies

LATER UNIT, not the first version: v1 chooses among shapes only, and
variants join as a second candidate dimension once the machinery exists.

A family may carry more than one formulation of the same product. For the
Q4_K tile that is the integer per-sub-block fold, the float fold, and the
dual epilogue. Each variant:
- is held to the same precision contract by its own tests against the host
  oracle;
- enters the filter and the timing as one more candidate dimension.

**Measured 2026-10-02 on sm_89 at 4096x14336:** integer fold 280 us, float
fold 307, dual epilogue 295. The integer fold wins there, and the same may
not hold on another part. That is exactly the choice this spec automates.

## 6. The route reads the choice

A route asks the manifest which instantiation won for its problem class and
launches that one. It never names a backend or a part. The existing
`Linear.matmulBatchKeep` arms become ONE ARM PER FAMILY: the arm asks the
cache for the winning instantiation at its (M, N, K) and launches it.
Today's hand-picked TcTile shape becomes the family's default listed shape,
used until a winner is measured.

## 7. Decisions (Julian, 2026-10-03, in an interactive review)

- 7.1 Home: its own spec in the tile family, not folded into
  `xpu-kernel-adaptor` or `xpu-tile-manifest`.
- 7.2 Parameterization: class template parameters. The lowering must bind
  them (measured missing on 2026-10-02: "unbound identifier 'TM'"), and
  fragment arrays must unroll under constant-trip loops.
- 7.3 Candidate space: the author lists shapes, and §3's rules prune them.
- 7.4 Compile: every listed candidate is lowered at build, under a cap the
  family declares.
- 7.5 Probe: at first use, cached; a warm-up may run it ahead of time.
- 7.6 Cache key: exact (M, N, K) per device, family and build id.
- 7.7 Spills: any spill prunes.
- 7.8 Variants: shapes first; variants in a later unit.
- 7.9 Backends: nvptx first, amdgpu next.
- 7.10 `@Occupancy` constant expressions over template parameters are in
  scope.
- 7.11 Routes: one arm per family, reading the winner at its (M, N, K).
- 7.13 Spelling: a family lists its candidates with `@Shapes`, a list of
  instantiations on the family class (Julian, 2026-10-03).

- 7.14 The compiler work moves to `xpu-kernel-independence` (Julian,
  2026-10-03): template values, fragment arrays, constant expressions and
  the family's instantiation are kernel language that every library needs,
  not tile-selection features. 7.13's `@Shapes` is generalized there as
  explicit instantiation, spelled `@Instantiate` (that spec's §7.3,
  decided 2026-10-03); a family is a class template carrying it. This
  spec keeps the filter (§3), the measured choice (§4), variants (§5) and
  the routes (§6).

### 7.12 Cache invalidation (Julian, 2026-10-03)

The cache must re-measure whenever something could change which candidate
wins.
- **Key:** an entry is keyed by the device's UUID (not its name), the driver
  version, the build id, the family and the exact (M, N, K). A change to any
  of them misses the cache, and the next first use times again. The
  rows-per-block knob keys on the build id alone today.
- **Margin:** the entry records how far the winner led. A lead under about
  3% is recorded as a TIE, and either shape is acceptable. Clocks are NOT
  part of the key, since on Phoenix they move constantly. A clock or
  power-limit change can only reorder close candidates, and those are ties.
- **Measurement guard:** the probe refuses to cache a measurement taken
  while another process uses the device (foreign compute apps, a busy CI
  runner on the box). It interleaves and repeats the candidates, because
  the box's clock is bimodal.
- **Reset:** `cajeta tune --reset` clears the cache by hand.
- **Not yet:** periodic re-checks of the winner against the runner-up, until
  there is evidence of drift the above misses.
