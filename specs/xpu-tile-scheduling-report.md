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
| Instrument | Host clock (`Clock.nanoTime`) around launch and sync, in two modes: **isolated** (one launch, one sync — kernel plus launch plus sync latency) and **pipelined** (fifty queued launches, one sync, divided by fifty — the per-kernel cost a full queue sees), so a row means the same thing on every backend. The profiler's AMD device tier (rocprofiler-sdk, bound from `$ROCM_PATH/lib` — this box's user-local ROCm 7.11.0 tree carries it; proven by a loader trace, since the runtime prints nothing when it binds) records device spans and is the cross-check: `run.sh` profiles the seam pass with it (§3.4) and the cajeta-llm run was profiled with it (§3.5). `CAJETA_PROFILER_GPU_RING=4194304` for runs with tens of thousands of launches; the sink drops on overflow and a small ring keeps averages but loses totals. CUPTI on NVIDIA when that session runs. |
| Warm-up | first-touch pages committed and every kernel run once per shape before timing (the first-touch finding: ~890 ms one-time on fresh buffers) |
| Runs | 5 blocks per KPI; a block's median is one sample of the noise band (min/max over blocks); p95 is nearest-rank over every individual sample (20 per block for kernels, 100 for the seam probe, every frame for the frame stand-in); cajeta-llm: 5 processes, one run each |
| Idle gate | no other `cajeta_test`, `xpubench`, `gpuparity`, `SchedThroughput`, or llama.cpp binary alive, matched by process NAME (`pgrep -a`) excluding the harness itself; exit 3 and no rows otherwise; load average printed beside it. Tested both ways in `XpuBenchHarnessTests` |
| Arm order | alternated A/B/B/A across runs (a fixed order let a decaying load fake a speedup once); `Stats.abbaOrder`, tested |
| Storage | harness rows under repo `tmp/bench/` (never `/tmp`), one JSON object per line per (workload, shape, KPI), each carrying device / backend / driver / compiler / power mode / date / arm; tables rendered by `tools/xpubench-report` (`baseline` and `trial` verbs) |
| Harness | `test/xpu/bench/run.sh` (`all`, `hip`, `cpu`, `smoke`); sources `test/xpu/bench/src/xpubench/`; the workload table is in its README |
| Verdict rule | "not worse" = every KPI within its noise band of the previous accepted row; a regression blocks the unit or ships gated off with the residual in §5. Frame stand-in: verdicts read `frame_p99` and `missed_frames_per_10000`; `frame_p50` is informational (developer, 2026-09-06: power mode stays `auto`, what a game gets on this part; a unit claiming a jitter reduction shows it over several runs) |
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

Filled by scheduling Unit 0 on 2026-09-06, commit `8fea9b63` (the harness
and the two compiler/runtime fixes it needed are the commit after). Rows:
`tmp/bench/rows-{hip,cpu}-20260906-1810.jsonl` (gitignored; the tables below
are the rendered copy). One row per (workload, shape, KPI); the noise band is
min/max over five blocks (over frames for the frame stand-in); `pending`
rows name what could not be measured here and why. Rows for `bytes_moved`
and `bandwidth_fraction` (pending on every kernel until §2 has the device's
achievable bandwidth) are in the JSON and omitted from the tables.

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
- **cajeta-llm** on the same compiler: 2,048-token prefill 9.82 s (208.5
  tok/s), decode 40.4 tok/s (24.8 ms per token), five processes agreeing
  within 0.3%.
- **Two kernels were fixed before the baseline was taken.** The first
  reductions used a same-address float atomic and were contention-bound on
  HIP (446 µs for 1M elements, 2.3 GFLOP/s, a bimodal band at 16M); the
  baseline carries the two-stage form (per-block partials, one final
  launch), which took the CG stand-in from 1,190 to 10,141 iterations/s.
  That change is in the kernels, not in any scheduler.
- **The CPU backend declines three kernels** (`reduceSum`, `finalSum2`,
  `matmulTiled`: a barrier inside a loop), so `dot`, `reduceSum`,
  `matmulTiled`, `cg` and `degenerate` are pending on that leg — §5.
- **Frame p50 is noisier than p99 suggests**: 4.18 ms median with a
  3.5–6.6 ms band over 600 frames, unscheduled and solo. Later units read
  the p99 and the miss count, not the p50, for the frame stand-in.

### 3.2 gfx1151 (HIP)

Identity: AMD RYZEN AI MAX+ 395 w/ Radeon 8060S | hip | linux 7.0.0-30-generic, ROCm 7.2.53150-7b886380f9 | 8fea9b63 | auto | 2026-09-06T17:09:24Z

| Workload | Shape | Device | KPI | Median | Noise band | p95 | n | Note |
|---|---|---|---|---|---|---|---|---|
| kernel.saxpy | 1048576 | hip | duration_isolated | 19.97 us | [19.93, 21.04] | 77.67 | 5 | launch+sync per sample; class memory-bound |
| kernel.saxpy | 1048576 | hip | duration_pipelined | 15.08 us | [15.06, 22.55] |  | 5 | 50 queued launches / count; class memory-bound |
| kernel.saxpy | 1048576 | hip | achieved_rate | 139.1 GFLOP/s | [139.1, 139.1] |  | 1 | flops / duration_pipelined |
| kernel.saxpy | 16777216 | hip | duration_isolated | 917.0 us | [914.2, 923.4] | 933.9 | 5 | launch+sync per sample; class memory-bound |
| kernel.saxpy | 16777216 | hip | duration_pipelined | 918.9 us | [917.7, 930.8] |  | 5 | 50 queued launches / count; class memory-bound |
| kernel.saxpy | 16777216 | hip | achieved_rate | 36.51 GFLOP/s | [36.51, 36.51] |  | 1 | flops / duration_pipelined |
| kernel.dot | 1048576 | hip | duration_isolated | 25.63 us | [25.45, 93.73] | 112.2 | 5 | launch+sync per sample; class memory-bound+reduce |
| kernel.dot | 1048576 | hip | duration_pipelined | 20.68 us | [20.49, 35.76] |  | 5 | 50 queued launches / count; class memory-bound+reduce |
| kernel.dot | 1048576 | hip | achieved_rate | 101.4 GFLOP/s | [101.4, 101.4] |  | 1 | flops / duration_pipelined |
| kernel.dot | 16777216 | hip | duration_isolated | 659.8 us | [658.2, 673.2] | 718.2 | 5 | launch+sync per sample; class memory-bound+reduce |
| kernel.dot | 16777216 | hip | duration_pipelined | 656.9 us | [652.3, 663.1] |  | 5 | 50 queued launches / count; class memory-bound+reduce |
| kernel.dot | 16777216 | hip | achieved_rate | 51.08 GFLOP/s | [51.08, 51.08] |  | 1 | flops / duration_pipelined |
| kernel.stencil5 | 1024x1024 | hip | duration_isolated | 21.93 us | [21.8, 88.21] | 106.4 | 5 | launch+sync per sample; class memory-bound |
| kernel.stencil5 | 1024x1024 | hip | duration_pipelined | 16.66 us | [16.62, 16.79] |  | 5 | 50 queued launches / count; class memory-bound |
| kernel.stencil5 | 1024x1024 | hip | achieved_rate | 503.6 GFLOP/s | [503.6, 503.6] |  | 1 | flops / duration_pipelined |
| kernel.stencil5 | 4096x4096 | hip | duration_isolated | 604.8 us | [604.2, 626.2] | 638.8 | 5 | launch+sync per sample; class memory-bound |
| kernel.stencil5 | 4096x4096 | hip | duration_pipelined | 605.4 us | [599.1, 615.9] |  | 5 | 50 queued launches / count; class memory-bound |
| kernel.stencil5 | 4096x4096 | hip | achieved_rate | 221.7 GFLOP/s | [221.7, 221.7] |  | 1 | flops / duration_pipelined |
| kernel.reduceSum | 1048576 | hip | duration_isolated | 37.19 us | [37.04, 38.04] | 51.01 | 5 | launch+sync per sample; class sync-bound |
| kernel.reduceSum | 1048576 | hip | duration_pipelined | 26.45 us | [26.34, 26.63] |  | 5 | 50 queued launches / count; class sync-bound |
| kernel.reduceSum | 1048576 | hip | achieved_rate | 39.64 GFLOP/s | [39.64, 39.64] |  | 1 | flops / duration_pipelined |
| kernel.reduceSum | 16777216 | hip | duration_isolated | 555.0 us | [552.8, 556.0] | 574.8 | 5 | launch+sync per sample; class sync-bound |
| kernel.reduceSum | 16777216 | hip | duration_pipelined | 551.2 us | [547.1, 567.6] |  | 5 | 50 queued launches / count; class sync-bound |
| kernel.reduceSum | 16777216 | hip | achieved_rate | 30.44 GFLOP/s | [30.44, 30.44] |  | 1 | flops / duration_pipelined |
| kernel.matmulTiled | 512^2 | hip | duration_isolated | 179.6 us | [175.5, 195.7] | 376.3 | 5 | launch+sync per sample; class compute-bound |
| kernel.matmulTiled | 512^2 | hip | duration_pipelined | 161.4 us | [160.5, 167.7] |  | 5 | 50 queued launches / count; class compute-bound |
| kernel.matmulTiled | 512^2 | hip | achieved_rate | 1663.6 GFLOP/s | [1663.6, 1663.6] |  | 1 | flops / duration_pipelined |
| kernel.matmulTiled | 2048^2 | hip | duration_isolated | 10234.0 us | [10213.7, 10241.8] | 10330.2 | 5 | launch+sync per sample; class compute-bound |
| kernel.matmulTiled | 2048^2 | hip | duration_pipelined | 10320.0 us | [10207.0, 10381.3] |  | 5 | 50 queued launches / count; class compute-bound |
| kernel.matmulTiled | 2048^2 | hip | achieved_rate | 1664.7 GFLOP/s | [1664.7, 1664.7] |  | 1 | flops / duration_pipelined |
| kernel.wmmaGemm | 512^2 | hip | duration_isolated | 78.46 us | [65.21, 79.67] | 144.6 | 5 | launch+sync per sample; class matrix-core |
| kernel.wmmaGemm | 512^2 | hip | duration_pipelined | 61.48 us | [60.76, 63.5] |  | 5 | 50 queued launches / count; class matrix-core |
| kernel.wmmaGemm | 512^2 | hip | achieved_rate | 4366.5 GFLOP/s | [4366.5, 4366.5] |  | 1 | flops / duration_pipelined |
| kernel.wmmaGemm | 2048^2 | hip | duration_isolated | 3140.2 us | [3103.6, 3171.2] | 3252.1 | 5 | launch+sync per sample; class matrix-core |
| kernel.wmmaGemm | 2048^2 | hip | duration_pipelined | 3180.7 us | [3138.4, 3252.8] |  | 5 | 50 queued launches / count; class matrix-core |
| kernel.wmmaGemm | 2048^2 | hip | achieved_rate | 5401.3 GFLOP/s | [5401.3, 5401.3] |  | 1 | flops / duration_pipelined |
| kernel.gather | 1048576 | hip | duration_isolated | 107.9 us | [103.8, 108.1] | 110.3 | 5 | launch+sync per sample; class indirect |
| kernel.gather | 1048576 | hip | duration_pipelined | 87.8 us | [87.5, 93.39] |  | 5 | 50 queued launches / count; class indirect |
| kernel.gather | 16777216 | hip | duration_isolated | 9978.6 us | [9976.9, 9989.2] | 10187.9 | 5 | launch+sync per sample; class indirect |
| kernel.gather | 16777216 | hip | duration_pipelined | 9999.5 us | [9987.2, 10008.2] |  | 5 | 50 queued launches / count; class indirect |
| cg | 1024x1024x10000 | hip | iterations_per_second | 10141.3 iter/s | [9919.4, 10208.1] |  | 5 | 7 launches/iteration (stencil + 2 dot partials + 1 final + 3 axpy), 2000 iterations per block, one sync per block |
| cg | 1024x1024x10000 | hip | host_cost_per_node | 0.829 us | [0.826, 0.954] |  | 5 | host wall time inside the launch calls / launches |
| cg | 1024x1024x10000 | hip | wall_per_iteration | 98.61 us | [98.61, 98.61] |  | 1 | 1e6 / iterations_per_second |
| degenerate | 1x1x10000 | hip | iterations_per_second | 67606.5 iter/s | [63625.4, 69171.5] |  | 5 | 7 launches/iteration (stencil + 2 dot partials + 1 final + 3 axpy), 2000 iterations per block, one sync per block |
| degenerate | 1x1x10000 | hip | host_cost_per_node | 0.887 us | [0.878, 0.945] |  | 5 | host wall time inside the launch calls / launches |
| degenerate | 1x1x10000 | hip | wall_per_iteration | 14.79 us | [14.79, 14.79] |  | 1 | 1e6 / iterations_per_second |
| seam | 5us | hip | kernel_time | 7.35 us | [7.35, 7.35] |  | 1 | spin(1002) pipelined x100; calibrated from 4.988 ns/iter |
| seam | 5us | hip | launch_call | 0.871 us | [0.791, 0.872] | 0.972 | 5 | host time inside the launch statement |
| seam | 5us | hip | pipelined_overhead | -0.23 us | [-0.232, -0.215] |  | 5 | (100 queued launches + sync)/100 minus kernel_time |
| seam | 5us | hip | isolated_overhead | 5.305 us | [5.154, 5.345] |  | 5 | launch + sync minus kernel_time |
| seam | 50us | hip | kernel_time | 53.56 us | [53.56, 53.56] |  | 1 | spin(10008) pipelined x100; calibrated from 4.996 ns/iter |
| seam | 50us | hip | launch_call | 14.818 us | [14.808, 14.828] | 14.928 | 5 | host time inside the launch statement |
| seam | 50us | hip | pipelined_overhead | 0.704 us | [-0.631, 1.232] |  | 5 | (100 queued launches + sync)/100 minus kernel_time |
| seam | 50us | hip | isolated_overhead | 18.297 us | [18.257, 18.337] |  | 5 | launch + sync minus kernel_time |
| seam | 200us | hip | kernel_time | 207.3 us | [207.3, 207.3] |  | 1 | spin(40066) pipelined x100; calibrated from 4.992 ns/iter |
| seam | 200us | hip | launch_call | 11.732 us | [10.35, 14.818] | 15.339 | 5 | host time inside the launch statement |
| seam | 200us | hip | pipelined_overhead | 1.116 us | [0.119, 2.517] |  | 5 | (100 queued launches + sync)/100 minus kernel_time |
| seam | 200us | hip | isolated_overhead | 13.976 us | [13.705, 18.013] |  | 5 | launch + sync minus kernel_time |
| llm.prefill | prompt2048+gen64 | hip | prefill_ms | 9824.8 ms | [9819.8, 9844.3] |  | 5 | SchedThroughput, one process per run, chunked at the engine's default |
| llm.prefill | prompt2048+gen64 | hip | prefill_tokens_per_second | 208.5 tok/s | [208.0, 208.6] |  | 5 | from the same runs |
| llm.prefill | prompt2048+gen64 | hip | matrix_core_fraction | pending | | | | needs the device tier (rocprofiler-sdk) to attribute prefill time to WMMA kernels |
| llm.decode | prompt2048+gen64 | hip | tokens_per_second | 40.37 tok/s | [40.36, 40.45] |  | 5 | batch 1, greedy, 64 generated tokens after the prompt |
| llm.decode | prompt2048+gen64 | hip | ms_per_token | 24.77 ms | [24.72, 24.78] |  | 5 | mean per token per run |
| llm.decode | prompt2048+gen64 | hip | per_token_p99 | pending | | | | SchedThroughput prints a per-run mean, not per-token latencies |
| llm.decode | prompt2048+gen64 | hip | attention_kernel_duration | pending | | | | needs the device tier (rocprofiler-sdk) for per-kernel device spans |
| frame | 8388608x12@16.667ms | hip | frame_p50 | 4.17 ms | [3.538, 5.921] | 5.097 | 600 | 12 dependent chainStep launches + one sync per frame; band is min/max over frames |
| frame | 8388608x12@16.667ms | hip | frame_p99 | 5.097 ms | [5.097, 5.097] |  | 1 | nearest-rank over frames |
| frame | 8388608x12@16.667ms | hip | missed_frames_per_10000 | 0 frames | [0, 0] |  | 1 | 0 of 600 frames exceeded the period |
| frame | 8388608x12@16.667ms | hip | sync_points_per_frame | 1 count | [1, 1] |  | 1 | one host sync per frame; the 11 intra-frame dependencies ride stream order |
| pair | 8388608x12@16.667ms | hip | frame_p50 | 9.319 ms | [9.17, 10.627] | 10.192 | 600 | 12 dependent chainStep launches + one sync per frame; band is min/max over frames |
| pair | 8388608x12@16.667ms | hip | frame_p99 | 10.192 ms | [10.192, 10.192] |  | 1 | nearest-rank over frames |
| pair | 8388608x12@16.667ms | hip | missed_frames_per_10000 | 0 frames | [0, 0] |  | 1 | 0 of 600 frames exceeded the period |
| pair | 8388608x12@16.667ms | hip | sync_points_per_frame | 1 count | [1, 1] |  | 1 | one host sync per frame; the 11 intra-frame dependencies ride stream order |
| pair | 8388608x12@16.667ms | hip | besteffort_throughput | 169.9 GB/s | [169.9, 169.9] |  | 1 | 8448 saxpy(16M) launches on the second stream, batches of 24 refilled on completion |
| pair | 8388608x12@16.667ms | hip | protected_p99_slowdown | 100.0 % | [100.0, 100.0] |  | 1 | frame p99 co-run vs solo 5.097 ms |
| pair | 8388608x12@16.667ms | hip | besteffort_pct_of_solo | 76.1 % | [76.1, 76.1] |  | 1 | vs solo pipelined saxpy(16M) 223.2 GB/s |
| pair | 8388608x12@16.667ms | hip | goodput | 88.1 % | [88.1, 88.1] |  | 1 | mean of protected on-time % and best-effort % of solo |

### 3.3 CPU backend

The CPU backend runs every launch inline on the calling thread, so
`launch_call` equals the kernel's own time, a second stream is not
concurrent (the pair row shows the frame absorbing the best-effort work),
and the pipelined and isolated modes coincide.

Identity: AMD RYZEN AI MAX+ 395 w/ Radeon 8060S | cpu | linux 7.0.0-30-generic | 8fea9b63 | auto | 2026-09-06T17:11:37Z

| Workload | Shape | Device | KPI | Median | Noise band | p95 | n | Note |
|---|---|---|---|---|---|---|---|---|
| kernel.saxpy | 1048576 | cpu | duration_isolated | 44.71 us | [43.45, 57.74] | 67.43 | 5 | launch+sync per sample; class memory-bound |
| kernel.saxpy | 1048576 | cpu | duration_pipelined | 44.69 us | [43.63, 69.01] |  | 5 | 50 queued launches / count; class memory-bound |
| kernel.saxpy | 1048576 | cpu | achieved_rate | 46.93 GFLOP/s | [46.93, 46.93] |  | 1 | flops / duration_pipelined |
| kernel.saxpy | 16777216 | cpu | duration_isolated | 979.2 us | [949.1, 1112.4] | 1280.8 | 5 | launch+sync per sample; class memory-bound |
| kernel.saxpy | 16777216 | cpu | duration_pipelined | 1049.0 us | [964.8, 1066.5] |  | 5 | 50 queued launches / count; class memory-bound |
| kernel.saxpy | 16777216 | cpu | achieved_rate | 31.99 GFLOP/s | [31.99, 31.99] |  | 1 | flops / duration_pipelined |
| kernel.dot | 1048576 | cpu | duration_isolated | pending | | | | 351 launches failed on this backend (kernel not registered; see stderr) |
| kernel.dot | 1048576 | cpu | duration_pipelined | pending | | | | 351 launches failed on this backend (kernel not registered; see stderr) |
| kernel.dot | 1048576 | cpu | achieved_bandwidth | pending | | | | 351 launches failed on this backend (kernel not registered; see stderr) |
| kernel.dot | 16777216 | cpu | duration_isolated | pending | | | | 351 launches failed on this backend (kernel not registered; see stderr) |
| kernel.dot | 16777216 | cpu | duration_pipelined | pending | | | | 351 launches failed on this backend (kernel not registered; see stderr) |
| kernel.dot | 16777216 | cpu | achieved_bandwidth | pending | | | | 351 launches failed on this backend (kernel not registered; see stderr) |
| kernel.stencil5 | 1024x1024 | cpu | duration_isolated | 116.7 us | [114.4, 159.8] | 188.9 | 5 | launch+sync per sample; class memory-bound |
| kernel.stencil5 | 1024x1024 | cpu | duration_pipelined | 139.2 us | [124.2, 142.1] |  | 5 | 50 queued launches / count; class memory-bound |
| kernel.stencil5 | 1024x1024 | cpu | achieved_rate | 60.28 GFLOP/s | [60.28, 60.28] |  | 1 | flops / duration_pipelined |
| kernel.stencil5 | 4096x4096 | cpu | duration_isolated | 2269.1 us | [2096.7, 2370.4] | 2579.4 | 5 | launch+sync per sample; class memory-bound |
| kernel.stencil5 | 4096x4096 | cpu | duration_pipelined | 2231.8 us | [2068.3, 2281.6] |  | 5 | 50 queued launches / count; class memory-bound |
| kernel.stencil5 | 4096x4096 | cpu | achieved_rate | 60.14 GFLOP/s | [60.14, 60.14] |  | 1 | flops / duration_pipelined |
| kernel.reduceSum | 1048576 | cpu | duration_isolated | pending | | | | 702 launches failed on this backend (kernel not registered; see stderr) |
| kernel.reduceSum | 1048576 | cpu | duration_pipelined | pending | | | | 702 launches failed on this backend (kernel not registered; see stderr) |
| kernel.reduceSum | 1048576 | cpu | achieved_bandwidth | pending | | | | 702 launches failed on this backend (kernel not registered; see stderr) |
| kernel.reduceSum | 16777216 | cpu | duration_isolated | pending | | | | 702 launches failed on this backend (kernel not registered; see stderr) |
| kernel.reduceSum | 16777216 | cpu | duration_pipelined | pending | | | | 702 launches failed on this backend (kernel not registered; see stderr) |
| kernel.reduceSum | 16777216 | cpu | achieved_bandwidth | pending | | | | 702 launches failed on this backend (kernel not registered; see stderr) |
| kernel.matmulTiled | 512^2 | cpu | duration_isolated | pending | | | | 51 launches failed on this backend (kernel not registered; see stderr) |
| kernel.matmulTiled | 512^2 | cpu | duration_pipelined | pending | | | | 51 launches failed on this backend (kernel not registered; see stderr) |
| kernel.matmulTiled | 512^2 | cpu | achieved_bandwidth | pending | | | | 51 launches failed on this backend (kernel not registered; see stderr) |
| kernel.matmulTiled | 1024^2 | cpu | duration_isolated | pending | | | | 51 launches failed on this backend (kernel not registered; see stderr) |
| kernel.matmulTiled | 1024^2 | cpu | duration_pipelined | pending | | | | 51 launches failed on this backend (kernel not registered; see stderr) |
| kernel.matmulTiled | 1024^2 | cpu | achieved_bandwidth | pending | | | | 51 launches failed on this backend (kernel not registered; see stderr) |
| kernel.wmmaGemm | 512^2 | cpu | duration_isolated | 2788.6 us | [2543.8, 2792.9] | 2975.1 | 5 | launch+sync per sample; class matrix-core |
| kernel.wmmaGemm | 512^2 | cpu | duration_pipelined | 2797.8 us | [2350.3, 2989.5] |  | 5 | 50 queued launches / count; class matrix-core |
| kernel.wmmaGemm | 512^2 | cpu | achieved_rate | 95.95 GFLOP/s | [95.95, 95.95] |  | 1 | flops / duration_pipelined |
| kernel.wmmaGemm | 1024^2 | cpu | duration_isolated | 19551.6 us | [19314.4, 20293.6] | 20561.3 | 5 | launch+sync per sample; class matrix-core |
| kernel.wmmaGemm | 1024^2 | cpu | duration_pipelined | 18483.0 us | [17635.7, 19392.1] |  | 5 | 50 queued launches / count; class matrix-core |
| kernel.wmmaGemm | 1024^2 | cpu | achieved_rate | 116.2 GFLOP/s | [116.2, 116.2] |  | 1 | flops / duration_pipelined |
| kernel.gather | 1048576 | cpu | duration_isolated | 77.26 us | [73.21, 96.53] | 122.0 | 5 | launch+sync per sample; class indirect |
| kernel.gather | 1048576 | cpu | duration_pipelined | 77.76 us | [74, 97.47] |  | 5 | 50 queued launches / count; class indirect |
| kernel.gather | 16777216 | cpu | duration_isolated | 7215.4 us | [7124.7, 7314.9] | 8103.8 | 5 | launch+sync per sample; class indirect |
| kernel.gather | 16777216 | cpu | duration_pipelined | 7097.0 us | [6844.4, 7255.4] |  | 5 | 50 queued launches / count; class indirect |
| cg | 1024x1024x2000 | cpu | iterations_per_second | pending | | | | 2001 launches failed on this backend (kernel not registered; see stderr) |
| cg | 1024x1024x2000 | cpu | host_cost_per_node | pending | | | | 2001 launches failed on this backend (kernel not registered; see stderr) |
| degenerate | 1x1x2000 | cpu | iterations_per_second | pending | | | | 2001 launches failed on this backend (kernel not registered; see stderr) |
| degenerate | 1x1x2000 | cpu | host_cost_per_node | pending | | | | 2001 launches failed on this backend (kernel not registered; see stderr) |
| seam | 5us | cpu | kernel_time | 6.46 us | [6.46, 6.46] |  | 1 | spin(4235) pipelined x100; calibrated from 1.181 ns/iter |
| seam | 5us | cpu | launch_call | 6.302 us | [6.282, 6.402] | 6.442 | 5 | host time inside the launch statement |
| seam | 5us | cpu | pipelined_overhead | -0.087 us | [-0.145, 0.066] |  | 5 | (100 queued launches + sync)/100 minus kernel_time |
| seam | 5us | cpu | isolated_overhead | -0.124 us | [-0.144, 0.056] |  | 5 | launch + sync minus kernel_time |
| seam | 50us | cpu | kernel_time | 50.96 us | [50.96, 50.96] |  | 1 | spin(42541) pipelined x100; calibrated from 1.175 ns/iter |
| seam | 50us | cpu | launch_call | 50.856 us | [50.796, 50.966] | 51.157 | 5 | host time inside the launch statement |
| seam | 50us | cpu | pipelined_overhead | 0.2 us | [0.018, 0.263] |  | 5 | (100 queued launches + sync)/100 minus kernel_time |
| seam | 50us | cpu | isolated_overhead | -0.07 us | [-0.13, 0.04] |  | 5 | launch + sync minus kernel_time |
| seam | 200us | cpu | kernel_time | 200.1 us | [200.1, 200.1] |  | 1 | spin(169658) pipelined x100; calibrated from 1.179 ns/iter |
| seam | 200us | cpu | launch_call | 199.5 us | [199.5, 199.5] | 202.4 | 5 | host time inside the launch statement |
| seam | 200us | cpu | pipelined_overhead | 0.055 us | [-0.027, 0.267] |  | 5 | (100 queued launches + sync)/100 minus kernel_time |
| seam | 200us | cpu | isolated_overhead | -0.553 us | [-0.573, -0.483] |  | 5 | launch + sync minus kernel_time |
| frame | 8388608x12@16.667ms | cpu | frame_p50 | 3.339 ms | [1.986, 7.233] | 5.184 | 600 | 12 dependent chainStep launches + one sync per frame; band is min/max over frames |
| frame | 8388608x12@16.667ms | cpu | frame_p99 | 5.184 ms | [5.184, 5.184] |  | 1 | nearest-rank over frames |
| frame | 8388608x12@16.667ms | cpu | missed_frames_per_10000 | 0 frames | [0, 0] |  | 1 | 0 of 600 frames exceeded the period |
| frame | 8388608x12@16.667ms | cpu | sync_points_per_frame | 1 count | [1, 1] |  | 1 | one host sync per frame; the 11 intra-frame dependencies ride stream order |
| pair | 8388608x12@16.667ms | cpu | frame_p50 | 53.318 ms | [48.956, 60.047] | 58.035 | 600 | 12 dependent chainStep launches + one sync per frame; band is min/max over frames |
| pair | 8388608x12@16.667ms | cpu | frame_p99 | 58.035 ms | [58.035, 58.035] |  | 1 | nearest-rank over frames |
| pair | 8388608x12@16.667ms | cpu | missed_frames_per_10000 | 10000.0 frames | [10000.0, 10000.0] |  | 1 | 600 of 600 frames exceeded the period |
| pair | 8388608x12@16.667ms | cpu | sync_points_per_frame | 1 count | [1, 1] |  | 1 | one host sync per frame; the 11 intra-frame dependencies ride stream order |
| pair | 8388608x12@16.667ms | cpu | besteffort_throughput | 180.9 GB/s | [180.9, 180.9] |  | 1 | 28800 saxpy(16M) launches on the second stream, batches of 24 refilled on completion |
| pair | 8388608x12@16.667ms | cpu | protected_p99_slowdown | 1019.4 % | [1019.4, 1019.4] |  | 1 | frame p99 co-run vs solo 5.184 ms |
| pair | 8388608x12@16.667ms | cpu | besteffort_pct_of_solo | 97.9 % | [97.9, 97.9] |  | 1 | vs solo pipelined saxpy(16M) 184.87 GB/s |
| pair | 8388608x12@16.667ms | cpu | goodput | 48.9 % | [48.9, 48.9] |  | 1 | mean of protected on-time % and best-effort % of solo |

### 3.4 Device tier on the seam pass

`cajeta profile summary --csv` over the profiled seam pass (`spin`, all three
durations mixed, 2,133 launches): average device span 100.4 µs on HIP. The
unprofiled rows put the three kernel times at 7.35, 53.6 and 207.3 µs (mean
89.4 µs), so the device tier reads the same kernels about 11 µs longer on
average, which is the dispatch-to-completion part the host clock cannot
see inside a pipelined batch. The CPU backend's figure (88.9 µs) is the inline
launch itself. A per-duration split needs one profiled pass per target,
which a later unit can add; the mixed figure is recorded, not used for
verdicts.

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
| CPU backend: `dot`, `reduceSum`, `matmulTiled`, `cg`, `degenerate` pending | baseline | The CPU barrier fission declines a workgroup barrier inside a loop ("unstructured barrier control flow", `CpuBarrierFission.cpp`) — the LDS tree reduce, the two-array final reduce and the LDS-tiled GEMM all carry one. The kernel gets no CPU code; the launch prints `no registered CPU kernel` and returns. Two silences fixed in Unit 0: the build now prints `[xpu-kernel-skipped] … barrier fission: …` and the runtime now counts the failed launch (`XpuCpuBarrierFissionNoteTests`, 3 tests). | The fission accepts a barrier inside a uniform loop (a compiler unit, not part of this family); then rerun the CPU leg and fill the rows |
| `bandwidth_fraction` pending on every kernel | baseline | needs the device's measured achievable bandwidth | scheduling Unit 2 fills §2 |
| `matrix_core_fraction`, `attention_kernel_duration` not produced by the harness | baseline | the harness rows are host-clocked; §3.5 measured both from device spans in one profiled run (94%; 43.6 µs decode, 917 µs prefill) | a later unit folds a profiled llm pass into the harness so the rows carry them |
| `per_token_p99` pending | baseline | `SchedThroughput` prints a per-run mean, not per-token latencies | cajeta-llm's bench emits per-token timings (profiles plan Unit 1 needs it) |
| Seam device-tier figure (§3.4) is a three-duration mix | baseline | one profiled pass covers 5, 50 and 200 µs together; the summary cannot split them by name | one profiled pass per target duration |
| Frame p50 band 3.5–6.6 ms solo | baseline | run-to-run jitter of the unscheduled frame; p99 and the miss count are the frame KPIs that verdicts read | a scheduler unit that claims to reduce jitter measures p50 with N runs, not one |
| CG `queue-empty time` not measured | baseline | needs the device-tier spans of the CG loop subtracted from wall; the harness rows are host-clocked | a profiled CG pass (the device tier is available, §3.5 shows the method) |

## 6. Closing summary (after the last unit)

Per workload and device: baseline row, closing row, better / same / worse
with the noise band. Nothing may read `worse`.

| Workload | Device | KPI | Baseline | Closing | Verdict |
|---|---|---|---|---|---|
| | | | | | |
