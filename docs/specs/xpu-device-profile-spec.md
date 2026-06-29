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
  - Resurrecting the removed runtime block-size **sweep** (`@Autotune`,
    `KernelTuner`'s timed candidate search). The picker is analytic.
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
- **Analytic over search.** Config is *computed* from a model + roofline, not
  discovered by timing many candidates. The optional counter tier *validates* the
  computation; it does not replace it with a sweep.
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
The profile suite's throughput reporting shall express a kernel's achieved
throughput as a fraction of the device's **measured** ceiling (memory bandwidth
for memory-bound kernels, peak FLOP for compute-bound), not of a theoretical
peak. The measured ceiling is sourced from the `DeviceProfile` (§2.2).

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

## 5. Opt-in hardware counter tier (calibration + ground truth)

### 5.1 Requirement
Behind an **opt-in** flag, the runtime shall read **real hardware performance
counters** for a kernel launch — achieved occupancy, memory-unit busy, LDS bank
conflicts, VALU busy, wave counts (AMD); the analogous CUPTI metrics (NVIDIA) —
to (a) **calibrate/validate** the analytic picker's predictions against ground
truth and (b) emit ground-truth diagnostics. It is never on the default launch
path.

### 5.2 Mechanism
- AMD: `dlopen` **rocprofiler-sdk** (the in-process API that `rocprofv3` wraps),
  following the established optional-lib pattern used for `libcajeta_amdtex`
  (tri-state load, `CAJETA_AMD_ROCPROFILER_LIB` env override, path search, graceful
  degrade). NVIDIA: the same shape over **CUPTI**.
- Counter collection uses **kernel replay** (the kernel runs multiple passes, one
  per counter set), costing hundreds of ms to seconds per profiled kernel — hence
  opt-in and dev/tuning-time only.
- Results are returned/printed in-process; **never persisted** (§7).
- When the lib is absent or the flag is off, the tier is a no-op and the picker
  runs model-only (§4) with no degradation in correctness.

### 5.3 Use cases
- 5.3.1 As a developer validating the picker, when I opt into the counter tier,
  then the runtime reports the picker's *predicted* vs the *measured* occupancy /
  bank-conflict / mem-busy, so I can trust or correct the model.
- 5.3.2 As a developer diagnosing a kernel, when I opt in, then I get real counter
  values in-process without manually wiring `rocprofv3` from the CLI.
- 5.3.3 As any user not opting in (or on a box without the profiler lib), when I
  run, then nothing loads, nothing is profiled, and the default path is unchanged.

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
