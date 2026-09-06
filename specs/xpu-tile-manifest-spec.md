# Spec: Tile kernel manifest — what a kernel declares and what the compiler records (`xpu-tile-manifest`)

**active** — filed 2026-09-06, approved 2026-09-06 (every open question resolved with the developer; plan in `agents/`). The compile-time half of the Tile scheduling
family. Companion documents: [`xpu-tile-scheduling`](xpu-tile-scheduling-spec.md)
(the runtime scheduler that consumes the manifest) and
[`xpu-tile-workload-profiles`](xpu-tile-workload-profiles-spec.md) (the four
client profiles). Research record:
[`xpu-tile-scheduling-findings`](xpu-tile-scheduling-findings.md). Machine-readable
shape: [`schemas/tile-manifest-v1.schema.json`](schemas/tile-manifest-v1.schema.json).

## 1. Definition

### 1.1 Purpose

Every paper in the scheduling corpus had to *recover* the same handful of
per-kernel facts by profiling, interposition, or learned models: the resource
footprint, the arithmetic character, the working set, the launch count, whether
the kernel is restartable. Cajeta compiles the kernel. It can **record** those
facts at build time and hand them to the runtime, so the scheduler never guesses
what the compiler already knew. The findings document calls this the single
largest structural advantage the project has over every system it studied
(iGniter profiled 11 configurations per workload; gpu-lets profiled 1,250 pairs;
MISO trained an autoencoder — all to reconstruct what a manifest states).

The **manifest** is a per-`(kernel, target)` record emitted by the compiler next
to the device artifact, readable at runtime and by tooling, carrying the
footprint, the work model, the access modes, the tile geometry, and the
schedule declarations of the kernel.

### 1.2 Scope

- **In:** the manifest record and its schema; the Tile-surface declarations
  that populate it (schedule policy, per-target shape pins, outputs per lane,
  degraded variants, streaming access, accumulating writes, ragged work lists,
  indirect launch bounds); the compiler-derived fields (footprint from the
  artifact, cost expressions from the tile body, restartability from access
  sets, capture safety, reconvergence needs); the runtime accessor; the
  inspection command's use of the same record.
- **Out:** how the runtime uses the manifest (`xpu-tile-scheduling`); timing of
  any kind (`cajeta-profiler`); the device profile's own machine model
  (`xpu-device-profile`, archived); disassembly tiers
  (`kernel-artifact-inspection`, which shares this record's footprint section).

### 1.3 Problem

The shipped cooperative-tile surface emits a `KernelResourceDescriptor` whose
fields the *author* fills in at the launch site: the LDS reservation is whatever
was typed, the VGPR count is a documented zero stub, and the arithmetic character
is a hand-picked enum. Nothing in that record is checked against the artifact.
Meanwhile the AMD backend already parses `.vgpr_count` and `.vgpr_spill_count`
from the assembled code object, the occupancy picker already computes resident
waves from VGPR and LDS, and the device profile already measures the bandwidth
ceiling — and no launch path reads any of it (`hardware-profile-tuning-findings`
§7.2: the profile is a thing you can print, not a thing that decides).

### 1.4 Non-goals

- Not a performance model. The manifest carries counts and expressions; the
  runtime turns them into predictions against a measured device (scheduling
  spec §4).
- Not a substitute for measurement. Where a fact cannot be derived (a hand
  kernel with data-dependent loops), the field says `measured` and the runtime
  fills it from observation.
- Not vendor lore. A field that exists on one backend and not another is
  reported as absent, never as zero (`kernel-artifact-inspection` §1.3).

### 1.5 Principles

- **Derived beats declared.** A fact the compiler can compute from the artifact
  or the tile body is never accepted from the author. The author declares only
  intent: policy, quality variants, streaming hints, shape pins.
- **Expressions over the kernel's own parameters.** Cost fields are expressions
  in the kernel's launch arguments and tile constants, so a submission needs no
  side channel to be classified — the arguments *are* the shape.
- **One record, three readers.** The scheduler, the inspection command, and the
  profiler's record sink read the same manifest by the same identity.

## 2. Identity and provenance

- **2.1** When a `@Kernel` is compiled for a target, the compiler emits one
  manifest for that `(kernel, target)` pair, keyed by the kernel's qualified
  name, the target architecture, and a content hash of the device code.
- **2.2** When the device code changes, the hash changes and every cached
  runtime fact keyed by the old hash is discarded (the resident-daemon lesson:
  identity must be a content hash).
- **2.3** When the manifest is emitted, it records the compiler version, the
  XPU ABI version, and the manifest schema version, so a runtime can refuse a
  record it does not understand.
- **2.4** When a kernel is compiled for the CPU backend, a manifest is still
  emitted; footprint fields that do not exist there (VGPR, LDS) are absent, not
  zero.

## 3. Footprint — from the artifact

- **3.1** When the target assembles a code object that reports per-kernel
  resource usage (AMDGPU `.amdgpu_metadata`; NVPTX via `cuobjdump`/`ptxas -v`;
  SPIR-V via pipeline statistics where the driver exposes them), the manifest
  records vector registers, scalar registers, spill or scratch bytes, static
  shared memory, and the wave width, as the artifact states them.
- **3.2** When a kernel spills, the manifest records the spill bytes and the
  build emits a warning naming the kernel; a spilling kernel is never silently
  accepted as tuned (Volkov).
- **3.3** When the block size is pinned (a `@Tile` shape or an `@Occupancy`
  clamp), the manifest records the pinned threads per group and the resident
  groups per compute unit the occupancy picker computes from the footprint; when
  the block size is a launch argument, it records the feasible block sizes
  best-first as the picker enumerates them.
- **3.4** When the manifest reports occupancy, it labels it as a capacity
  accounting unit for the scheduler, never as a performance target; any report
  that prints occupancy also prints the roofline fraction where one is measured
  (findings §A.11.4).
- **3.5** When dynamic shared memory is a launch argument, the manifest records
  the parameter that sizes it rather than a value.

## 4. Work model — from the tile body

- **4.1** When a kernel is expressed on the cooperative surface, the compiler
  derives per-group cost expressions from the body: matrix-core multiply-adds
  (from `Group.mac` over `Tile` fragments), vector multiply-adds (from `mac`
  over vector operands and from arithmetic in `stripe` loops), integer
  operations, and bytes read and written per buffer, each as an expression in
  the kernel's parameters and tile constants.
- **4.2** When a loop's trip count is a launch parameter or a tile constant, the
  expression carries it symbolically; when the trip count is data-dependent (a
  loop bound loaded from a buffer), the field is marked `measured` and the
  runtime substitutes observation.
- **4.3** When a kernel is a hand kernel (not on the surface), the compiler
  still derives what it can from buffer accesses and constant or argument loop
  bounds, and marks the rest `measured`. There is no author-declared cost: a
  developer cannot reasonably know it, the corpus shows declared classes are
  wrong across phases, and one solo run per shape is cheap (§14). The measured
  cost is reported back to the developer through the profiler (§14.4).
- **4.3a** When every cost field of a kernel is `measured`, the scheduler is
  still correct: derived cost expressions shorten warm-up, they are never a
  requirement (scheduling spec §3.4).
- **4.4** When matrix-core and vector-ALU operations are both present, the
  manifest keeps them separate; a device's two peaks differ by an order of
  magnitude (A100: 312 vs 19.5 TFLOP/s) and a single FLOP count misclassifies.
- **4.5** When operands are stored at a narrower width than they are computed
  at (4-bit weights, 8-bit activations, f16 KV), the manifest records the stored
  width per buffer, because bytes moved and compute width diverge and the
  crossover from memory-bound to compute-bound moves with the stored width
  (findings §ML.13.1).
- **4.6** When a kernel's parallel axis is a token count, a row count, or an
  item count, the manifest names which parameter is the work axis so the
  runtime can read the roofline coordinate directly from the arguments.

## 5. Tile geometry

- **5.1** When a kernel uses `Tile` fragments or an LDS-sized tile, the manifest
  records the tile shape per axis and the granularity per axis (the multiple a
  dimension must round to); a launch whose shape is not a multiple pays a
  measured 32 % for a one-element miss, so the runtime rounds against this
  field.
- **5.2** When the author declares `outputsPerLane` (the number of outputs one
  lane accumulates before writing), the manifest records it; the default is
  greater than one (Volkov: work per lane, not occupancy, hides latency).
- **5.3** When per-target shape pins are declared (`@Tile(target = …, rows, cols,
  k)`), the manifest records the shape chosen for this target and whether it was
  pinned or defaulted, and the compiler verifies a pinned shape is legal for the
  algorithm (cooperative-tile §2.4, §6.1 — previously unimplemented).
- **5.4** When the implied group count for a launch would fall below the
  device's compute-unit count, the manifest records the reduction axis the
  kernel exposes for splitting (split-K, KV-chunk) if it exposes one, so the
  runtime can add parallelism before launching rather than run underfilled.

## 6. Access modes and sets

- **6.1** When a kernel parameter is a `KernelBuffer`, `Shared`, image, or
  resident-pool handle, the manifest records its access mode: `read`,
  `write` (exclusive, previous contents discardable), `readwrite`,
  `accumulate` (atomic or order-dependent accumulation), or `indirect`
  (holds launch bounds or work lists read by the runtime).
- **6.2** When the access mode is derived from the body (loads only, stores
  only, atomics), the manifest marks it `derived`; when the author narrows it
  (`@Access(write)` on a buffer the body only stores to in a subset), the
  manifest marks it `declared` and the compiler checks the body does not
  contradict it.
- **6.3** When a buffer is read with a streaming pattern the author marks
  `@Streaming` (or the compiler proves is touch-once), loads and stores are
  lowered non-temporal where the target supports it and the manifest records it,
  so a best-effort kernel does not evict a protected kernel's working set
  (MASK).
- **6.4** When a kernel accumulates into a buffer, the manifest records that
  buffer as `accumulate`; two launches accumulating into one buffer are never
  co-run and the result is documented as order-dependent in float
  (FlashAttention-2's `dQ`).
- **6.5** When a kernel's read set and write set are disjoint and it holds no
  `accumulate` or `readwrite` buffer, the manifest marks it **restartable**; any
  other kernel is marked non-restartable and the runtime never cancels it
  mid-flight (REEF's idempotence assumption, made checkable).
- **6.6** When the manifest is read at submit time, the submission's buffer
  sets are the manifest's parameter modes bound to the actual handles; the
  author does not restate them, so they cannot drift from what the kernel does.
- **6.7** When a kernel is a global reduction whose result the host or a
  scalar decision consumes (a solver dot product, an all-reduce), the manifest
  marks it `drainsDevice`; the scheduler treats such a node as a barrier and
  the window behind it as a placement opportunity (profiles spec §4.4.6).

## 7. Ragged work and indirect bounds

- **7.1** When an algorithm's work items are non-uniform (attention over
  sequences of different length, expert GEMMs of different row counts), the
  surface accepts one launch over an **item list** with per-item geometry, and
  the manifest records the per-item cost expression (`α·l_q + β·l_kv` for
  attention) the runtime uses to balance items across groups; skewed workloads
  lose 38 % of per-token latency to bad balance alone (FlashInfer).
- **7.2** When a launch's work count is known only on device (routed token
  counts, accepted speculative tokens, compacted survivor counts), the surface
  accepts an `indirect` buffer as the launch bound and the manifest names it,
  so the runtime neither pads to a worst case nor round-trips to the host.
- **7.3** When a kernel is a member of a ragged launch, its geometry is per
  item; the manifest's footprint is per group and the runtime sizes groups from
  the item list.

## 8. Restartability, yield points, and capture safety

- **8.1** When a kernel is restartable (§6.5) and the author marks it
  `@BestEffort`, the compiler inserts a group-boundary yield check (a flag read
  at the top of each group's work; Effisha-style), and the manifest records the
  kernel as yield-capable; a runtime that must clear the device for a deadline
  sets the flag and waits at most one group's duration.
- **8.2** When a kernel is not restartable, no yield point is inserted and the
  manifest says why; the runtime falls back to admission control for that
  kernel (GPreempt: preemption is a 100 µs-class operation, so deadlines tighter
  than that come from not launching, not from stopping).
- **8.2a** When yield points are inserted, the compiler spaces them so the work
  between checks is 10–50 µs at the manifest's predicted per-group cost, and
  records the spacing; a kernel whose groups are shorter than that is checked
  every N groups, not every group (scheduling §8.4.6).
- **8.3** When a kernel makes no host callback, uses no host-visible allocation
  on its path, and its launch bounds are either constants, arguments, or
  `indirect` buffers, the manifest marks it **capture-safe**; only capture-safe
  kernels may enter a replayed graph (scheduling spec §9).
- **8.4** When a kernel uses a cross-lane operation, the manifest records it so
  backends that must request maximal reconvergence (Vulkan) do so, exactly as
  the existing `usedSubgroupOp` seam does today.

## 9. Completion instrumentation

- **9.1** When instrumentation is enabled for a kernel, the compiler emits a
  group-start and group-end notification from lane 0 of each group into the
  runtime's completion ring, aggregated by a compile-time factor (default 16
  groups per record), and the manifest records the factor; the added cost of an
  otherwise-empty kernel stays under 8 µs at 160 groups (Paella measured
  6.6 µs), asserted by a microbenchmark.
- **9.2** When instrumentation is off, no notification code is emitted and the
  device code hash differs from the instrumented build; the two are distinct
  manifests.
- **9.3** When the profiler's record sink delivers a dispatch record, it carries
  the manifest identity, so achieved duration lands on the right
  `(kernel, target, geometry)` window without a name lookup.

## 10. Degraded variants

- **10.1** When an author provides more than one implementation of an algorithm
  at different quality or cost (fewer iterations, lower resolution, a partial
  reduction, a quantized path), the surface lets them be declared one
  **variant family** with an ordered quality rank, and each variant's manifest
  names the family and its rank.
- **10.2** When the runtime must meet a deadline it cannot meet with the
  requested variant, it may substitute a lower-ranked member of the same family
  and reports the substitution; it never substitutes across families.
- **10.3** When variants differ in numeric tier, each variant's declared
  tolerance (cooperative-tile §4A) is recorded in its manifest, so a
  substitution's numeric consequence is stated, not discovered.

## 11. Schedule declarations echoed

- **11.1** When the author declares a default schedule policy on the kernel
  (`@Schedule(policy = throughput | latency | frameBudget)`), the manifest
  records it; a submission may override it.
- **11.2** When the author declares an arithmetic-character hint
  (`ArithmeticCharacter` on the submission today), the manifest records it as
  `hint`; the runtime derives the class from §4 against the measured device and
  reports disagreement between hint and derivation as a diagnostic, never
  trusting the hint alone (Orion: the declared class is the decisive signal
  only when it is true).
- **11.3** When the author declares a power class (`@Schedule(power = high)`)
  for a kernel known to push a device toward its cap (dense prefill on a 4090),
  the manifest records it; the runtime's power channel (scheduling spec §6.6)
  reads it.
- **11.4** When a kernel is declared `@BestEffort`, `@Protected`, or neither,
  the manifest records the default protection class.

## 12. Runtime accessor and inspection

- **12.1** When a program references a kernel, `k.manifest()` returns the
  manifest for the active target as a host record with the fields above;
  absent fields read as absent, not zero.
- **12.2** When `Scheduler.submit` is called, it reads the manifest for the
  submission's kernel; the author-filled `KernelResourceDescriptor` fields
  shipped in cooperative-tile Unit 6 (`ldsBytes`, `vgprCount`) are replaced by
  manifest values and the author-facing constructor parameters for them are
  removed.
- **12.3** When `cajeta kernel-inspect` (the `kernel-artifact-inspection` spec)
  prints a kernel's resource report, its footprint columns are the manifest's
  §3 fields, so the inspection tool and the scheduler cannot disagree.
- **12.4** When a manifest is emitted, it is embedded in the artifact it
  describes — a data section of an `--emit=exe` binary, a member of the `.cja`
  beside the kernel's device code, in memory for a JIT run — and a JSON copy
  conforming to `schemas/tile-manifest-v1.schema.json` is written to the build
  output so a build can be audited without running it. The artifact is never
  modified after build; nothing at runtime writes to a manifest, and reading
  one needs no storage.
- **12.5** When a program runs where no writable storage exists, the manifest
  is still available (it is in the artifact) and every measured fact is held in
  memory; the calibration tiers of the scheduling spec §12 say what may be
  cached or embedded beyond that.

## 13. Portability corrections carried from the cooperative-tile review

- **13.1** When a launch site needs the group width, it reads
  `Group.laneBlock()`; witness A's launcher in cajeta-llm still computes threads
  from a literal 32 and must be corrected as part of this spec's adoption.
- **13.2** When `TargetDescriptor.waveWidth()` is called on the host, the
  compiler reports an error naming `Group.laneBlock()`; today it silently
  returns 1 on every backend.
- **13.3** When `Group.rowId()` is used with a launch that places more than one
  group per block, the row mapping is wrong; the manifest records the
  rows-per-group the kernel assumes and the runtime checks the launch against
  it.
- **13.4** When the `[mma-tiering]` note is emitted for a `Tile`, it names
  `Tile`, not `CooperativeMatrix`.
- **13.5** When a `Tile` local is assigned two roles across `mac` calls, the
  compiler reports an error; today the first role silently wins.

## 14. Developer workflow — how the numbers are produced and shipped

Settled with the developer 2026-09-06. The author declares intent (a stream's
period, a best-effort mark, a streamed buffer, a variant family) and never a
quantity; every quantity is derived by the compiler or measured by the runtime,
and every measured quantity is shown back to the author.

- **14.1 Write.** When a kernel is written on the cooperative surface, its cost
  is countable and derived; when it is a hand kernel, it starts as `measured`.
  Intent lives in types (`PeriodicStream`, a `Degradable` variant family, a
  submission builder), not in annotations sprinkled on launch sites.
- **14.2 Build.** When `cajeta build` lowers a kernel, it emits the manifest
  into the artifact (§12.4); `cajeta kernel-inspect` prints it; the IDE shows
  the same record inline on the `@Kernel` (registers, LDS, wave width, spill as
  a warning, derived class per target, `measured` where the compiler could not
  count), and a tile-granularity miss at a launch site is an inspection.
- **14.3 Test.** When a project pins a kernel's facts, it asserts them from
  `k.manifest()` in ordinary tests (no spill, restartable, memory-bound on a
  named target, granularity honored), run in the device-tests leg; the ISA
  reader's "spilled 0" acceptance in cajeta-llm is the precedent.
- **14.4 Profile.** When `cajeta profile` runs a program or its bench verb,
  the summary carries one line per kernel and shape: derived class, achieved
  fraction of the measured roofline, duration percentile, predictability, and
  bytes moved; the profiler tool window shows the same as a kernel card, and
  `profile-run-history` gives the delta between two runs. The scheduler's own
  outcomes (goodput, per-kernel slowdown, what a frame cut) flow through the
  same sink into the same view.
- **14.5 Ship.** When an artifact is built, it carries the manifest and, by
  default, nothing measured: measurements are device-specific and the language
  is platform-independent, so every device measures itself (scheduling §12
  tier 1). The archive never changes after build.
- **14.6 Embed, opt-in, for fixed hardware.** When a deployment has no
  writable storage and a fixed target (an appliance, an embedded board, code in
  EPROM), the developer runs `cajeta calibrate` on that device and passes the
  resulting snapshot to `cajeta build`, which embeds it as read-only data beside
  the manifest; at runtime the bundled `CalibrationStore` serves it after
  validating it against the live device identity (scheduling §12.4c). This is
  never the default.
- **14.6a Store, replaceable.** When an application knows where its
  calibration records should live (a console's save-data API, a flash region,
  a network cache), it replaces the default `CalibrationStore` module — which
  implements the platform heuristic — with its own before the first launch
  (scheduling §12.4a); the discovery class is the only storage mechanism, and
  the static manifest never goes through it — it is in the artifact.
- **14.7 Calibrate deliberately.** When `cajeta calibrate` runs, it executes
  every kernel in the build solo at representative shapes and fills the
  calibration set without running the application; the shapes come from a
  previous profile run, or optionally from a static method the kernel's class
  provides. This is the one optional developer input.

## 15. Decisions

- **D1 — one manifest for every `@Kernel`, richer on the surface.** The
  scheduler must serve hand kernels (cajeta-llm has 183 launch sites) and
  surface kernels alike. Hand kernels get footprint and access modes from the
  artifact and body; surface kernels additionally get derived cost expressions.
- **D2 — the author never fills the footprint.** The Unit 6
  `KernelResourceDescriptor` constructor that took `vgprCount` and `ldsBytes`
  from the author is retired; both come from the manifest.
- **D3 — cost is symbolic, evaluated per submission.** No per-launch side
  channel for shape. The runtime evaluates the manifest's expressions over the
  submission's arguments.
- **D4 — restartability is derived, never declared.** An author cannot mark a
  kernel restartable; the compiler proves it from access modes.
- **D5 — the manifest shares the inspection tool's footprint schema.**
  `kernel-artifact-inspection` §1.2(b) and this spec emit one record.
- **D6 — no author-declared cost; measured, and reported.** (Developer,
  2026-09-06.) A developer cannot reasonably know a kernel's cost, and asking
  creates burden without accuracy. Hand kernels are measured; the profiler
  reports what was measured (§14.4).
- **D7 — the manifest lives in the artifact, immutable.** Not a sidecar that
  can be lost, not a file the runtime rewrites; reading it needs no storage.
- **D8 — ship nothing measured by default.** The language is platform
  independent; a binary carries no assumption about the device it lands on. An
  embedded calibration snapshot is an opt-in build input for fixed-hardware,
  storage-less targets only (§14.6).

## 16. Open questions — resolved 2026-09-06

- **O1** (author-declared cost) — resolved as D6.
- **O2** (manifest in the `.cja`) — resolved as D7: the manifest goes wherever
  the device artifact goes; it is a packaging invariant, not a choice.

## 17. References

- Research record: [`xpu-tile-scheduling-findings`](xpu-tile-scheduling-findings.md).
- Corpora: `research/xpu-scheduling/papers/` (Paella instrumentation, Orion
  classification, REEF restartability, Volkov, roofline, MASK),
  `research/llm-serving/papers/` (FlashInfer ragged plans, FlashAttention-2
  accumulation, Sarathi tile granularity), `research/sim-scheduling/papers/`
  (StarPU access modes, Legion regions), `research/gfx-scheduling/` (render-graph
  declarations).
- Sibling specs: [`xpu-cooperative-tile`](xpu-cooperative-tile-spec.md),
  [`kernel-artifact-inspection`](kernel-artifact-inspection-spec.md),
  [`xpu-kernel-scheduling-hints`](xpu-kernel-scheduling-hints-spec.md),
  archived [`xpu-device-profile`](archive/xpu-device-profile-spec.md) and
  [`kernel-occupancy-autotune`](archive/kernel-occupancy-autotune-spec.md).
