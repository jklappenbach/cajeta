# kernel-profile — GPU and kernel work, shaped for the profiler

A sibling of [`profile`](../profile/README.md), pointed at a different question.
`profile` asks *how fast is Cajeta against Rust and C++*. This one asks: **what does
GPU work look like in the profiler, and what can you click on.**

Nothing here is tuned. Every choice exists to put something specific on screen.

## Run it

```sh
./run.sh          # GPU build (amdgpu / gfx1151) — the point of the sample
./run.sh cpu      # portable build — host lanes only, the control
```

Both write `cajeta.pftrace` beside this file. Open it with
**Tools → Cajeta → Open Cajeta Profile…**, or from the Cajeta profiler tool window.

The program **prints the backend it actually got** before it does anything else:

```
=== cajeta kernel profile sample ===
active backend: hip
```

That line is not decoration. A GPU build that fell back to the CPU produces a trace
with no device tracks at all, and without the line you cannot tell that apart from a
profiler bug. The amdgpu test suite compiled for the CPU for weeks before anyone
noticed, which is why it is printed first and checked in the source.

## What to look for

**Timeline tab** — the track hierarchy the workload was built to produce:

```
cajeta.xpu.hip device 0
  context 0
    queue 97155764854928        <- stream A
    queue 97155765704704        <- stream B
cajeta.thread.0                 <- host
```

Two streams, so there are two device queues to compare. The timeline exists to answer
*what was everything else doing at that moment*, which a single queue cannot show.

**Flame graph / Totals** — three kernels appear as device slices by their own names:

| kernel | shape |
|---|---|
| `saxpy` | `y = a*x + y`, the heaviest |
| `vecAdd` | `c = a + b` |
| `scale`  | `y = y * k`, deliberately the cheapest |

Three distinct kernels means the totals table has rows worth comparing, and one of them
is visibly *not* where the time went. Host frames (`KernelProfile.runStream`,
`hostWork`) and the XPU API (`KernelBuffer.upload`, `KernelStream.sync`) appear beside
them, so host and device lanes interleave rather than the device looking permanently
busy.

**Click anything.** A totals row navigates to the method's declaration; a flame frame
navigates to the line it was sampled on. A kernel frame can also reach the *launch
site* — the line that dispatched it, which is a different place from the kernel body.

## Switching backends

Build `cpu` then `gpu` (or the reverse) and each task rebuilds what it must: the
incremental cache is keyed on `--xpu-backend` and `--xpu-arch`, so a task never
inherits the other's device objects. Unchanged sources are still reused within a
backend, so a second build of the same task stays incremental.

This was not always true. Until 2026-09-02 the cache ignored both flags
(`xpu-cache-discriminator`): building `gpu` then `cpu` yielded a "cpu" binary
that still contained HIP kernels and reported `active backend: hip`, and the
reverse made the gpu task report `cpu` — with a different sha from the same task
built clean, so the artifact genuinely differed while the embedded kernels did
not. `run.sh` carried an unconditional `rm -rf .cajeta/cache` to work around it;
that purge is gone, and the sample no longer pays for a build-tool bug.
