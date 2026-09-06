# xpubench — the xpu-tile baseline harness

The numbers behind [`specs/xpu-tile-scheduling-report.md`](../../../specs/xpu-tile-scheduling-report.md).
Every optimization unit of the three `xpu-tile-*` plans is accepted or refused
against rows this harness wrote; Unit 0 of the scheduling plan is the first
run, before any scheduler code exists.

```sh
test/xpu/bench/run.sh            # gfx1151 and the CPU backend, full shapes
test/xpu/bench/run.sh hip        # the GPU only
test/xpu/bench/run.sh cpu        # the CPU backend only
test/xpu/bench/run.sh smoke      # small shapes, both legs, a few minutes
```

Everything lands under the repo's `tmp/bench/` (never `/tmp`): the binaries,
one `rows-<leg>-<stamp>.jsonl` per leg, the seam probe's profiler trace and
its summary CSV, and `baseline-<leg>-<stamp>.md`, the rendered table that is
pasted into the report.

## What it measures

| Workload | Shapes | KPIs |
|---|---|---|
| `kernel.*` — saxpy, dot, stencil5, reduceSum, matmulTiled, wmmaGemm, gather | two per kernel (1M/16M elements, 1024²/4096² grid, 512²/2048² matrices) | `duration_isolated` (launch+sync, p50 and p95), `duration_pipelined` (50 queued launches / 50), `bytes_moved`, `achieved_bandwidth`, `achieved_rate` |
| `cg` — stencil SpMV + 2 dots (per-block partials) + 1 final reduce + 3 axpys, seven launches per iteration | 1024², 10,000 iterations | `iterations_per_second`, `host_cost_per_node`, `wall_per_iteration` |
| `degenerate` — the same loop on one element | 1×1, 10,000 iterations | the framework overhead per iteration |
| `frame` — 12 dependent kernels at a 16.67 ms period | 8M elements, 600 frames | `frame_p50`, `frame_p99`, `missed_frames_per_10000`, `sync_points_per_frame` |
| `pair` — the frame with a saturating best-effort stream beside it | same | plus `protected_p99_slowdown`, `besteffort_throughput`, `besteffort_pct_of_solo`, `goodput` |
| `seam` — a calibrated spin kernel at 5 / 50 / 200 µs | three durations | `launch_call`, `pipelined_overhead`, `isolated_overhead`, each minus `kernel_time` |
| `llm.*` — cajeta-llm `SchedThroughput`, one process per run | 2,048-token prompt, 64 generated | `prefill_ms`, `prefill_tokens_per_second`, `tokens_per_second`, `ms_per_token` |

The plan named "every kernel in `test/xpu`"; those are eighty inline probe
sources, most of them emit checks, so the set above stands in — one
representative per class the scheduler will classify against.

Two facts about the set, both measured on 2026-09-06:

- **Reductions are two-stage.** `dot` and `reduceSum` write one partial per
  block and `finalSum2` folds the partials (both CG dots in one launch). The
  first cuts used a same-address float atomic and were contention-bound on
  HIP: 446 µs for 1M elements, 2.3 GFLOP/s, a bimodal band at 16M. That was
  the kernel, not any scheduler, so the baseline carries the two-stage form.
- **Declined CPU kernels become pending rows.** The CPU barrier fission
  declines, by name, any barrier shape it cannot run correctly (per-work-item
  code in the latch of a barrier loop, a barrier under divergent control
  flow, a work-item-dependent trip count); the kernel gets no CPU code and
  its launch prints `no registered CPU kernel` and returns. The harness
  snapshots `Device.launchFailures()` around every workload and emits
  **pending** rows, with the reason, instead of numbers when it moves. The
  build prints `[xpu-kernel-skipped] … barrier fission: …` for the declined
  shape and the runtime counts the failed launch
  (`XpuCpuBarrierFissionNoteTests`). The first CPU leg (2026-09-06) had
  `dot`, `reduceSum`, `matmulTiled`, `cg` and `degenerate` pending because
  the fission declined a uniform loop whose code after its last barrier was
  the latch itself (`reduceSum`, `finalSum2`, `matmulTiled`); that was fixed
  the same day (`XpuCpuBarrierFissionLoopTests`) and the leg reran.

## Discipline

- **Instrument.** Host clock (`Clock.nanoTime`) around launch and sync, in
  two modes: *isolated* (one launch, one sync) for latency and *pipelined*
  (fifty queued, one sync) for the cost a full queue sees. The harness rows
  stay host-clocked so they mean the same thing on every backend.
- **Profiled passes** (scheduling plan 0.2.4). After the timed run, `run.sh`
  runs each workload once more under `CAJETA_PROFILER=1` — one kernel shape
  and one seam target per pass, so every span in a trace belongs to one row
  (`--kernel-shape=small|large`, `--seam-targets=5`) — summarises the trace
  per kernel (`cajeta profile summary --csv`) and derives the rows the host
  clock cannot give (`xpubench-report spans`, `Spans.cajeta`): per-kernel
  `device_span`, the CG stand-in's `device_time_per_iteration` and
  `queue_empty_time`, the seam probe's span per target, and on the llm run
  `matrix_core_fraction`, `device_busy` and the two
  `attention_kernel_duration`s. Every figure is an average span times the
  launch count the harness recorded in its `launches` rows (kernel names
  with multiplicity, iterations issued), never a total: the capture ring
  overwrites its oldest records, and the summary's first CSV line
  (`# gpu_records_kept=N gpu_records_dropped=M`) plus the launch counts turn
  a lossy pass into `pending` rows with the counts in the note. Rings are
  sized per pass (65 K kernels and seam, 256 K CG, 4 M llm). The AMD device
  tier is rocprofiler-sdk from `$ROCM_PATH/lib`; on the CPU backend a span
  is the inline launch itself. `SPANS=0` skips the passes; `SPANS_ONLY=1`
  with `KEEP_ROWS=1` and the leg's `DATE=`/`STAMP=` re-runs only the passes
  of the listed workloads and appends their rows (a later row with the same
  key supersedes an earlier one in both report verbs).
- **Warm-up.** Every kernel runs once per shape before timing.
- **Blocks.** Five blocks per KPI; each block's median is one sample of the
  noise band (min/max over blocks); p95 is over every individual sample.
- **Idle gate.** The harness refuses (exit 3) while another `cajeta_test`,
  `xpubench`, `gpuparity`, `SchedThroughput`, or llama.cpp binary is alive,
  by process name, excluding itself. Load average is printed beside it.
- **Arms.** `--arms=a,b` runs arms in A/B/B/A order; in Unit 0 there is one
  arm, `baseline`, and the switch point (`Workloads.applyArm`) is where a
  later unit turns the scheduler on and off.
- **Identity.** Every row carries device, backend, driver, compiler commit,
  power mode, date, and arm.

`XpuBenchHarnessTests.cpp` compiles the harness's own `Stats` and `Gate`
sources and checks the A/B/B/A order, the band arithmetic, and the gate in
both directions (it refuses beside a live process named like a bench, and
does not refuse on a clean table or because of its own pid).

## Rendering

```sh
tmp/bench/xpubench-report baseline tmp/bench/rows-hip-<stamp>.jsonl
tmp/bench/xpubench-report trial before.jsonl after.jsonl --id=T-001 --unit="scheduling U1" --commit=<sha>
tmp/bench/xpubench-report spans --csv=prof-cg-hip-<stamp>.csv --rows=prof-cg-hip-<stamp>.jsonl --out=rows-hip-<stamp>.jsonl
```

`baseline` prints the report's §3 table (`launches` rows hidden, a later
row superseding an earlier one with the same key); `trial` pairs two rows
files by (workload, shape, KPI, backend) and prints §4 rows with delta, the
before row's noise band, and a verdict — `keep`, `worse`, or `single` when
the before row has no band (n = 1: reported, not gated; exit 1 on any
`worse`). With `--bands=<rerun rows>` (the day-apart rerun of the before
code) a KPI's band is the union of the two runs' bands and its n their sum,
so a pair of single-sample rows is banded by the two runs' spread and a
drifting five-block KPI widens to that drift; a KPI absent from the rerun
keeps its own band. `spans` appends device-span rows derived from one
profiled pass (`run.sh` calls it after every pass).

A leg can be split across invocations to fit a tool's timeout:
`run.sh cpu --workloads=kernels`, then `KEEP_ROWS=1 DATE=<the first
run's date> STAMP=<its stamp> SKIP_BUILD=1 run.sh cpu
--workloads=cg,degenerate,frame,pair,seam` — one rows file, one identity.
The compiler field is `git rev-parse HEAD`, so commit before the leg of
record.
