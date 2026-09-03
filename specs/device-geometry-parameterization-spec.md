# Device-geometry parameterization

**Status:** draft · **Axis:** xpu / cajeta-llm tuning · **Opened:** 2026-09-03

## 1. Definition

Cajeta's GPU kernels and their launch shapes are currently sized by constants
that were *measured on one part* — Strix Halo, `gfx1151` — and then frozen.
Each such constant has a derivation recorded beside it, and every derivation
reads the same way: **a device property crossed with a kernel property**. The
device half is knowable at runtime and is not; so the constant travels to
machines it was never measured on and silently misdescribes them.

This capability closes that gap in one direction only: it makes the **device
half a queried quantity** and rewrites each frozen constant as the law its own
comment already states. It is explicitly NOT a vendor fork. Julian's north
star, stated 2026-09-02:

> Ideally, we have one code base in the cajeta-llm project, and it works
> maximally optimized no matter what device it's running on.

so the acceptance bar for every item below is: **the AMD number is unchanged**
(the law reproduces the measured constant on `gfx1151`) **and** the NVIDIA
number becomes derived rather than inherited.

### 1.1 What "hardcoded for AMD" actually turned out to mean

Measured, not assumed. Two exhibits, both found by running the real workload
on an RTX 4090 rather than by reading code:

| # | Site | Frozen value | What it silently means elsewhere |
|---|---|---|---|
| 1 | `cajeta-llm` `Linear.cajeta:167` `TARGET_BLOCKS = 80` | 80 workgroups | Ada dispatches 80 of 256 possible blocks — **under-dispatched 3.2x** |
| 2 | `q4kF16CoopN256Kernel` / `q6kF16CoopN256Kernel` LDS tile | 55296 B | `ptxas error: uses too much shared data (0xd800, 0xc000 max)` — **the kernel does not exist on NVIDIA** |

Exhibit 2 is the sharper one: the tile was sized against `gfx1151`'s 64 KB of
LDS *per workgroup*. Ada has **more** shared memory per SM (102400 B) and still
rejects the kernel, because NVIDIA caps a single **block** at 49152 B unless the
launch opts in. The profile today models `ldsBytesPerMP` and has no notion of a
per-block ceiling at all, so the model says the tile fits and the assembler says
it does not. That distinction is exactly llama.cpp's `smpb` vs `smpbo`.

## 2. The quantifiable geometry

Every value below was read from a live device before being specified.
NVIDIA figures come from a standalone `dlopen("libcuda.so.1")` probe on the
RTX 4090 in this box (all ordinals `rc=0`); AMD figures from the `gfx1151`
arch row and the on-device verification recorded in `DeviceProfile.cpp`.

### 2.1 Tier A — already queried (no change)

| Fact | CUDA ord. | HIP ord. | 4090 | gfx1151 |
|---|---|---|---|---|
| `waveSize` | 10 | 87 | 32 | 32 |
| `maxThreadsPerBlock` | 1 | 56 | 1024 | 1024 |
| `multiprocessorCount` | 16 | 63 | 128 SM | 20 WGP |
| `regsPerMP` | 82 | 72 | 65536 | 196608 |
| `threadsPerMP` | 39 | 57 | 1536 | 2048 |
| `ldsBytesPerMP` | 81 | 10002 | 102400 | 65536 |

### 2.2 Tier B — queryable, load-bearing, and absent today

| Fact | CUDA ord. | 4090 | Why a kernel decision needs it |
|---|---|---|---|
| `ldsBytesPerBlock` | 8 | **49152** | The ceiling a static shared tile is actually checked against (exhibit 2) |
| `ldsBytesPerBlockOptin` | 97 | **101376** | The raised ceiling, reachable only via an explicit dynamic-shared opt-in |
| `maxBlocksPerMP` | 106 | 24 | A residency limiter the occupancy closed form does not model |
| `l2CacheBytes` | 38 | 75497472 | The limiter `TARGET_BLOCKS` is *actually* bounded by, per its own comment |
| `totalGlobalMemBytes` | — (`cuDeviceTotalMem`) | 25756565504 | Whether weights can be device-resident at all |
| `memoryBusWidthBits` | 37 | 384 | Theoretical roofline, to cross-check the measured probe |
| `memoryClockKHz` | 36 | 10501000 | ditto |
| `integrated` | 18 | 0 | APU vs discrete: whether a host↔device copy is a real transfer |
| `maxGridDim{X,Y,Z}` | 5,6,7 | 2147483647 / 65535 / 65535 | Grid clamping and stepping |
| `maxBlockDim{X,Y,Z}` | 2,3,4 | 1024 / 1024 / 64 | Block-shape clamping |
| `clockRateKHz` | 13 | 2535000 | FLOP ceiling, once a shader-width constant exists |

### 2.3 Tier C — arch constants, not driver attributes

Not exposed by any driver call; one table row per family, the way
`ldsBankCount` already is.

| Fact | RDNA3/3.5 | NVIDIA SM (Volta+) | Used for |
|---|---|---|---|
| `simdsPerMP` | 8 per WGP (2 CU x 4) | 4 partitions | The dispatch law (exhibit 1) |
| `ldsBankCount` / `ldsBankWidth` | 32 x 4 B | 32 x 4 B | Swizzle / conflict diagnostics |
| `coopMatrixShapes` | WMMA 16x16x16 | 16x16x16, 16x8x16, … | Tile structure of every quantized GEMM |

### 2.4 Inventory of frozen sites (measured, 2026-09-03)

What is actually hardcoded, counted rather than estimated, so the size of the
remaining work is visible instead of implied. In `cajeta-llm/src/main`:

| Shape | Count | Status |
|---|---|---|
| `Linear.TARGET_BLOCKS` | 1 | **derived** (unit 6) |
| coop-matrix LDS tile above the per-block ceiling | 2 kernels | ceiling now modelled; re-tiling open |
| `block: [256` at a launch site | 71 | frozen |
| `block: [32` | 63 | frozen |
| `block: [128` | 26 | frozen |
| `block: [64` | 23 | frozen |
| `QuantKernel.ROWS_PER_BLOCK = 64` | 1 | frozen (it is the block width the law divides by) |

The 183 literal block sizes are NOT all wrong — a block width is a genuine
kernel property, and several were measured. What makes them a debt is that
none of them is checked against the device they run on: `Device.waveSize()`
and `Device.maxThreadsPerBlock()` now exist to say whether a literal is a
whole number of waves and whether it fits. Retiring them is per-kernel work
with a measurement each, not a sweep, and it is deliberately not in this plan.

## 3. The laws

Each law is stated so that substituting `gfx1151` reproduces the frozen
constant. That reproduction is the regression test, not a comment.

- **L1 — dispatch width.**
  `targetBlocks = (mpCount x simdsPerMP) / wavesPerBlock`
  gfx1151: `(20 x 8) / 2 = 80` — the frozen value, exactly.
  sm_89:   `(128 x 4) / 2 = 256`.
  This is deliberately a *SIMD-saturation* target and not an occupancy
  maximum: `Linear.cajeta`'s comment records that adding waves past this
  point made each one evict the others from L1/L2. L6 is what bounds it.

- **L2 — shared-memory tile ceiling.**
  `tileBytes <= ldsBytesPerBlock`, or `<= ldsBytesPerBlockOptin` when the
  launch opts in. A kernel that needs more must re-tile, not be dropped.
  Note the asymmetry this exposes: AMD's per-block ceiling *equals* its
  per-MP budget, NVIDIA's is roughly half of it. Code that reads
  `ldsBytesPerMP` as "what one block may use" is correct on AMD by accident.

- **L3 — occupancy.** Unchanged closed form, plus a new `maxBlocksPerMP`
  clamp.

- **L4 — roofline denominator.**
  `theoreticalGBps = 2 x memoryClockKHz x 1e3 x memoryBusWidthBits / 8 / 1e9`
  4090: 1008.05 GB/s, against 882.55 GB/s measured = 87.6% of theoretical.
  Two independent ceilings that cross-check; today only the probe exists, so
  a probe that silently under-reports is indistinguishable from a slow part.

- **L5 — residency.** `integrated` and `totalGlobalMemBytes` decide whether
  weights are staged on device and whether an upload is a transfer or a
  pointer. `Linear.wantUnified` is currently a hand-set boolean whose comment
  reasons explicitly about "an APU has unified memory".

- **L6 — cache footprint.** `l2CacheBytes` bounds the concurrent working set
  that L1 makes profitable. 4090 has 72 MiB; Strix Halo's Infinity Cache is
  32 MiB. A dispatch tuned to the smaller one leaves the larger idle.

## 4. Use cases

- **UC-1** A 4090 runs `cajeta gpu-profile` and every Tier-B fact appears,
  populated from the driver, with no arch-table row for `sm_89`.
- **UC-2** A `gfx1151` runs the same and reports the same field set; values
  absent from HIP are 0 and flagged, never defaulted to NVIDIA's.
- **UC-3** L1 applied to a queried `gfx1151` yields exactly 80.
- **UC-4** L1 applied to a queried `sm_89` yields 256.
- **UC-5** A kernel whose static shared tile exceeds `ldsBytesPerBlock` is
  re-tiled at compile time rather than failing in `ptxas`.
- **UC-6** A kernel that genuinely needs more than the default block ceiling
  is launched with the dynamic-shared opt-in raised to its requirement, and
  fails loudly if that exceeds `ldsBytesPerBlockOptin`.
- **UC-7** The theoretical and measured bandwidth ceilings are both reported;
  a measured value below a configurable fraction of theoretical is flagged.
- **UC-8** An unqueryable device still yields a model, still flagged
  `estimated`, and no law silently uses a default as if it were measured.
- **UC-9** Every law is unit-tested GPU-free by injecting `RawDeviceProps`,
  for both the AMD and the NVIDIA shape, in the same test file.

## 5. Non-goals

- No new vendor branch anywhere in `cajeta-llm`. A law that cannot be written
  against the profile is a missing profile field, not a licence to branch.
- No change to AMD's measured throughput. Every law reproduces the frozen
  constant on `gfx1151`; where it cannot, the frozen constant stays and the
  discrepancy is recorded here.
- No FLOP-ceiling probe (still deferred); `clockRateKHz` is captured but
  unused until a shader-width constant exists to multiply it by.

## 6. Open questions

- **Q1** Does HIP expose a per-block shared-memory ordinal that is stable
  across ROCm 6/7? Unverifiable on this box (no ROCm). Until measured on
  `proton`, AMD's `ldsBytesPerBlock` is left 0 and consumers fall back to
  `ldsBytesPerMP`, which is the value AMD has always effectively used.
- **Q2** Is `simdsPerMP` better as an arch-table column or derived from the
  vendor plus `threadsPerMP / waveSize`? Table, provisionally: the derivation
  would give 48/32 = 1.5 on Ada, which is a residency cap and not a count of
  schedulers.
