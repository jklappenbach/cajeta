# Spec: GPU device profile & analytic launch-config picker (`xpu-device-profile`)

## 1. Definition

### 1.1 Purpose
Cajeta's GPU performance logistics — occupancy, launch geometry, and the
roofline a kernel is measured against — currently rest on **hardcoded gfx1151
constants** and **theoretical peaks**. The removed `DeviceModel` baked
`waveSize=32, ldsBytesPerCU=65536, vgprFilePerSIMD=1536, ldsBankCount=32, …`
as literals with a comment that "a caller fills these from rocminfo /
hipDeviceProp" — but nothing ever did. The runtime today reads exactly **one**
device field (`gcnArchName`, by string-scanning the prop buffer) and uses it
only for texture emulation. There is no measured memory-bandwidth ceiling, so
the profile suite reports raw `GFLOP/s` with no honest denominator.

This feature builds a **DeviceProfile**: a runtime capability that
**interrogates the actual GPU** — its static machine model (from
`hipGetDevicePropertiesR0600` / `cudaGetDeviceProperties`) plus a **measured
roofline** (a one-time bandwidth and peak-FLOP micro-benchmark) — and feeds
that into (a) an **analytic launch-config picker** that *computes* occupancy and
launch geometry rather than blindly sweeping, and (b) **honest diagnostics**
that measure kernels against the device's *measured* ceiling. An **opt-in**
hardware performance-counter tier (rocprofiler-sdk on AMD, CUPTI on NVIDIA)
calibrates the picker and powers ground-truth diagnostics.

### 1.2 Scope
- **In:**
  - (§2) Lightweight, always-on device interrogation → a portable `DeviceProfile`
    (static machine model from device props + a measured bandwidth/peak-FLOP
    roofline), replacing the gfx1151 hardcoding.
  - (§3) Honest roofline diagnostics: a *measured* ceiling as the denominator for
    the profile suite's throughput reporting.
  - (§4) An **analytic** launch-config picker that derives launch geometry /
    occupancy from the profile + the kernel's already-known resource usage
    (VGPR/spill/LDS) + the problem shape — computed, never a timed sweep.
  - (§5) An **opt-in**, `dlopen`'d counter tier (rocprofiler-sdk / CUPTI) reading
    real hardware counters to calibrate the picker and emit ground-truth
    diagnostics.
- **Out:**
  - Resurrecting the **blind/redundant** runtime sweep (`@Autotune`,
    `KernelTuner`'s timed search that ran even on devices we can model). A
    **bounded** sweep gated to *unmodelable* devices is in scope — see §4.5.
  - Changing kernel **semantics** or a kernel author's hand-chosen algorithmic
    tiling.
  - Changing **compile-time** register budgeting at runtime (VGPR allocation is
    baked into the hsaco/cubin; the picker chooses runtime launch geometry, not
    register counts).
  - **Persisting** any device or process data to disk (see §7).

### 1.3 Non-goals
- Not a replacement for hand-tuned kernels' fixed geometry (e.g. the WMMA GEMM's
  MT is bound to its LDS tiling — for such kernels the picker is *advisory*).
- Not a guarantee of a throughput **win**. For a memory-bound kernel at large
  sizes the picker will (correctly) predict "no geometry win available" — that
  honest negative is itself a deliverable.
- Not a persistent tuning database or shipped-guidance table.

### 1.4 Principles
- **Interrogate, don't assume.** Every machine constant comes from a live device
  query or an on-device measurement — no architecture literals.
- **Analytic over search, search only when you can't model.** For a device we
  can model (known arch, or queryable with the occupancy constants), config is
  *computed* from the model + roofline — never a blind timed search. A bounded
  empirical sweep is the fallback **only** when the device is queryable but its
  arch is unknown, so the occupancy constants are missing and the analytic
  ranking is unreliable (§4.5). The optional counter tier *validates* the
  computation; it does not replace it.
- **Cheap by default, heavy only on opt-in.** The lightweight probe (§2) rides on
  the HIP init the process already pays (~15–50 ms, once per process). The counter
  tier (§5) — which needs kernel replay — is `dlopen`'d and never on the default
  launch path.
- **Honest measurement.** A measured ceiling beats a theoretical one; every claim
  is measured on-device and recorded, never assumed.
- **Nothing on disk.** The profile is in-memory, per process; no cache file, no
  device fingerprint, no process internals written anywhere (§7).

## 2. Lightweight device interrogation → `DeviceProfile`

### 2.1 Requirement
At first GPU use, the runtime shall build an in-memory `DeviceProfile` for the
active device, populated from **live queries and measurements**, not hardcoded
constants. It has two parts:
- a **machine model** — the `DeviceModel` fields (wave size, max threads/block,
  LDS bytes, VGPR file size, CU/SIMD counts, LDS bank count/width, arch name)
  filled from `hipGetDevicePropertiesR0600` (AMD) / `cudaGetDeviceProperties`
  (NVIDIA), with arch-derived fields (e.g. VGPR file per SIMD) resolved from the
  arch string where the prop struct lacks them;
- a **measured roofline** — see §2.2.

The profile is built **once per process**, lazily, cached in memory.

### 2.2 Measured roofline
The profile shall include device performance ceilings obtained by **on-device
micro-benchmark**, not data-sheet peaks:
- **Memory bandwidth** — allocate a buffer (default ~128–256 MB), run a warmup +
  N copy/read passes (default 3–5), record the max achieved GB/s.
- **Peak FLOP** (optional, for the roofline ridge point) — a short
  FMA-saturating kernel measuring achieved FLOP/s for the relevant precision.
The buffer size and pass count are overridable (env); a sane default is shipped.

### 2.3 Mechanism
- Expand the existing `hipGetDevicePropertiesR0600` call (today read only for
  `gcnArchName`) to populate the full machine model from documented struct fields
  (`warpSize`, `maxThreadsPerBlock`, `sharedMemPerBlock`, `multiProcessorCount`).
- Arch-specific constants the prop struct does not expose (VGPR file per SIMD,
  LDS bank geometry, max waves/SIMD) are resolved from an **arch table** keyed by
  the queried arch string — a small, explicit, per-arch map (not a single
  hardcoded GPU), so adding a GPU is a table row, not a code change.
- The roofline micro-benchmarks reuse the existing host launch path (alloc,
  copy/launch, time via the HIP event timer already bound in the runtime).
- Host-side, the build hooks into the launch dispatcher / module registration
  (`__cajeta_xpu_register_module` → first launch), so the profile exists before
  the first picker decision.

### 2.4 Use cases
- 2.4.1 As the runtime, when the first kernel launches on an unknown GPU, then I
  query its props and measure its roofline once, and every later launch reads the
  cached profile (no re-measure).
- 2.4.2 As a maintainer porting to a new AMD/NVIDIA part, when I add its arch row
  to the table, then the profile populates correctly with no other code change —
  the gfx1151 hardcoding is gone.
- 2.4.3 As the runtime, when device props are unavailable (driver too old, query
  fails), then the profile degrades gracefully to documented defaults for the
  queried arch and records that it is estimated, never crashing.
- 2.4.4 As a developer, when I disable profiling via env, then no probe runs and
  consumers fall back to conservative defaults.

## 3. Honest roofline diagnostics

### 3.1 Requirement
The profile suite's throughput reporting shall surface the device's **measured**
ceiling (bandwidth + arch/CU) as device context, and express a kernel's achieved
throughput as a fraction of that measured ceiling **only where the ratio is
exact** — i.e. for genuinely DRAM-bound rows whose bytes-moved is unambiguous
(saxpy / dot / stream / copy: bytes ÷ time → % of the measured ceiling). The
measured ceiling is sourced from the `DeviceProfile` (§2.2).

**Honesty caveat (measured, not assumed).** A roofline % from an *ideal*
byte-traffic formula would mislead for cache/LDS-reusing kernels: for matmul the
ideal traffic (3N²·dtype) gives an optimistic ~70 TFLOP/s memory roofline that
would label the GEMM compute-bound, flatly contradicting rocprof's measured 86%
MemUnitBusy (memory-bound, counting *actual* LDS+global traffic). So matmul shows
achieved GFLOP/s + the ideal-traffic roofline **explicitly labeled an optimistic
upper bound**, never a headline %; its true % awaits the §5 counter tier
(actual traffic).

### 3.2 Mechanism
- The GPU benchmarks emit (or the runtime exposes) the measured ceiling alongside
  the kernel time; `report.py`'s `derive_thr` gains a measured-ceiling denominator
  so a `% of measured ceiling` column/annotation is available for GPU rows.
- Where no measured ceiling exists (non-GPU areas, or profiling disabled), the
  report behaves exactly as today (no regression).

### 3.3 Use cases
- 3.3.1 As a developer reading the GEMM report, when the kernel is memory-bound,
  then I see "28.0 TFLOP/s = ~99% of measured bandwidth ceiling" rather than an
  unanchored number — and know there is no headroom to chase.
- 3.3.2 As a developer on a kernel below the ceiling, when I read the report, then
  the gap to the measured ceiling tells me real headroom exists.
- 3.3.3 As a CI consumer with profiling disabled, when the report runs, then it
  produces the same output as before this feature (graceful absence).

## 4. Analytic launch-config picker

### 4.1 Requirement
For kernels whose launch geometry is **tunable** (not hard-bound to the
algorithm), the runtime shall **compute** an occupancy-optimal launch
configuration from: the `DeviceProfile` machine model, the kernel's
already-extracted resource usage (`KernelResourceInfo{vgpr, spill}` from the
emitted ISA, plus static LDS use), and the launch's problem shape. The picker is
**analytic** — closed-form occupancy + a roofline bound — never a timed sweep.

### 4.2 Mechanism
- **Occupancy model:** from VGPR/SIMD, LDS/CU, wave size, and the kernel's
  per-thread VGPR + per-block LDS, compute waves/CU and pick the launch geometry
  that maximizes occupancy without spilling, clamped by any `@Occupancy` override
  (§3 of the occupancy-autotune spec) and the §2 compile-time launch bound.
- **Roofline bound:** from the measured bandwidth/FLOP ceilings and the kernel's
  arithmetic intensity (bytes vs FLOPs for the shape), predict whether the kernel
  is memory- or compute-bound and the achievable bound — so the picker reports
  *"geometry change cannot help here"* when memory-bound.
- The picker is **pure decision logic** with the `DeviceProfile` and resource
  usage **injected**, so it is validated **GPU-free** against fixture profiles.
- For kernels with **fixed** geometry (hand-tiled WMMA GEMM), the picker does not
  override the author; it emits the roofline prediction as advisory diagnostics.

### 4.3 Use cases
- 4.3.1 As a generic elementwise/reduction kernel with tunable block size, when I
  launch on a queried device, then the picker computes the occupancy-maximizing
  block size from the profile + my VGPR usage — no timing sweep.
- 4.3.2 As a memory-bound GEMM at n≥1024, when the picker runs, then it predicts
  "memory-bound, no geometry win" and leaves the hand-tuned geometry untouched.
- 4.3.3 As a kernel with an `@Occupancy` override, when the picker runs, then its
  candidate geometry is clamped to the override and the compile-time bound — never
  exceeding a legal launch.
- 4.3.4 As a maintainer, when I unit-test the picker, then I inject fixture
  `DeviceProfile`s and resource usages and assert the computed geometry — no GPU
  required.

## 4.5 Bounded sweep — the unmodelable-device fallback

### 4.5.1 Requirement
The occupancy constants are read **live** (§5): `hipDeviceGetAttribute` reports
the register file, wave cap, and LDS budget per multiprocessor on any queryable
device, so even an **unknown-arch** device is normally *modelable* and stays
analytic. The bounded sweep is the fallback **only** in the residual case where a
device responded but the driver supplied **neither** an arch-table row **nor** the
live occupancy attributes (`model.queried && model.estimated`) — so the model is
genuinely undetermined. It times a small feasible candidate set and picks the
fastest. This is the tier the removed `@Autotune` got wrong by running it
*everywhere*; here it fires **only** where analysis genuinely cannot decide —
which, given live attributes, is near-never.

### 4.5.2 Mechanism
- `shouldSweep(model)` is true iff `model.queried && model.estimated` — i.e. a
  real device responded but we got no occupancy constants (no arch row AND no live
  attributes). A modelable device (`!estimated`, including unknown-arch-with-live-
  attrs) stays fully analytic; a device that did not respond (`!queried`) uses safe
  defaults — neither sweeps.
- `pickLaunch` sets `needsSweep` from `shouldSweep`; the caller then runs
  `sweepBlocks(candidates, timeBlock)` over the feasible candidate set, with the
  timer **injected** (GPU-free testable; the real launcher supplies launch-and-time).
- The sweep is **bounded** to the feasible candidate set (wave-multiple blocks
  that fit the budgets), never an open search.

### 4.5.3 Use cases
- 4.5.1 As the runtime on a brand-new GPU not in the arch table, when I launch a
  tunable-geometry kernel, then I sweep the feasible blocks once and pick the
  fastest — better than trusting an occupancy ranking built on guessed constants.
- 4.5.2 As the runtime on a known device (gfx1151), when I launch, then I never
  sweep — the analytic pick stands (no redundant cost; the lesson of the removal).
- 4.5.3 As the runtime with no reachable GPU, when config is requested, then I use
  conservative estimated defaults and never sweep.

## 5. Live occupancy-constant interrogation (chosen over the counter tier)

**Pivot (2026-06-29).** The original plan here was an opt-in, `dlopen`'d
rocprofiler-sdk (AMD) / CUPTI (NVIDIA) **counter tier** to read achieved occupancy
/ MemUnitBusy / bank conflicts and calibrate the picker. Two findings overturned it:
1. **rocprofiler-sdk can't profile live, mid-run.** Its PMC collection requires the
   tool to register via `rocprofiler_configure` / `force_configure` **before HIP
   initializes**; you cannot `dlopen` it after the runtime is up and count
   already-running kernels. So §5.2's "dlopen-and-profile-a-launch" model is
   unworkable in-process.
2. **The config decision needs no counters.** The quantities the counter tier was
   for — occupancy — are *derivable*, and the inputs are reported **live** by
   `hipDeviceGetAttribute`: `MaxRegistersPerMultiprocessor` (the VGPR file),
   `MaxThreadsPerMultiProcessor` (the wave cap), `MaxSharedMemoryPerMultiprocessor`
   (the LDS budget). Combined with the kernel's *actual compiled* VGPR/LDS (already
   extracted from the ISA), occupancy is computed exactly — no PMC.

### 5.1 Requirement
The interrogation (§2) shall read the occupancy constants **live** at init, in
per-multiprocessor terms (topology-free), so the analytic model (§4) works on
**any queryable device** — known arch or not — and the bounded sweep (§4.5)
becomes a near-never residual. This is cheap (a few attribute reads), live, and
portable; it replaces the gfx1151-hardcoded constants with driver-reported ones.

### 5.2 Mechanism
- Read `MaxRegistersPerMultiprocessor`, `MaxThreadsPerMultiProcessor`,
  `MaxSharedMemoryPerMultiprocessor` via `hipDeviceGetAttribute` (sanity-clamped;
  the ordinals are version-sensitive, so a wrong one fails safe to the arch table).
- `occupancy` is per-multiprocessor: register-, wave-, and LDS-limited blocks from
  those live quantities + the kernel's compiled VGPR/LDS.
- The arch table collapses to a **fallback** (when the driver omits an attribute)
  plus the two genuinely arch-only values: LDS bank geometry and the CU-per-WGP
  factor.

### 5.3 Dynamic counters — deferred, optional dev-diagnostic
The *dynamic* counters (MemUnitBusy, LDS bank-conflict replays, VALUBusy) are **not
derivable** and **not needed to configure** a kernel — only to *diagnose* runtime
behavior after the fact. They are deferred as an **optional dev-diagnostic** via a
**subprocess `rocprofv3`** invocation (which handles the before-init registration
correctly), never the fragile in-process SDK path, and never on the default launch
path. `rocprofv3` already covers this manually today.

## 6. Portability & backends

### 6.1 Requirement
The `DeviceProfile`, roofline, and picker shall be **backend-portable** by the
same discipline as the rest of XPU: a common host-side abstraction with
per-backend population, and a clean no-op where a backend has no equivalent.

### 6.2 Use cases
- 6.2.1 As the AMD backend (first), when the profile builds, then it is populated
  from `hipGetDevicePropertiesR0600` + the AMD roofline probe + (opt-in)
  rocprofiler-sdk.
- 6.2.2 As the NVIDIA backend, when the profile builds, then it is populated from
  `cudaGetDeviceProperties` + the CUDA roofline probe + (opt-in) CUPTI — the same
  `DeviceProfile` surface.
- 6.2.3 As the Vulkan/CPU backends, when the profile is requested, then it returns
  a documented limited/empty profile (no crash), and the picker falls back to
  defaults.

## 7. Security & persistence constraints

### 7.1 Requirement
- 7.1.1 The `DeviceProfile` and all counter-tier results shall live **only in
  process memory**; nothing about the device, the process, or the measurements is
  written to disk. No cache file, no device fingerprint, no UUID persisted.
- 7.1.2 Re-probing each process is acceptable because the lightweight probe is
  ms-cheap relative to HIP init.
- 7.1.3 The opt-in profiler lib is `dlopen`'d with `RTLD_LOCAL`; an
  `CAJETA_AMD_ROCPROFILER_LIB` override is honored exactly like the existing
  texture-lib override.

### 7.2 Use cases
- 7.2.1 As a security-conscious operator, when Cajeta runs GPU code, then no
  device/process telemetry is persisted anywhere — there is no on-disk artifact to
  exfiltrate or tamper with.
- 7.2.2 As an operator, when I set the profiler-lib env override, then only the
  path I specify is loaded (no surprise search beyond the documented fallback).
