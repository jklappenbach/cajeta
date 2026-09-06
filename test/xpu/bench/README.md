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
- **The CPU backend declines three of them.** `reduceSum`, `finalSum2` and
  `matmulTiled` carry a workgroup barrier inside a loop, which the CPU
  barrier fission rejects as unstructured control flow; the kernel gets no
  CPU code and its launch prints `no registered CPU kernel` and returns. The
  harness snapshots `Device.launchFailures()` around every workload and
  emits **pending** rows, with the reason, instead of numbers when it moves.
  On this leg that makes `dot`, `reduceSum`, `matmulTiled` and `cg` pending
  on the CPU backend. Two things were silent before this unit and are not
  now: the build prints `[xpu-kernel-skipped] … barrier fission: …` for the
  declined shape, and the runtime counts the failed launch
  (`XpuCpuBarrierFissionNoteTests`). Lifting the fission limit is a
  compiler item, filed from the report's residuals.

## Discipline

- **Instrument.** Host clock (`Clock.nanoTime`) around launch and sync, in
  two modes: *isolated* (one launch, one sync) for latency and *pipelined*
  (fifty queued, one sync) for the cost a full queue sees. The profiler's
  AMD device tier (rocprofiler-sdk, loaded from `$ROCM_PATH/lib` — this
  box's user-local ROCm tree carries it) is active and records device
  spans; `run.sh` profiles the seam pass with it as a cross-check, and the
  report's §3.5 uses it on the cajeta-llm run. Set
  `CAJETA_PROFILER_GPU_RING` large (4 M) for a run with tens of thousands
  of launches: the record sink drops on overflow, so a small ring keeps
  per-kernel averages but loses totals. The harness rows stay host-clocked
  so they mean the same thing on every backend.
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
```

`baseline` prints the report's §3 table; `trial` pairs two rows files by
(workload, shape, KPI, backend) and prints §4 rows with delta, the before
row's noise band, and a `keep` / `worse` verdict (exit 1 on any `worse`).
