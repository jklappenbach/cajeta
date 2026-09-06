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
| Instrument | Host clock (`Clock.nanoTime`) around launch and sync, in two modes: **isolated** (one launch, one sync — kernel plus launch plus sync latency) and **pipelined** (fifty queued launches, one sync, divided by fifty — the per-kernel cost a full queue sees), so a row means the same thing on every backend. The profiler's AMD device tier (rocprofiler-sdk, bound from `$ROCM_PATH/lib` — this box's user-local ROCm 7.11.0 tree carries it; proven by a loader trace, since the runtime prints nothing when it binds) records device spans, and since 0.2.4 (2026-09-06) every workload gets a profiled pass after its timed run — one kernel shape or seam target per pass — whose per-kernel spans become rows beside the host-clocked ones (`device_span`, `device_time_per_iteration`, `queue_empty_time`, and on the llm run `matrix_core_fraction`, `device_busy`, both `attention_kernel_duration`s; `xpubench-report spans`). Every such figure is an average span times the launch count the harness recorded, never a total: the capture ring overwrites its oldest records, `cajeta profile summary` now reports `gpu_records_kept` / `_dropped`, and a pass whose ring dropped or whose counts fall short of the launches issued yields `pending` rows carrying the counts. Rings are sized per pass (65 K kernels and seam, 256 K CG, 4 M llm). CUPTI on NVIDIA when that session runs. |
| Warm-up | first-touch pages committed and every kernel run once per shape before timing (the first-touch finding: ~890 ms one-time on fresh buffers) |
| Runs | 5 blocks per KPI; a block's median is one sample of the noise band (min/max over blocks); p95 is nearest-rank over every individual sample (20 per block for kernels, 100 for the seam probe, every frame for the frame stand-in); cajeta-llm: 5 processes, one run each |
| Idle gate | no other `cajeta_test`, `xpubench`, `gpuparity`, `SchedThroughput`, or llama.cpp binary alive, matched by process NAME (`pgrep -a`) excluding the harness itself; exit 3 and no rows otherwise; load average printed beside it. Tested both ways in `XpuBenchHarnessTests` |
| Arm order | alternated A/B/B/A across runs (a fixed order let a decaying load fake a speedup once); `Stats.abbaOrder`, tested |
| Storage | harness rows under repo `tmp/bench/` (never `/tmp`), one JSON object per line per (workload, shape, KPI), each carrying device / backend / driver / compiler / power mode / date / arm; tables rendered by `tools/xpubench-report` (`baseline` and `trial` verbs) |
| Harness | `test/xpu/bench/run.sh` (`all`, `hip`, `cpu`, `smoke`); sources `test/xpu/bench/src/xpubench/`; the workload table is in its README |
| Verdict rule | "not worse" = every KPI within its noise band of the previous accepted row; a regression blocks the unit or ships gated off with the residual in §5. Frame stand-in: verdicts read `frame_p99` and `missed_frames_per_10000`; `frame_p50` is informational (developer, 2026-09-06: power mode stays `auto`, what a game gets on this part; a unit claiming a jitter reduction shows it over several runs). Derived rows (`achieved_bandwidth`, `achieved_rate`, `wall_per_iteration`) carry the band their source implies. A before row with no band (n = 1: `frame_p99`, `missed_frames_per_10000`, seam `kernel_time`, the pair ratios) gets the verdict `single` — reported with its delta, not gated — until 0.3.3 gives those KPIs a run-to-run band from the day-apart rerun; the first real trial (2026-09-06, the fission fix) read twelve phantom regressions against zero-width bands before this rule |
| Power mode | `auto`, recorded per row; never pinned for a baseline or a verdict |

### 1.1 KPIs per workload

| Workload | KPIs |
|---|---|
| Representative kernel set (saxpy, dot, stencil5, reduceSum, matmulTiled, wmmaGemm, gather — one per class; "every `test/xpu` kernel" was eighty inline probes and was narrowed in Unit 0), two shapes | duration isolated p50 / p95 and pipelined (µs); bytes moved; achieved bandwidth and rate; bandwidth fraction (pending §2); declared class |
| cajeta-llm decode (4-bit model) | tokens per second; per-token p99 (ms); attention kernel duration |
| cajeta-llm prefill (2,048 tokens) | milliseconds; matrix-core fraction |
| CG stand-in (stencil SpMV + 2 dots as partials + 1 final + 3 axpys, seven launches, 10,000 iterations) | iterations per second; per-node host cost (µs); wall per iteration (µs); queue-empty time (%) — pending, needs device spans (§5) |
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

Filled by scheduling Unit 0 on 2026-09-06 (first legs on commit `8fea9b63`)
and rerun the same evening on `f721f0bf` with the profiled passes of 0.2.4,
which is the baseline of record: `tmp/bench/rows-hip-20260906-1532.jsonl`
for gfx1151 and `tmp/bench/rows-cpu-20260906-1529.jsonl` for the CPU
backend (gitignored; the tables below are the rendered copy). Each leg ran
as two invocations of `run.sh` under one identity (`KEEP_ROWS=1` with the
first invocation's `DATE` and `STAMP`), and the kernel profiled passes were
rerun once (`SPANS_ONLY=1`) after a launch-count bookkeeping fix
(`8663d500`); a later row supersedes an earlier one with the same key. The
first legs — `rows-hip-20260906-1810`, `rows-cpu-20260906-1810`, and the
CPU rerun `rows-cpu-20260906-1502-full` after the barrier-fission fix
(`e0fa4871`) — are the `Before` sides of the trials in §4. One row per (workload, shape, KPI); the noise band is
min/max over five blocks (over frames for the frame stand-in); `pending`
rows name what could not be measured here and why. `bandwidth_fraction` is
pending on every kernel until §2 has the device's achievable bandwidth; the
`launches` rows the profiled passes use (iterations issued, kernel names
with multiplicity) are in the JSON and hidden from the tables.

### 3.1 What the baseline says (read before the tables)

- **Launch cost depends on the kernel behind it.** On HIP the launch call
  costs 0.87 µs when the kernel is 5 µs and ~12–15 µs when it is 50 or
  200 µs (`seam.launch_call`); pipelined, the per-launch overhead is within
  noise of zero at every duration; isolated (launch + sync) it is 5 µs at
  5 µs and 14–18 µs at 50/200 µs. The seam a scheduler adds must be read
  against these, not against a single number.
- **The uncontrolled co-run.** With a saturated best-effort stream beside
  the 12-kernel frame, the frame's p99 goes from 5.10 ms to 10.19 ms
  (+100%), no frame misses the 16.67 ms period, and the best-effort stream
  runs at 76% of its solo bandwidth (170 of 223 GB/s). This is the row §5's
  reservation and §6's rate control are measured against.
- **The CG stand-in** runs 10,141 iterations/s (98.6 µs per iteration of
  seven launches, 0.83 µs of host time per launch); the one-element
  degenerate loop runs 67,600 iterations/s (14.8 µs per iteration), so the
  framework floor is ~2.1 µs per launch when nothing else is happening.
- **Memory versus matrix cores.** saxpy at 16M elements reaches 223 GB/s
  pipelined (the coalesced ceiling this part has shown before); the f32
  LDS-tiled GEMM 1.66 TFLOP/s; the f16 tile GEMM on the matrix cores
  5.4 TFLOP/s; a random gather runs 11× slower than the streaming kernel
  at the same element count (10.0 ms vs 0.92 ms).
- **cajeta-llm** on the same compiler: 2,048-token prefill 9.82 s (208.6
  tok/s), decode 40.5 tok/s (24.7 ms per token), five processes agreeing
  within 0.3%. From the profiled run (0.2.4 rows; ring 43,874 of 43,874
  records kept): the device is busy 98.7% of prefill + decode wall;
  prefill's device time is 92.9% on the WMMA kernels (a lower bound —
  kernels both phases share are counted as prefill; §3.5's hand split gave
  ~94%); decode attention takes 58.0 µs per layer per token (attention +
  its reduce), prefill attention 904.6 µs per layer per 128-token chunk.
- **The device is idle for 15% of the CG loop.** From the profiled passes:
  the CG stand-in's seven kernels occupy gfx1151 for 92.6 µs of a 98.0 µs
  iteration (`queue_empty_time` 15.4%), and the one-element degenerate loop
  for 6.2 of 15.3 µs (74.5% empty) — the launch gaps a scheduler that
  submits ahead can close, measured rather than inferred. On the CPU
  backend a launch is the kernel (0.5% and 16.7%).
- **The seam, seen from the device.** One profiled pass per target: the
  `spin` kernel's device span is 5.9 / 50.6 / 198.8 µs where the
  host-clocked pipelined time is 9.5 / 52.7 / 207.4 µs, so a pipelined
  launch on HIP leaves 2–9 µs of device idle between kernels beyond the
  kernel itself; the CPU backend's spans (7.5 / 53.4 / 190.7 µs) are the
  inline launches and bracket the host figure. A per-kernel `device_span`
  row now sits beside every `duration_isolated` in §3.2 and §3.3.
- **Two kernels were fixed before the baseline was taken.** The first
  reductions used a same-address float atomic and were contention-bound on
  HIP (446 µs for 1M elements, 2.3 GFLOP/s, a bimodal band at 16M); the
  baseline carries the two-stage form (per-block partials, one final
  launch), which took the CG stand-in from 1,190 to 10,141 iterations/s.
  That change is in the kernels, not in any scheduler.
- **The CPU backend declined three kernels in the first leg** (`reduceSum`,
  `finalSum2`, `matmulTiled`: a barrier loop whose code after the last
  barrier was the latch block), so `dot`, `reduceSum`, `matmulTiled`, `cg`
  and `degenerate` were pending on that leg. Fixed the same day
  (`cpu-barrier-fission-loops`, commit `e0fa4871`) and the leg rerun: 112
  rows, 14 pending, all of them `bandwidth_fraction` (§2). The CPU numbers
  worth knowing: `matmulTiled` 1024² runs at 65 GFLOP/s (33 ms a launch;
  the WMMA software tile does 115), the tree reduce moves 16 GB/s against
  saxpy's 260, and the CG stand-in does 2,023 iterations/s (494 µs per
  iteration, 70.6 µs of host time per launch — the CPU backend runs each
  launch inline, so host cost is the kernel).
- **Run-to-run drift on the CPU backend is wider than one run's band.**
  Three reruns of the fixed binary, minutes apart, disagreed by up to 31%
  on `dot` 1M isolated (78 → 103 µs) and 18% on `stencil5` 1024² pipelined,
  against five-block bands a few percent wide; a control trial of two
  identical-code runs read 9 of 43 banded KPIs as `worse`. The gfx1151
  rerun the same evening (T-003, no device code changed) flagged 4 of 47:
  `matmulTiled` 2048² by 0.8–1.2%, the degenerate loop's host cost by 7%,
  one seam overhead by 4%. The trial verb's bands come from one run, so a
  verdict on either leg is not readable until 0.3.2 and 0.3.3 set bands
  from repeated runs (§4, §5).
- **Frame p50 is noisier than p99 suggests**: 4.18 ms median with a
  3.5–6.6 ms band over 600 frames, unscheduled and solo. Later units read
  the p99 and the miss count, not the p50, for the frame stand-in.

### 3.2 gfx1151 (HIP)

Identity: AMD RYZEN AI MAX+ 395 w/ Radeon 8060S | hip | linux 7.0.0-30-generic, ROCm 7.2.53150-7b886380f9 | f721f0bf | auto | 2026-09-06T19:32:22Z

| Workload | Shape | Device | KPI | Median | Noise band | p95 | n | Note |
|---|---|---|---|---|---|---|---|---|
| kernel.saxpy | 1048576 | hip | duration_isolated | 19.97 us | [19.93, 20.82] | 43.23 | 5 | launch+sync per sample; class memory-bound |
| kernel.saxpy | 1048576 | hip | duration_pipelined | 15.08 us | [15.05, 15.18] |  | 5 | 50 queued launches / count; class memory-bound |
| kernel.saxpy | 1048576 | hip | bytes_moved | 12582912 bytes | [12582912, 12582912] |  | 1 | ideal traffic: every element read/written once |
| kernel.saxpy | 1048576 | hip | achieved_bandwidth | 834.3 GB/s | [828.7, 835.9] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.saxpy | 1048576 | hip | achieved_rate | 139.1 GFLOP/s | [138.1, 139.3] |  | 5 | flops / duration_pipelined; band from the duration band |
| kernel.saxpy | 1048576 | hip | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.saxpy | 16777216 | hip | duration_isolated | 907.5 us | [904.4, 913.4] | 920.9 | 5 | launch+sync per sample; class memory-bound |
| kernel.saxpy | 16777216 | hip | duration_pipelined | 908.1 us | [905.0, 930.3] |  | 5 | 50 queued launches / count; class memory-bound |
| kernel.saxpy | 16777216 | hip | bytes_moved | 201326592 bytes | [201326592, 201326592] |  | 1 | ideal traffic: every element read/written once |
| kernel.saxpy | 16777216 | hip | achieved_bandwidth | 221.7 GB/s | [216.4, 222.5] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.saxpy | 16777216 | hip | achieved_rate | 36.95 GFLOP/s | [36.07, 37.08] |  | 5 | flops / duration_pipelined; band from the duration band |
| kernel.saxpy | 16777216 | hip | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.dot | 1048576 | hip | duration_isolated | 25.72 us | [25.53, 26.54] | 104.4 | 5 | launch+sync per sample; class memory-bound+reduce |
| kernel.dot | 1048576 | hip | duration_pipelined | 20.65 us | [20.5, 23.83] |  | 5 | 50 queued launches / count; class memory-bound+reduce |
| kernel.dot | 1048576 | hip | bytes_moved | 8388608 bytes | [8388608, 8388608] |  | 1 | ideal traffic: every element read/written once |
| kernel.dot | 1048576 | hip | achieved_bandwidth | 406.2 GB/s | [352.0, 409.2] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.dot | 1048576 | hip | achieved_rate | 101.6 GFLOP/s | [88, 102.3] |  | 5 | flops / duration_pipelined; band from the duration band |
| kernel.dot | 1048576 | hip | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.dot | 16777216 | hip | duration_isolated | 655.1 us | [654.5, 660.5] | 666.9 | 5 | launch+sync per sample; class memory-bound+reduce |
| kernel.dot | 16777216 | hip | duration_pipelined | 644.8 us | [639.8, 699.6] |  | 5 | 50 queued launches / count; class memory-bound+reduce |
| kernel.dot | 16777216 | hip | bytes_moved | 134217728 bytes | [134217728, 134217728] |  | 1 | ideal traffic: every element read/written once |
| kernel.dot | 16777216 | hip | achieved_bandwidth | 208.2 GB/s | [191.8, 209.8] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.dot | 16777216 | hip | achieved_rate | 52.04 GFLOP/s | [47.96, 52.44] |  | 5 | flops / duration_pipelined; band from the duration band |
| kernel.dot | 16777216 | hip | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.stencil5 | 1024x1024 | hip | duration_isolated | 21.92 us | [21.88, 89.01] | 106.8 | 5 | launch+sync per sample; class memory-bound |
| kernel.stencil5 | 1024x1024 | hip | duration_pipelined | 16.65 us | [16.64, 16.98] |  | 5 | 50 queued launches / count; class memory-bound |
| kernel.stencil5 | 1024x1024 | hip | bytes_moved | 8388608 bytes | [8388608, 8388608] |  | 1 | ideal traffic: every element read/written once |
| kernel.stencil5 | 1024x1024 | hip | achieved_bandwidth | 503.8 GB/s | [494.0, 504.0] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.stencil5 | 1024x1024 | hip | achieved_rate | 503.8 GFLOP/s | [494.0, 504.0] |  | 5 | flops / duration_pipelined; band from the duration band |
| kernel.stencil5 | 1024x1024 | hip | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.stencil5 | 4096x4096 | hip | duration_isolated | 599.0 us | [590.5, 624.4] | 630.5 | 5 | launch+sync per sample; class memory-bound |
| kernel.stencil5 | 4096x4096 | hip | duration_pipelined | 601.8 us | [593.5, 615.0] |  | 5 | 50 queued launches / count; class memory-bound |
| kernel.stencil5 | 4096x4096 | hip | bytes_moved | 134217728 bytes | [134217728, 134217728] |  | 1 | ideal traffic: every element read/written once |
| kernel.stencil5 | 4096x4096 | hip | achieved_bandwidth | 223.0 GB/s | [218.3, 226.1] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.stencil5 | 4096x4096 | hip | achieved_rate | 223.0 GFLOP/s | [218.3, 226.1] |  | 5 | flops / duration_pipelined; band from the duration band |
| kernel.stencil5 | 4096x4096 | hip | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.reduceSum | 1048576 | hip | duration_isolated | 36.91 us | [31.66, 37.3] | 130.5 | 5 | launch+sync per sample; class sync-bound |
| kernel.reduceSum | 1048576 | hip | duration_pipelined | 26.37 us | [26.26, 26.52] |  | 5 | 50 queued launches / count; class sync-bound |
| kernel.reduceSum | 1048576 | hip | bytes_moved | 4194304 bytes | [4194304, 4194304] |  | 1 | ideal traffic: every element read/written once |
| kernel.reduceSum | 1048576 | hip | achieved_bandwidth | 159.1 GB/s | [158.1, 159.7] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.reduceSum | 1048576 | hip | achieved_rate | 39.77 GFLOP/s | [39.53, 39.92] |  | 5 | flops / duration_pipelined; band from the duration band |
| kernel.reduceSum | 1048576 | hip | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.reduceSum | 16777216 | hip | duration_isolated | 548.8 us | [537.4, 577.9] | 584.6 | 5 | launch+sync per sample; class sync-bound |
| kernel.reduceSum | 16777216 | hip | duration_pipelined | 538.2 us | [533.1, 551.3] |  | 5 | 50 queued launches / count; class sync-bound |
| kernel.reduceSum | 16777216 | hip | bytes_moved | 67108864 bytes | [67108864, 67108864] |  | 1 | ideal traffic: every element read/written once |
| kernel.reduceSum | 16777216 | hip | achieved_bandwidth | 124.7 GB/s | [121.7, 125.9] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.reduceSum | 16777216 | hip | achieved_rate | 31.17 GFLOP/s | [30.43, 31.47] |  | 5 | flops / duration_pipelined; band from the duration band |
| kernel.reduceSum | 16777216 | hip | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.matmulTiled | 512^2 | hip | duration_isolated | 179.9 us | [175.1, 180.4] | 185.0 | 5 | launch+sync per sample; class compute-bound |
| kernel.matmulTiled | 512^2 | hip | duration_pipelined | 162.9 us | [161.3, 168.9] |  | 5 | 50 queued launches / count; class compute-bound |
| kernel.matmulTiled | 512^2 | hip | bytes_moved | 3145728 bytes | [3145728, 3145728] |  | 1 | ideal traffic: every element read/written once |
| kernel.matmulTiled | 512^2 | hip | achieved_bandwidth | 19.31 GB/s | [18.62, 19.51] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.matmulTiled | 512^2 | hip | achieved_rate | 1647.4 GFLOP/s | [1589.0, 1664.5] |  | 5 | flops / duration_pipelined; band from the duration band |
| kernel.matmulTiled | 512^2 | hip | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.matmulTiled | 2048^2 | hip | duration_isolated | 10314.9 us | [10288.0, 10386.5] | 11281.2 | 5 | launch+sync per sample; class compute-bound |
| kernel.matmulTiled | 2048^2 | hip | duration_pipelined | 10446.7 us | [10292.8, 10588.3] |  | 5 | 50 queued launches / count; class compute-bound |
| kernel.matmulTiled | 2048^2 | hip | bytes_moved | 50331648 bytes | [50331648, 50331648] |  | 1 | ideal traffic: every element read/written once |
| kernel.matmulTiled | 2048^2 | hip | achieved_bandwidth | 4.82 GB/s | [4.75, 4.89] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.matmulTiled | 2048^2 | hip | achieved_rate | 1644.5 GFLOP/s | [1622.5, 1669.1] |  | 5 | flops / duration_pipelined; band from the duration band |
| kernel.matmulTiled | 2048^2 | hip | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.wmmaGemm | 512^2 | hip | duration_isolated | 77.58 us | [66.38, 297.3] | 298.8 | 5 | launch+sync per sample; class matrix-core |
| kernel.wmmaGemm | 512^2 | hip | duration_pipelined | 62.69 us | [60.81, 63.92] |  | 5 | 50 queued launches / count; class matrix-core |
| kernel.wmmaGemm | 512^2 | hip | bytes_moved | 2097152 bytes | [2097152, 2097152] |  | 1 | ideal traffic: every element read/written once |
| kernel.wmmaGemm | 512^2 | hip | achieved_bandwidth | 33.45 GB/s | [32.81, 34.49] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.wmmaGemm | 512^2 | hip | achieved_rate | 4281.9 GFLOP/s | [4199.8, 4414.1] |  | 5 | flops / duration_pipelined; band from the duration band |
| kernel.wmmaGemm | 512^2 | hip | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.wmmaGemm | 2048^2 | hip | duration_isolated | 3135.6 us | [3129.4, 3173.4] | 3231.7 | 5 | launch+sync per sample; class matrix-core |
| kernel.wmmaGemm | 2048^2 | hip | duration_pipelined | 3133.7 us | [3095.3, 3174.9] |  | 5 | 50 queued launches / count; class matrix-core |
| kernel.wmmaGemm | 2048^2 | hip | bytes_moved | 33554432 bytes | [33554432, 33554432] |  | 1 | ideal traffic: every element read/written once |
| kernel.wmmaGemm | 2048^2 | hip | achieved_bandwidth | 10.71 GB/s | [10.57, 10.84] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.wmmaGemm | 2048^2 | hip | achieved_rate | 5482.2 GFLOP/s | [5411.2, 5550.4] |  | 5 | flops / duration_pipelined; band from the duration band |
| kernel.wmmaGemm | 2048^2 | hip | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.gather | 1048576 | hip | duration_isolated | 108.0 us | [104.0, 108.2] | 113.3 | 5 | launch+sync per sample; class indirect |
| kernel.gather | 1048576 | hip | duration_pipelined | 87.74 us | [87.63, 98.22] |  | 5 | 50 queued launches / count; class indirect |
| kernel.gather | 1048576 | hip | bytes_moved | 12582912 bytes | [12582912, 12582912] |  | 1 | ideal traffic: every element read/written once |
| kernel.gather | 1048576 | hip | achieved_bandwidth | 143.4 GB/s | [128.1, 143.6] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.gather | 1048576 | hip | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.gather | 16777216 | hip | duration_isolated | 9587.7 us | [9585.1, 9588.3] | 9743.3 | 5 | launch+sync per sample; class indirect |
| kernel.gather | 16777216 | hip | duration_pipelined | 9611.2 us | [9602.3, 9643.5] |  | 5 | 50 queued launches / count; class indirect |
| kernel.gather | 16777216 | hip | bytes_moved | 201326592 bytes | [201326592, 201326592] |  | 1 | ideal traffic: every element read/written once |
| kernel.gather | 16777216 | hip | achieved_bandwidth | 20.95 GB/s | [20.88, 20.97] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.gather | 16777216 | hip | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| cg | 1024x1024x10000 | hip | iterations_per_second | 10208.7 iter/s | [9956.4, 10248.1] |  | 5 | 7 launches/iteration (stencil + 2 dot partials + 1 final + 3 axpy), 2000 iterations per block, one sync per block |
| cg | 1024x1024x10000 | hip | host_cost_per_node | 0.843 us | [0.828, 0.962] |  | 5 | host wall time inside the launch calls / launches |
| cg | 1024x1024x10000 | hip | wall_per_iteration | 97.96 us | [97.58, 100.4] |  | 5 | 1e6 / iterations_per_second; band from its band |
| degenerate | 1x1x10000 | hip | iterations_per_second | 65565.6 iter/s | [65315.8, 68723.9] |  | 5 | 7 launches/iteration (stencil + 2 dot partials + 1 final + 3 axpy), 2000 iterations per block, one sync per block |
| degenerate | 1x1x10000 | hip | host_cost_per_node | 0.953 us | [0.866, 0.971] |  | 5 | host wall time inside the launch calls / launches |
| degenerate | 1x1x10000 | hip | wall_per_iteration | 15.25 us | [14.55, 15.31] |  | 5 | 1e6 / iterations_per_second; band from its band |
| cg | 1024x1024x10000 | hip | device_span | 92.63 us | [92.63, 92.63] |  | 1 | device tier, avg span x launches per iteration (stencil5,dot,dot,finalSum2,saxpy,saxpy,saxpy), 10001 iterations profiled |
| cg | 1024x1024x10000 | hip | device_time_per_iteration | 92.63 us | [92.63, 92.63] |  | 1 | device tier, avg span x launches per iteration (stencil5,dot,dot,finalSum2,saxpy,saxpy,saxpy), 10001 iterations profiled |
| cg | 1024x1024x10000 | hip | queue_empty_time | 15.4 % | [15.4, 15.4] |  | 1 | 100 x (1 - device_time_per_iteration / wall_per_iteration 109.5 us) in the profiled pass |
| degenerate | 1x1x10000 | hip | device_span | 6.16 us | [6.16, 6.16] |  | 1 | device tier, avg span x launches per iteration (stencil5,dot,dot,finalSum2,saxpy,saxpy,saxpy), 10001 iterations profiled |
| degenerate | 1x1x10000 | hip | device_time_per_iteration | 6.16 us | [6.16, 6.16] |  | 1 | device tier, avg span x launches per iteration (stencil5,dot,dot,finalSum2,saxpy,saxpy,saxpy), 10001 iterations profiled |
| degenerate | 1x1x10000 | hip | queue_empty_time | 74.5 % | [74.5, 74.5] |  | 1 | 100 x (1 - device_time_per_iteration / wall_per_iteration 24.1 us) in the profiled pass |
| frame | 8388608x12@16.667ms | hip | frame_p50 | 4.162 ms | [3.544, 5.036] | 4.908 | 600 | 12 dependent chainStep launches + one sync per frame; band is min/max over frames |
| frame | 8388608x12@16.667ms | hip | frame_p99 | 4.908 ms | [4.908, 4.908] |  | 1 | nearest-rank over frames |
| frame | 8388608x12@16.667ms | hip | missed_frames_per_10000 | 0 frames | [0, 0] |  | 1 | 0 of 600 frames exceeded the period |
| frame | 8388608x12@16.667ms | hip | sync_points_per_frame | 1 count | [1, 1] |  | 1 | one host sync per frame; the 11 intra-frame dependencies ride stream order |
| pair | 8388608x12@16.667ms | hip | frame_p50 | 9.313 ms | [9.147, 10.638] | 10.177 | 600 | 12 dependent chainStep launches + one sync per frame; band is min/max over frames |
| pair | 8388608x12@16.667ms | hip | frame_p99 | 10.177 ms | [10.177, 10.177] |  | 1 | nearest-rank over frames |
| pair | 8388608x12@16.667ms | hip | missed_frames_per_10000 | 0 frames | [0, 0] |  | 1 | 0 of 600 frames exceeded the period |
| pair | 8388608x12@16.667ms | hip | sync_points_per_frame | 1 count | [1, 1] |  | 1 | one host sync per frame; the 11 intra-frame dependencies ride stream order |
| pair | 8388608x12@16.667ms | hip | besteffort_throughput | 170.6 GB/s | [170.6, 170.6] |  | 1 | 8496 saxpy(16M) launches on the second stream, batches of 24 refilled on completion |
| pair | 8388608x12@16.667ms | hip | protected_p99_slowdown | 107.3 % | [107.3, 107.3] |  | 1 | frame p99 co-run vs solo 4.908 ms |
| pair | 8388608x12@16.667ms | hip | besteffort_pct_of_solo | 79.8 % | [79.8, 79.8] |  | 1 | vs solo pipelined saxpy(16M) 213.68 GB/s |
| pair | 8388608x12@16.667ms | hip | goodput | 89.9 % | [89.9, 89.9] |  | 1 | mean of protected on-time % and best-effort % of solo |
| seam | 5us | hip | kernel_time | 9.47 us | [9.47, 9.47] |  | 1 | spin(1000) pipelined x100; calibrated from 4.999 ns/iter |
| seam | 5us | hip | launch_call | 0.871 us | [0.831, 0.872] | 0.962 | 5 | host time inside the launch statement |
| seam | 5us | hip | pipelined_overhead | -2.366 us | [-2.377, -2.344] |  | 5 | (100 queued launches + sync)/100 minus kernel_time |
| seam | 5us | hip | isolated_overhead | 3.12 us | [3.09, 3.129] |  | 5 | launch + sync minus kernel_time |
| seam | 50us | hip | kernel_time | 52.69 us | [52.69, 52.69] |  | 1 | spin(9980) pipelined x100; calibrated from 5.01 ns/iter |
| seam | 50us | hip | launch_call | 14.818 us | [14.287, 14.837] | 14.928 | 5 | host time inside the launch statement |
| seam | 50us | hip | pipelined_overhead | 0.274 us | [-0.181, 3.6] |  | 5 | (100 queued launches + sync)/100 minus kernel_time |
| seam | 50us | hip | isolated_overhead | 19.034 us | [18.644, 19.224] |  | 5 | launch + sync minus kernel_time |
| seam | 200us | hip | kernel_time | 207.4 us | [207.4, 207.4] |  | 1 | spin(39974) pipelined x100; calibrated from 5.003 ns/iter |
| seam | 200us | hip | launch_call | 10.399 us | [10.35, 14.817] | 14.998 | 5 | host time inside the launch statement |
| seam | 200us | hip | pipelined_overhead | -0.705 us | [-1.823, 1.599] |  | 5 | (100 queued launches + sync)/100 minus kernel_time |
| seam | 200us | hip | isolated_overhead | 15.191 us | [13.318, 17.566] |  | 5 | launch + sync minus kernel_time |
| seam | 5us | hip | device_span | 5.87 us | [5.87, 5.87] |  | 1 | device tier, avg span x launches per iteration (spin), 500 iterations profiled |
| seam | 50us | hip | device_span | 50.55 us | [50.55, 50.55] |  | 1 | device tier, avg span x launches per iteration (spin), 500 iterations profiled |
| seam | 200us | hip | device_span | 198.8 us | [198.8, 198.8] |  | 1 | device tier, avg span x launches per iteration (spin), 500 iterations profiled |
| llm.prefill | prompt2048+gen64 | hip | prefill_ms | 9816.4 ms | [9802.6, 9862.0] |  | 5 | SchedThroughput, one process per run, chunked at the engine's default |
| llm.prefill | prompt2048+gen64 | hip | prefill_tokens_per_second | 208.6 tok/s | [207.7, 208.9] |  | 5 | from the same runs |
| llm.decode | prompt2048+gen64 | hip | tokens_per_second | 40.51 tok/s | [40.42, 40.56] |  | 5 | batch 1, greedy, 64 generated tokens after the prompt |
| llm.decode | prompt2048+gen64 | hip | ms_per_token | 24.68 ms | [24.66, 24.74] |  | 5 | mean per token per run |
| llm.decode | prompt2048+gen64 | hip | per_token_p99 | pending | | | | SchedThroughput prints a per-run mean, not per-token latencies |
| llm.prefill | prompt2048+gen64 | hip | matrix_core_fraction | 92.9 % | [92.9, 92.9] |  | 1 | WMMA kernels' device time / prefill device time (all kernels minus the decode-only ones; shared kernels count as prefill — a lower bound) |
| llm | prompt2048+gen64 | hip | device_busy | 98.7 % | [98.7, 98.7] |  | 1 | device time over prefill + decode wall of the profiled process (11548 ms) |
| llm.decode | prompt2048+gen64 | hip | attention_kernel_duration | 58 us | [58, 58] |  | 1 | decode attention + its reduce, avg device span per layer per token |
| llm.prefill | prompt2048+gen64 | hip | attention_kernel_duration | 904.6 us | [904.6, 904.6] |  | 1 | prefill attention, avg device span per layer per chunk |
| kernel.saxpy | 1048576 | hip | device_span | 18.29 us | [18.29, 18.29] |  | 1 | device tier, avg span x launches per iteration (saxpy), 111 iterations profiled |
| kernel.dot | 1048576 | hip | device_span | 18.29 us | [18.29, 18.29] |  | 1 | device tier, avg span x launches per iteration (dot,finalSum2), 111 iterations profiled |
| kernel.stencil5 | 1024x1024 | hip | device_span | 19.66 us | [19.66, 19.66] |  | 1 | device tier, avg span x launches per iteration (stencil5), 111 iterations profiled |
| kernel.reduceSum | 1048576 | hip | device_span | 28.15 us | [28.15, 28.15] |  | 1 | device tier, avg span x launches per iteration (reduceSum,finalSum2), 111 iterations profiled |
| kernel.matmulTiled | 512^2 | hip | device_span | 210.7 us | [210.7, 210.7] |  | 1 | device tier, avg span x launches per iteration (matmulTiled), 17 iterations profiled |
| kernel.wmmaGemm | 512^2 | hip | device_span | 112.8 us | [112.8, 112.8] |  | 1 | device tier, avg span x launches per iteration (wmmaGemm), 17 iterations profiled |
| kernel.gather | 1048576 | hip | device_span | 86.07 us | [86.07, 86.07] |  | 1 | device tier, avg span x launches per iteration (gather), 111 iterations profiled |
| kernel.saxpy | 16777216 | hip | device_span | 897.0 us | [897.0, 897.0] |  | 1 | device tier, avg span x launches per iteration (saxpy), 111 iterations profiled |
| kernel.dot | 16777216 | hip | device_span | 633.9 us | [633.9, 633.9] |  | 1 | device tier, avg span x launches per iteration (dot,finalSum2), 111 iterations profiled |
| kernel.stencil5 | 4096x4096 | hip | device_span | 610.4 us | [610.4, 610.4] |  | 1 | device tier, avg span x launches per iteration (stencil5), 111 iterations profiled |
| kernel.reduceSum | 16777216 | hip | device_span | 574.1 us | [574.1, 574.1] |  | 1 | device tier, avg span x launches per iteration (reduceSum,finalSum2), 111 iterations profiled |
| kernel.matmulTiled | 2048^2 | hip | device_span | 10357.2 us | [10357.2, 10357.2] |  | 1 | device tier, avg span x launches per iteration (matmulTiled), 17 iterations profiled |
| kernel.wmmaGemm | 2048^2 | hip | device_span | 3163.3 us | [3163.3, 3163.3] |  | 1 | device tier, avg span x launches per iteration (wmmaGemm), 17 iterations profiled |
| kernel.gather | 16777216 | hip | device_span | 11692.3 us | [11692.3, 11692.3] |  | 1 | device tier, avg span x launches per iteration (gather), 111 iterations profiled |

### 3.3 CPU backend

The CPU backend runs every launch inline on the calling thread, so
`launch_call` equals the kernel's own time, a second stream is not
concurrent (the pair row shows the frame absorbing the best-effort work),
and the pipelined and isolated modes coincide.

Identity: AMD RYZEN AI MAX+ 395 w/ Radeon 8060S | cpu | linux 7.0.0-30-generic | f721f0bf | auto | 2026-09-06T19:29:50Z

| Workload | Shape | Device | KPI | Median | Noise band | p95 | n | Note |
|---|---|---|---|---|---|---|---|---|
| kernel.saxpy | 1048576 | cpu | duration_isolated | 48.88 us | [43.47, 60.24] | 65.25 | 5 | launch+sync per sample; class memory-bound |
| kernel.saxpy | 1048576 | cpu | duration_pipelined | 49.83 us | [47.31, 54.19] |  | 5 | 50 queued launches / count; class memory-bound |
| kernel.saxpy | 1048576 | cpu | bytes_moved | 12582912 bytes | [12582912, 12582912] |  | 1 | ideal traffic: every element read/written once |
| kernel.saxpy | 1048576 | cpu | achieved_bandwidth | 252.5 GB/s | [232.2, 266.0] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.saxpy | 1048576 | cpu | achieved_rate | 42.09 GFLOP/s | [38.7, 44.33] |  | 5 | flops / duration_pipelined; band from the duration band |
| kernel.saxpy | 1048576 | cpu | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.saxpy | 16777216 | cpu | duration_isolated | 1009.1 us | [989.5, 1127.5] | 1338.2 | 5 | launch+sync per sample; class memory-bound |
| kernel.saxpy | 16777216 | cpu | duration_pipelined | 996.3 us | [984.3, 1019.5] |  | 5 | 50 queued launches / count; class memory-bound |
| kernel.saxpy | 16777216 | cpu | bytes_moved | 201326592 bytes | [201326592, 201326592] |  | 1 | ideal traffic: every element read/written once |
| kernel.saxpy | 16777216 | cpu | achieved_bandwidth | 202.1 GB/s | [197.5, 204.5] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.saxpy | 16777216 | cpu | achieved_rate | 33.68 GFLOP/s | [32.91, 34.09] |  | 5 | flops / duration_pipelined; band from the duration band |
| kernel.saxpy | 16777216 | cpu | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.dot | 1048576 | cpu | duration_isolated | 81.36 us | [72.31, 128.6] | 141.9 | 5 | launch+sync per sample; class memory-bound+reduce |
| kernel.dot | 1048576 | cpu | duration_pipelined | 90.89 us | [89.36, 101.9] |  | 5 | 50 queued launches / count; class memory-bound+reduce |
| kernel.dot | 1048576 | cpu | bytes_moved | 8388608 bytes | [8388608, 8388608] |  | 1 | ideal traffic: every element read/written once |
| kernel.dot | 1048576 | cpu | achieved_bandwidth | 92.3 GB/s | [82.32, 93.88] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.dot | 1048576 | cpu | achieved_rate | 23.07 GFLOP/s | [20.58, 23.47] |  | 5 | flops / duration_pipelined; band from the duration band |
| kernel.dot | 1048576 | cpu | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.dot | 16777216 | cpu | duration_isolated | 2053.0 us | [2038.8, 2134.9] | 2289.4 | 5 | launch+sync per sample; class memory-bound+reduce |
| kernel.dot | 16777216 | cpu | duration_pipelined | 1889.3 us | [1862.2, 2007.5] |  | 5 | 50 queued launches / count; class memory-bound+reduce |
| kernel.dot | 16777216 | cpu | bytes_moved | 134217728 bytes | [134217728, 134217728] |  | 1 | ideal traffic: every element read/written once |
| kernel.dot | 16777216 | cpu | achieved_bandwidth | 71.04 GB/s | [66.86, 72.07] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.dot | 16777216 | cpu | achieved_rate | 17.76 GFLOP/s | [16.71, 18.02] |  | 5 | flops / duration_pipelined; band from the duration band |
| kernel.dot | 16777216 | cpu | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.stencil5 | 1024x1024 | cpu | duration_isolated | 120.1 us | [114.0, 156.2] | 177.0 | 5 | launch+sync per sample; class memory-bound |
| kernel.stencil5 | 1024x1024 | cpu | duration_pipelined | 149.7 us | [133.6, 166.0] |  | 5 | 50 queued launches / count; class memory-bound |
| kernel.stencil5 | 1024x1024 | cpu | bytes_moved | 8388608 bytes | [8388608, 8388608] |  | 1 | ideal traffic: every element read/written once |
| kernel.stencil5 | 1024x1024 | cpu | achieved_bandwidth | 56.02 GB/s | [50.53, 62.8] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.stencil5 | 1024x1024 | cpu | achieved_rate | 56.02 GFLOP/s | [50.53, 62.8] |  | 5 | flops / duration_pipelined; band from the duration band |
| kernel.stencil5 | 1024x1024 | cpu | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.stencil5 | 4096x4096 | cpu | duration_isolated | 2272.0 us | [2258.5, 2313.6] | 2437.9 | 5 | launch+sync per sample; class memory-bound |
| kernel.stencil5 | 4096x4096 | cpu | duration_pipelined | 2148.1 us | [2133.0, 2313.6] |  | 5 | 50 queued launches / count; class memory-bound |
| kernel.stencil5 | 4096x4096 | cpu | bytes_moved | 134217728 bytes | [134217728, 134217728] |  | 1 | ideal traffic: every element read/written once |
| kernel.stencil5 | 4096x4096 | cpu | achieved_bandwidth | 62.48 GB/s | [58.01, 62.92] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.stencil5 | 4096x4096 | cpu | achieved_rate | 62.48 GFLOP/s | [58.01, 62.92] |  | 5 | flops / duration_pipelined; band from the duration band |
| kernel.stencil5 | 4096x4096 | cpu | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.reduceSum | 1048576 | cpu | duration_isolated | 287.2 us | [220.0, 298.2] | 364.1 | 5 | launch+sync per sample; class sync-bound |
| kernel.reduceSum | 1048576 | cpu | duration_pipelined | 267.9 us | [254.4, 324.4] |  | 5 | 50 queued launches / count; class sync-bound |
| kernel.reduceSum | 1048576 | cpu | bytes_moved | 4194304 bytes | [4194304, 4194304] |  | 1 | ideal traffic: every element read/written once |
| kernel.reduceSum | 1048576 | cpu | achieved_bandwidth | 15.66 GB/s | [12.93, 16.48] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.reduceSum | 1048576 | cpu | achieved_rate | 3.91 GFLOP/s | [3.23, 4.12] |  | 5 | flops / duration_pipelined; band from the duration band |
| kernel.reduceSum | 1048576 | cpu | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.reduceSum | 16777216 | cpu | duration_isolated | 4582.4 us | [4512.8, 4625.4] | 5018.0 | 5 | launch+sync per sample; class sync-bound |
| kernel.reduceSum | 16777216 | cpu | duration_pipelined | 4482.7 us | [4458.3, 4595.7] |  | 5 | 50 queued launches / count; class sync-bound |
| kernel.reduceSum | 16777216 | cpu | bytes_moved | 67108864 bytes | [67108864, 67108864] |  | 1 | ideal traffic: every element read/written once |
| kernel.reduceSum | 16777216 | cpu | achieved_bandwidth | 14.97 GB/s | [14.6, 15.05] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.reduceSum | 16777216 | cpu | achieved_rate | 3.74 GFLOP/s | [3.65, 3.76] |  | 5 | flops / duration_pipelined; band from the duration band |
| kernel.reduceSum | 16777216 | cpu | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.matmulTiled | 512^2 | cpu | duration_isolated | 4915.7 us | [4897.0, 4956.6] | 5458.7 | 5 | launch+sync per sample; class compute-bound |
| kernel.matmulTiled | 512^2 | cpu | duration_pipelined | 4756.9 us | [4476.0, 5267.3] |  | 5 | 50 queued launches / count; class compute-bound |
| kernel.matmulTiled | 512^2 | cpu | bytes_moved | 3145728 bytes | [3145728, 3145728] |  | 1 | ideal traffic: every element read/written once |
| kernel.matmulTiled | 512^2 | cpu | achieved_bandwidth | 0.66 GB/s | [0.6, 0.7] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.matmulTiled | 512^2 | cpu | achieved_rate | 56.43 GFLOP/s | [50.96, 59.97] |  | 5 | flops / duration_pipelined; band from the duration band |
| kernel.matmulTiled | 512^2 | cpu | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.matmulTiled | 1024^2 | cpu | duration_isolated | 33616.6 us | [29452.0, 35358.9] | 37293.8 | 5 | launch+sync per sample; class compute-bound |
| kernel.matmulTiled | 1024^2 | cpu | duration_pipelined | 33341.0 us | [31383.4, 33368.9] |  | 5 | 50 queued launches / count; class compute-bound |
| kernel.matmulTiled | 1024^2 | cpu | bytes_moved | 12582912 bytes | [12582912, 12582912] |  | 1 | ideal traffic: every element read/written once |
| kernel.matmulTiled | 1024^2 | cpu | achieved_bandwidth | 0.38 GB/s | [0.38, 0.4] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.matmulTiled | 1024^2 | cpu | achieved_rate | 64.41 GFLOP/s | [64.36, 68.43] |  | 5 | flops / duration_pipelined; band from the duration band |
| kernel.matmulTiled | 1024^2 | cpu | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.wmmaGemm | 512^2 | cpu | duration_isolated | 2793.2 us | [2170.7, 2814.6] | 2819.3 | 5 | launch+sync per sample; class matrix-core |
| kernel.wmmaGemm | 512^2 | cpu | duration_pipelined | 2697.6 us | [2576.0, 2805.5] |  | 5 | 50 queued launches / count; class matrix-core |
| kernel.wmmaGemm | 512^2 | cpu | bytes_moved | 2097152 bytes | [2097152, 2097152] |  | 1 | ideal traffic: every element read/written once |
| kernel.wmmaGemm | 512^2 | cpu | achieved_bandwidth | 0.78 GB/s | [0.75, 0.81] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.wmmaGemm | 512^2 | cpu | achieved_rate | 99.51 GFLOP/s | [95.68, 104.2] |  | 5 | flops / duration_pipelined; band from the duration band |
| kernel.wmmaGemm | 512^2 | cpu | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.wmmaGemm | 1024^2 | cpu | duration_isolated | 18395.5 us | [16577.2, 19718.5] | 20444.4 | 5 | launch+sync per sample; class matrix-core |
| kernel.wmmaGemm | 1024^2 | cpu | duration_pipelined | 18530.0 us | [17851.8, 20088.5] |  | 5 | 50 queued launches / count; class matrix-core |
| kernel.wmmaGemm | 1024^2 | cpu | bytes_moved | 8388608 bytes | [8388608, 8388608] |  | 1 | ideal traffic: every element read/written once |
| kernel.wmmaGemm | 1024^2 | cpu | achieved_bandwidth | 0.45 GB/s | [0.42, 0.47] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.wmmaGemm | 1024^2 | cpu | achieved_rate | 115.9 GFLOP/s | [106.9, 120.3] |  | 5 | flops / duration_pipelined; band from the duration band |
| kernel.wmmaGemm | 1024^2 | cpu | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.gather | 1048576 | cpu | duration_isolated | 73.48 us | [72.22, 100.0] | 122.7 | 5 | launch+sync per sample; class indirect |
| kernel.gather | 1048576 | cpu | duration_pipelined | 81.51 us | [76.43, 82.61] |  | 5 | 50 queued launches / count; class indirect |
| kernel.gather | 1048576 | cpu | bytes_moved | 12582912 bytes | [12582912, 12582912] |  | 1 | ideal traffic: every element read/written once |
| kernel.gather | 1048576 | cpu | achieved_bandwidth | 154.4 GB/s | [152.3, 164.6] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.gather | 1048576 | cpu | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| kernel.gather | 16777216 | cpu | duration_isolated | 7184.6 us | [7100.9, 7292.2] | 8005.1 | 5 | launch+sync per sample; class indirect |
| kernel.gather | 16777216 | cpu | duration_pipelined | 7053.9 us | [6987.1, 7110.9] |  | 5 | 50 queued launches / count; class indirect |
| kernel.gather | 16777216 | cpu | bytes_moved | 201326592 bytes | [201326592, 201326592] |  | 1 | ideal traffic: every element read/written once |
| kernel.gather | 16777216 | cpu | achieved_bandwidth | 28.54 GB/s | [28.31, 28.81] |  | 5 | bytes_moved / duration_pipelined; band from the duration band |
| kernel.gather | 16777216 | cpu | bandwidth_fraction | pending | | | | needs the device's measured achievable bandwidth (report §2, scheduling Unit 2) |
| cg | 1024x1024x2000 | cpu | iterations_per_second | 2032.0 iter/s | [1969.6, 2103.0] |  | 5 | 7 launches/iteration (stencil + 2 dot partials + 1 final + 3 axpy), 400 iterations per block, one sync per block |
| cg | 1024x1024x2000 | cpu | host_cost_per_node | 70.283 us | [67.907, 72.508] |  | 5 | host wall time inside the launch calls / launches |
| cg | 1024x1024x2000 | cpu | wall_per_iteration | 492.1 us | [475.5, 507.7] |  | 5 | 1e6 / iterations_per_second; band from its band |
| degenerate | 1x1x2000 | cpu | iterations_per_second | 90369.6 iter/s | [84182.7, 91637.9] |  | 5 | 7 launches/iteration (stencil + 2 dot partials + 1 final + 3 axpy), 400 iterations per block, one sync per block |
| degenerate | 1x1x2000 | cpu | host_cost_per_node | 1.578 us | [1.556, 1.694] |  | 5 | host wall time inside the launch calls / launches |
| degenerate | 1x1x2000 | cpu | wall_per_iteration | 11.07 us | [10.91, 11.88] |  | 5 | 1e6 / iterations_per_second; band from its band |
| frame | 8388608x12@16.667ms | cpu | frame_p50 | 3.245 ms | [2.045, 5.806] | 5.207 | 600 | 12 dependent chainStep launches + one sync per frame; band is min/max over frames |
| frame | 8388608x12@16.667ms | cpu | frame_p99 | 5.207 ms | [5.207, 5.207] |  | 1 | nearest-rank over frames |
| frame | 8388608x12@16.667ms | cpu | missed_frames_per_10000 | 0 frames | [0, 0] |  | 1 | 0 of 600 frames exceeded the period |
| frame | 8388608x12@16.667ms | cpu | sync_points_per_frame | 1 count | [1, 1] |  | 1 | one host sync per frame; the 11 intra-frame dependencies ride stream order |
| pair | 8388608x12@16.667ms | cpu | frame_p50 | 53.172 ms | [49.343, 59.771] | 58.486 | 600 | 12 dependent chainStep launches + one sync per frame; band is min/max over frames |
| pair | 8388608x12@16.667ms | cpu | frame_p99 | 58.486 ms | [58.486, 58.486] |  | 1 | nearest-rank over frames |
| pair | 8388608x12@16.667ms | cpu | missed_frames_per_10000 | 10000.0 frames | [10000.0, 10000.0] |  | 1 | 600 of 600 frames exceeded the period |
| pair | 8388608x12@16.667ms | cpu | sync_points_per_frame | 1 count | [1, 1] |  | 1 | one host sync per frame; the 11 intra-frame dependencies ride stream order |
| pair | 8388608x12@16.667ms | cpu | besteffort_throughput | 180.9 GB/s | [180.9, 180.9] |  | 1 | 28800 saxpy(16M) launches on the second stream, batches of 24 refilled on completion |
| pair | 8388608x12@16.667ms | cpu | protected_p99_slowdown | 1023.3 % | [1023.3, 1023.3] |  | 1 | frame p99 co-run vs solo 5.207 ms |
| pair | 8388608x12@16.667ms | cpu | besteffort_pct_of_solo | 95.4 % | [95.4, 95.4] |  | 1 | vs solo pipelined saxpy(16M) 189.71 GB/s |
| pair | 8388608x12@16.667ms | cpu | goodput | 47.7 % | [47.7, 47.7] |  | 1 | mean of protected on-time % and best-effort % of solo |
| seam | 5us | cpu | kernel_time | 6.43 us | [6.43, 6.43] |  | 1 | spin(4187) pipelined x100; calibrated from 1.194 ns/iter |
| seam | 5us | cpu | launch_call | 6.272 us | [6.231, 6.382] | 6.402 | 5 | host time inside the launch statement |
| seam | 5us | cpu | pipelined_overhead | -0.096 us | [-0.115, 0.021] |  | 5 | (100 queued launches + sync)/100 minus kernel_time |
| seam | 5us | cpu | isolated_overhead | -0.126 us | [-0.166, 0.075] |  | 5 | launch + sync minus kernel_time |
| seam | 50us | cpu | kernel_time | 51.05 us | [51.05, 51.05] |  | 1 | spin(42450) pipelined x100; calibrated from 1.178 ns/iter |
| seam | 50us | cpu | launch_call | 50.835 us | [50.776, 50.916] | 51.187 | 5 | host time inside the launch statement |
| seam | 50us | cpu | pipelined_overhead | -0.108 us | [-0.137, -0.016] |  | 5 | (100 queued launches + sync)/100 minus kernel_time |
| seam | 50us | cpu | isolated_overhead | -0.189 us | [-0.248, -0.108] |  | 5 | launch + sync minus kernel_time |
| seam | 200us | cpu | kernel_time | 200.6 us | [200.6, 200.6] |  | 1 | spin(169962) pipelined x100; calibrated from 1.177 ns/iter |
| seam | 200us | cpu | launch_call | 200.1 us | [200.0, 200.1] | 203.2 | 5 | host time inside the launch statement |
| seam | 200us | cpu | pipelined_overhead | 0.134 us | [-0.046, 0.292] |  | 5 | (100 queued launches + sync)/100 minus kernel_time |
| seam | 200us | cpu | isolated_overhead | -0.444 us | [-0.554, -0.394] |  | 5 | launch + sync minus kernel_time |
| cg | 1024x1024x2000 | cpu | device_span | 507.0 us | [507.0, 507.0] |  | 1 | device tier, avg span x launches per iteration (stencil5,dot,dot,finalSum2,saxpy,saxpy,saxpy), 2001 iterations profiled |
| cg | 1024x1024x2000 | cpu | device_time_per_iteration | 507.0 us | [507.0, 507.0] |  | 1 | device tier, avg span x launches per iteration (stencil5,dot,dot,finalSum2,saxpy,saxpy,saxpy), 2001 iterations profiled |
| cg | 1024x1024x2000 | cpu | queue_empty_time | 0.5 % | [0.5, 0.5] |  | 1 | 100 x (1 - device_time_per_iteration / wall_per_iteration 509.3 us) in the profiled pass |
| degenerate | 1x1x2000 | cpu | device_span | 11.22 us | [11.22, 11.22] |  | 1 | device tier, avg span x launches per iteration (stencil5,dot,dot,finalSum2,saxpy,saxpy,saxpy), 2001 iterations profiled |
| degenerate | 1x1x2000 | cpu | device_time_per_iteration | 11.22 us | [11.22, 11.22] |  | 1 | device tier, avg span x launches per iteration (stencil5,dot,dot,finalSum2,saxpy,saxpy,saxpy), 2001 iterations profiled |
| degenerate | 1x1x2000 | cpu | queue_empty_time | 16.7 % | [16.7, 16.7] |  | 1 | 100 x (1 - device_time_per_iteration / wall_per_iteration 13.5 us) in the profiled pass |
| seam | 5us | cpu | device_span | 7.51 us | [7.51, 7.51] |  | 1 | device tier, avg span x launches per iteration (spin), 500 iterations profiled |
| seam | 50us | cpu | device_span | 53.4 us | [53.4, 53.4] |  | 1 | device tier, avg span x launches per iteration (spin), 500 iterations profiled |
| seam | 200us | cpu | device_span | 190.7 us | [190.7, 190.7] |  | 1 | device tier, avg span x launches per iteration (spin), 500 iterations profiled |
| kernel.saxpy | 1048576 | cpu | device_span | 67.06 us | [67.06, 67.06] |  | 1 | device tier, avg span x launches per iteration (saxpy), 111 iterations profiled |
| kernel.dot | 1048576 | cpu | device_span | 91.16 us | [91.16, 91.16] |  | 1 | device tier, avg span x launches per iteration (dot,finalSum2), 111 iterations profiled |
| kernel.stencil5 | 1024x1024 | cpu | device_span | 135.2 us | [135.2, 135.2] |  | 1 | device tier, avg span x launches per iteration (stencil5), 111 iterations profiled |
| kernel.reduceSum | 1048576 | cpu | device_span | 260.0 us | [260.0, 260.0] |  | 1 | device tier, avg span x launches per iteration (reduceSum,finalSum2), 111 iterations profiled |
| kernel.matmulTiled | 512^2 | cpu | device_span | 4738.9 us | [4738.9, 4738.9] |  | 1 | device tier, avg span x launches per iteration (matmulTiled), 17 iterations profiled |
| kernel.wmmaGemm | 512^2 | cpu | device_span | 2414.1 us | [2414.1, 2414.1] |  | 1 | device tier, avg span x launches per iteration (wmmaGemm), 17 iterations profiled |
| kernel.gather | 1048576 | cpu | device_span | 86.16 us | [86.16, 86.16] |  | 1 | device tier, avg span x launches per iteration (gather), 111 iterations profiled |
| kernel.saxpy | 16777216 | cpu | device_span | 1092.3 us | [1092.3, 1092.3] |  | 1 | device tier, avg span x launches per iteration (saxpy), 111 iterations profiled |
| kernel.dot | 16777216 | cpu | device_span | 2200.9 us | [2200.9, 2200.9] |  | 1 | device tier, avg span x launches per iteration (dot,finalSum2), 111 iterations profiled |
| kernel.stencil5 | 4096x4096 | cpu | device_span | 2208.9 us | [2208.9, 2208.9] |  | 1 | device tier, avg span x launches per iteration (stencil5), 111 iterations profiled |
| kernel.reduceSum | 16777216 | cpu | device_span | 4694.3 us | [4694.3, 4694.3] |  | 1 | device tier, avg span x launches per iteration (reduceSum,finalSum2), 111 iterations profiled |
| kernel.matmulTiled | 1024^2 | cpu | device_span | 38293.4 us | [38293.4, 38293.4] |  | 1 | device tier, avg span x launches per iteration (matmulTiled), 17 iterations profiled |
| kernel.wmmaGemm | 1024^2 | cpu | device_span | 19339.1 us | [19339.1, 19339.1] |  | 1 | device tier, avg span x launches per iteration (wmmaGemm), 17 iterations profiled |
| kernel.gather | 16777216 | cpu | device_span | 7009.0 us | [7009.0, 7009.0] |  | 1 | device tier, avg span x launches per iteration (gather), 111 iterations profiled |

### 3.4 Device tier on the seam pass

Since 0.2.4 the seam probe gets one profiled pass per target and its device
span is a row (`seam device_span` in §3.2 and §3.3; the calibration launches
run under their own kernel name, `spinCal`, so they stay out of the target's
average). The first leg's single mixed pass (5, 50 and 200 µs together,
2,133 launches, 100.4 µs average on HIP against an 89.4 µs mean of the
three host-clocked kernel times) is superseded by those rows and by §3.1's
reading of them: 5.9 / 50.6 / 198.8 µs on the device against 9.5 / 52.7 /
207.4 µs pipelined on the host.

### 3.5 Device tier on the cajeta-llm run

One profiled `SchedThroughput` process (prompt 2,048, generate 64, ring
4 M records): 43,874 device spans, 17 kernels, 11,627 ms of device time
against 11,802 ms of prefill + decode wall (prefill 10,145 ms, decode
1,657 ms under the profiler, +3% on prefill over the unprofiled rows).

| Kernel | Launches | Total (ms) | Avg (µs) | Phase |
|---|---|---|---|---|
| q4kWmmaKernel | 3,264 | 7,904.2 | 2,421.6 | prefill, matrix cores |
| q6kWmmaEpiKernel | 544 | 1,470.9 | 2,703.9 | prefill, matrix cores |
| attnFlashPrefillGqa4Kernel | 544 | 498.8 | 916.9 | prefill attention |
| q4kQ8WaveMatVecKernel | 7,168 | 871.8 | 121.6 | decode |
| q6kQ8WaveMatVecKernel | 1,105 | 388.8 | 351.9 | decode |
| qkvWaveMatVecKernel | 2,048 | 137.5 | 67.2 | decode |
| attnFlashDecodeGqa4Kernel | 2,048 | 89.2 | 43.6 | decode attention |
| attnFlashDecodeReduceKernel | 2,048 | 37.3 | 18.2 | decode attention |
| q8kPackKernel | 10,432 | 68.3 | 6.5 | both |
| gluF32 / addF32 / rmsnorm* / ropeF32 / qkPrep / kvAppendRange | 14,656 | 155.7 | 3–18 | both |

Derived (these fill the two llm KPIs the harness marks pending):

| KPI | Value | Derivation |
|---|---|---|
| device busy during the run | 98.5% | 11,627 / 11,802 ms; the remaining 1.5% is launch gaps and host work |
| prefill matrix-core fraction | ~94% | WMMA kernels 9,375 ms of ~9,990 ms prefill device time (total minus the decode-only kernels and the decode share of the shared ones) |
| decode attention kernel duration | 43.6 µs + 18.2 µs reduce | per layer per token, 2,048 launches each (64 tokens x 32 layers) |
| prefill attention kernel duration | 916.9 µs | per layer per 128-token chunk (17 chunks x 32 layers) |
| decode per-token device time | 25.5 ms | decode kernels' totals / 64 tokens; the unprofiled rows measured 24.8 ms wall per token |

The first profiled attempt used the default ring and kept 8,000 of these
spans with no prefill kernel among them — averages survived, totals did
not. Any per-kernel accounting over a long run sets the ring explicitly.

Automated since 0.2.4: `run.sh` runs the bench under the profiler (ring
4 M) and `xpubench-report spans --llm` derives the rows in §3.2 —
`matrix_core_fraction` 92.9% (the lower bound above: kernels both phases
share count as prefill), `device_busy` 98.7%, `attention_kernel_duration`
58.0 µs decode (attention + reduce) and 904.6 µs prefill. The ring's own
accounting, now read by `cajeta profile summary` (`gpu_records_kept`
43,874, `gpu_records_dropped` 0 on this run), is what makes the fractions
trustworthy: a ring that dropped records yields pending fractions and keeps
only the averages.

## 4. Trials

One row per configuration tried, in order. `Before` is the previous accepted
row for the same (workload, device, KPI). Verdict per KPI is `keep`, `worse` or
`single` (§1, from the trial verb); the unit's verdict — `keep`, `revert`,
`gate-off` or `blocked` — is the line under its rows.

| Trial | Date | Plan / unit | Workload | Device | KPI | Before | After | Delta | Noise band | Commit | Verdict | Note |
|---|---|---|---|---|---|---|---|---|---|---|---|---|
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | kernel.saxpy [1048576] | cpu | duration_isolated | 44.71 us | 46.29 us | 1.58 (3.534%) | [43.45, 57.74] | e0fa4871 | keep | launch+sync per sample; class memory-bound |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | kernel.saxpy [1048576] | cpu | duration_pipelined | 44.69 us | 44.18 us | -0.51 (-1.141%) | [43.63, 69.01] | e0fa4871 | keep | 50 queued launches / count; class memory-bound |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | kernel.saxpy [16777216] | cpu | duration_isolated | 979.2 us | 986.8 us | 7.56 (0.772%) | [949.1, 1112.4] | e0fa4871 | keep | launch+sync per sample; class memory-bound |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | kernel.saxpy [16777216] | cpu | duration_pipelined | 1049.0 us | 992.6 us | -56.38 (-5.375%) | [964.8, 1066.5] | e0fa4871 | keep | 50 queued launches / count; class memory-bound |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | kernel.stencil5 [1024x1024] | cpu | duration_isolated | 116.7 us | 149.9 us | 33.29 (28.538%) | [114.4, 159.8] | e0fa4871 | keep | launch+sync per sample; class memory-bound |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | kernel.stencil5 [1024x1024] | cpu | duration_pipelined | 139.2 us | 154.7 us | 15.59 (11.204%) | [124.2, 142.1] | e0fa4871 | worse | 50 queued launches / count; class memory-bound |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | kernel.stencil5 [4096x4096] | cpu | duration_isolated | 2269.1 us | 2307.6 us | 38.49 (1.696%) | [2096.7, 2370.4] | e0fa4871 | keep | launch+sync per sample; class memory-bound |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | kernel.stencil5 [4096x4096] | cpu | duration_pipelined | 2231.8 us | 2243.0 us | 11.19 (0.501%) | [2068.3, 2281.6] | e0fa4871 | keep | 50 queued launches / count; class memory-bound |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | kernel.wmmaGemm [512^2] | cpu | duration_isolated | 2788.6 us | 2820.7 us | 32.1 (1.151%) | [2543.8, 2792.9] | e0fa4871 | worse | launch+sync per sample; class matrix-core |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | kernel.wmmaGemm [512^2] | cpu | duration_pipelined | 2797.8 us | 2679.1 us | -118.7 (-4.241%) | [2350.3, 2989.5] | e0fa4871 | keep | 50 queued launches / count; class matrix-core |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | kernel.wmmaGemm [1024^2] | cpu | duration_isolated | 19551.6 us | 18667.3 us | -884.3 (-4.523%) | [19314.4, 20293.6] | e0fa4871 | keep | launch+sync per sample; class matrix-core |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | kernel.wmmaGemm [1024^2] | cpu | duration_pipelined | 18483.0 us | 18577.0 us | 93.94 (0.508%) | [17635.7, 19392.1] | e0fa4871 | keep | 50 queued launches / count; class matrix-core |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | kernel.gather [1048576] | cpu | duration_isolated | 77.26 us | 73.75 us | -3.51 (-4.543%) | [73.21, 96.53] | e0fa4871 | keep | launch+sync per sample; class indirect |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | kernel.gather [1048576] | cpu | duration_pipelined | 77.76 us | 85.5 us | 7.74 (9.954%) | [74, 97.47] | e0fa4871 | keep | 50 queued launches / count; class indirect |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | kernel.gather [16777216] | cpu | duration_isolated | 7215.4 us | 7102.9 us | -112.6 (-1.56%) | [7124.7, 7314.9] | e0fa4871 | keep | launch+sync per sample; class indirect |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | kernel.gather [16777216] | cpu | duration_pipelined | 7097.0 us | 7074.9 us | -22.07 (-0.311%) | [6844.4, 7255.4] | e0fa4871 | keep | 50 queued launches / count; class indirect |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | seam [5us] | cpu | launch_call | 6.302 us | 6.392 us | 0.09 (1.428%) | [6.282, 6.402] | e0fa4871 | keep | host time inside the launch statement |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | seam [5us] | cpu | pipelined_overhead | -0.087 us | -0.142 us | -0.055 (63.219%) | [-0.145, 0.066] | e0fa4871 | keep | (100 queued launches + sync)/100 minus kernel_time |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | seam [5us] | cpu | isolated_overhead | -0.124 us | -0.144 us | -0.02 (16.129%) | [-0.144, 0.056] | e0fa4871 | keep | launch + sync minus kernel_time |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | seam [50us] | cpu | launch_call | 50.856 us | 50.626 us | -0.23 (-0.452%) | [50.796, 50.966] | e0fa4871 | keep | host time inside the launch statement |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | seam [50us] | cpu | pipelined_overhead | 0.2 us | -0.171 us | -0.371 (-185.5%) | [0.018, 0.263] | e0fa4871 | keep | (100 queued launches + sync)/100 minus kernel_time |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | seam [50us] | cpu | isolated_overhead | -0.07 us | -0.282 us | -0.212 (302.9%) | [-0.13, 0.04] | e0fa4871 | keep | launch + sync minus kernel_time |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | seam [200us] | cpu | launch_call | 199.5 us | 200.0 us | 0.561 (0.281%) | [199.5, 199.5] | e0fa4871 | worse | host time inside the launch statement |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | seam [200us] | cpu | pipelined_overhead | 0.055 us | 0.279 us | 0.224 (407.3%) | [-0.027, 0.267] | e0fa4871 | worse | (100 queued launches + sync)/100 minus kernel_time |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | seam [200us] | cpu | isolated_overhead | -0.553 us | -0.297 us | 0.256 (-46.293%) | [-0.573, -0.483] | e0fa4871 | worse | launch + sync minus kernel_time |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | frame [8388608x12@16.667ms] | cpu | frame_p50 | 3.339 ms | 3.257 ms | -0.082 (-2.456%) | [1.986, 7.233] | e0fa4871 | keep | 12 dependent chainStep launches + one sync per frame; band is min/max over frames |
| T-001 | 2026-09-06T19:02:13Z | cpu-barrier-fission-loops:1 | pair [8388608x12@16.667ms] | cpu | frame_p50 | 53.318 ms | 53.177 ms | -0.141 (-0.264%) | [48.956, 60.047] | e0fa4871 | keep | 12 dependent chainStep launches + one sync per frame; band is min/max over frames |

T-001 — `cpu-barrier-fission-loops` Unit 1 on the CPU leg (before: the first
leg, `8fea9b63`; after: the rerun on `e0fa4871`). Unit verdict: **keep**.
22 keep, 5 `worse`, 35 `single` (the single-sample rows are omitted above;
§1). The five: `stencil5` 1024² pipelined +11%, `wmmaGemm` 512² isolated
+1.2%, and three seam rows at 200 µs that are sub-microsecond differences of
200 µs measurements — none on a path the fission touches. Control T-002, two
runs of the same code minutes apart (`rows-cpu-20260906-1449-full` vs
`-1502-full`): 34 keep, 9 `worse`, 55 `single`, with `dot` 1M isolated +31%
and `stencil5` 1024² pipelined +18% — the CPU backend's run-to-run drift is
wider than one run's five-block band, so a CPU-leg `worse` is not readable
until 0.3.2 and 0.3.3 set bands from repeated runs (§5).

T-003 — control: the gfx1151 rerun on `f721f0bf` with no device code
changed (before `rows-hip-20260906-1810`, after `-1532`): 43 keep, 4
`worse`, 55 `single` — `matmulTiled` 2048² isolated +0.8% and pipelined
+1.2% against bands 0.3–1.7% wide, the degenerate loop's host cost per node
+7% (0.887 → 0.953 µs), seam 50 µs isolated overhead +4%. The same reading
as T-002: drift, and the band source is 0.3.3.

## 5. Residuals

Changes that measured worse or flat and shipped gated off, with the row that
decided it and what would reopen it.

| Residual | Trial | Why | Reopen when |
|---|---|---|---|
| CLOSED 2026-09-06 — CPU backend: `dot`, `reduceSum`, `matmulTiled`, `cg`, `degenerate` were pending | baseline → T-001 | The CPU barrier fission declined a uniform loop whose code after its last barrier was the latch block (`CpuBarrierFission.cpp` started a region at the latch and walked around the loop: "unstructured barrier control flow"). Fixed in `cpu-barrier-fission-loops` Unit 1 (cajeta `e0fa4871`; 7 tests, a per-work-item latch is now declined by name); the CPU leg reran the same day: 112 rows, 14 pending, all of them `bandwidth_fraction` (§3.3). Unit 0's two silences (the skip note, the failure count) stay in place | closed |
| CPU-leg verdicts: one run's band understates run-to-run drift | T-001, T-002 | Two runs of identical code minutes apart (T-002) flagged 9 of 43 banded KPIs — `dot` 1M isolated +31%, `stencil5` 1024² pipelined +18%, seam rows at 200 µs by sub-microsecond amounts; the fission trial (T-001) flagged 5 of the same kind on paths it never touched, and the gfx1151 rerun (T-003, no device code changed) 4 of 47. A five-block band from one run is a few percent wide on a 32-core host at `auto` power; drift between runs is not | scheduling 0.3.2 (the day-apart pair) with 0.3.3 (`trial --bands`: per KPI the wider of the within-run band and the day-apart spread); until then a CPU-leg `worse` is reported, not gating |
| `bandwidth_fraction` pending on every kernel | baseline | needs the device's measured achievable bandwidth | scheduling Unit 2 fills §2 |
| CLOSED 2026-09-06 — `matrix_core_fraction`, `attention_kernel_duration` were not produced by the harness | baseline → 0.2.4 | `run.sh` profiles the llm bench and `xpubench-report spans --llm` derives them as rows: 92.9% (a lower bound, shared kernels counted as prefill), 58.0 µs decode and 904.6 µs prefill attention, plus `device_busy` 98.7%; the ring's own accounting decides whether the totals-based rows are trusted (§3.5) | closed |
| `per_token_p99` pending | baseline | `SchedThroughput` prints a per-run mean, not per-token latencies | cajeta-llm's bench emits per-token timings (profiles plan Unit 1 needs it) |
| CLOSED 2026-09-06 — seam device-tier figure (§3.4) was a three-duration mix | baseline → 0.2.4 | one profiled pass per target, the calibration launches under their own kernel name: `seam device_span` 5.9 / 50.6 / 198.8 µs on gfx1151, 2–9 µs under the host-clocked pipelined time (§3.1) | closed |
| Frame p50 band 3.5–6.6 ms solo | baseline | run-to-run jitter of the unscheduled frame; p99 and the miss count are the frame KPIs that verdicts read | a scheduler unit that claims to reduce jitter measures p50 with N runs, not one |
| CLOSED 2026-09-06 — CG `queue-empty time` was not measured | baseline → 0.2.4 | a profiled CG pass: `queue_empty_time` 15.4% on gfx1151 (92.6 µs of device time in a 98.0 µs iteration), 74.5% for the degenerate loop; 0.5% and 16.7% on the CPU backend, where a launch is the kernel (§3.1) | closed |

## 6. Closing summary (after the last unit)

Per workload and device: baseline row, closing row, better / same / worse
with the noise band. Nothing may read `worse`.

| Workload | Device | KPI | Baseline | Closing | Verdict |
|---|---|---|---|---|---|
| | | | | | |
