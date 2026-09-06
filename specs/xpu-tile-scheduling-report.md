# Tile scheduling — the numbers report

The ledger for the `xpu-tile-*` family (developer, 2026-09-06: "numbers
driven — profile before, add the scheduler, profile again, store each trial in
a table, make sure we're not making things worse"). Filled by the scheduling
plan's Unit 0 (baseline) and by every optimization-bearing unit of the three
plans afterwards. Nothing in the family is accepted without a row here.
Companion: [`xpu-tile-scheduling-findings`](xpu-tile-scheduling-findings.md)
holds the literature's numbers; this file holds ours.

## 1. Method

Fixed before the first measurement so no unit can choose a flattering
instrument.

| Item | Setting |
|---|---|
| Devices | gfx1151 (Strix Halo APU, this box); CPU backend (this box); RTX 4090 (PHOENIX, rows filled by that session) |
| Identity recorded per row | device, driver version, compiler commit, power mode, backend |
| Instrument | `cajeta profile` device tier (HIP timestamps on AMD; the profiler's exact host tier on CPU; CUPTI on NVIDIA) |
| Warm-up | first-touch pages committed and every kernel run once per shape before timing (the first-touch finding: ~890 ms one-time on fresh buffers) |
| Runs | N ≥ 5 per arm; report median and the min/max noise band |
| Idle gate | no other `cajeta_test`, bench, or GPU client running (`pgrep` check in the harness); a rerun on a busy box is discarded |
| Arm order | alternated A/B/B/A across runs (a fixed order let a decaying load fake a speedup once) |
| Storage | harness rows under repo `tmp/bench/` (never `/tmp`); tables rendered by the report tool under `tools/` |
| Verdict rule | "not worse" = every KPI within its noise band of the previous accepted row; a regression blocks the unit or ships gated off with the residual in §5 |

### 1.1 KPIs per workload

| Workload | KPIs |
|---|---|
| Every `test/xpu` kernel, two shapes | duration p50 / p95 (µs); bytes moved; achieved bandwidth fraction; derived class |
| cajeta-llm decode (4-bit model) | tokens per second; per-token p99 (ms); attention kernel duration |
| cajeta-llm prefill (2,048 tokens) | milliseconds; matrix-core fraction |
| CG stand-in (stencil SpMV + 2 dots + 3 axpys, 10,000 iterations) | iterations per second; per-node host cost (µs); queue-empty time (%) |
| One-element degenerate problem | framework overhead per iteration (µs) |
| Frame stand-in (12 dependent kernels, 16.67 ms period) | frame p50 / p99 (ms); missed frames per 10,000; barrier count |
| Protected + best-effort pair | protected p99 slowdown vs solo (%); best-effort throughput (% of solo); goodput |
| Seam | cost per launch at 5 / 50 / 200 µs kernels (µs) |
| Scheduler | CPU share (%); decision cost (µs); graph maintenance per submission (µs) |
| Energy (where power is readable) | joules per period; joules per token; sampling latency of the source |

## 2. Device facts measured (calibration set, per device)

Filled by scheduling Unit 2. Absent means the device could not report it.

| Fact | gfx1151 | CPU backend | RTX 4090 |
|---|---|---|---|
| Matrix-core peak (TFLOP/s) | | — | |
| Vector peak (TFLOP/s) | | | |
| Achievable bandwidth, idle (GB/s) | | | |
| Achievable bandwidth under host load (GB/s) | | — | — |
| Ridge point (FLOP/byte) | | | |
| Bytes in flight to saturate | | — | |
| Launch cost α / β | | | |
| Event cost (µs) | | | |
| Graph node creation / instantiate (µs) | | | |
| Inter-queue dispatch delay (µs) | | — | |
| Scheduling-delay coefficients vs resident count | | | |
| Yield latency (µs) | | | |
| Occupancy headroom probe (4 → 48 units, ms) | | | |
| Picojoules per op by precision | | — | |
| Base power (W) | | — | |
| The 2.2 µs vs 8 µs launch-gap question (one method, both devices) | | — | |

## 3. Baseline (before any scheduler code)

Filled by scheduling Unit 0. One row per (workload, device, KPI).

| Workload | Device | KPI | Median | Noise band | Commit | Date |
|---|---|---|---|---|---|---|
| | | | | | | |

## 4. Trials

One row per configuration tried, in order. `Before` is the previous accepted
row for the same (workload, device, KPI). Verdict is one of `keep`, `revert`,
`gate-off`, `blocked`.

| Trial | Date | Plan / unit | Workload | Device | KPI | Before | After | Delta | Noise band | Commit | Verdict | Note |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| T-000 | | | | | | | | | | | | |

## 5. Residuals

Changes that measured worse or flat and shipped gated off, with the row that
decided it and what would reopen it.

| Residual | Trial | Why | Reopen when |
|---|---|---|---|
| | | | |

## 6. Closing summary (after the last unit)

Per workload and device: baseline row, closing row, better / same / worse
with the noise band. Nothing may read `worse`.

| Workload | Device | KPI | Baseline | Closing | Verdict |
|---|---|---|---|---|---|
| | | | | | |
