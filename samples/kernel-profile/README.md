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

## Known sharp edge

`run.sh` purges the object cache before every build. The build tool's incremental
cache is **not keyed on `xpu-backend`** (measured 2026-09-01): building `gpu` then
`cpu` yields a "cpu" binary still containing HIP kernels that reports
`active backend: hip`, and the reverse order makes the gpu task report `cpu`. Both
produce a different sha from the same task built clean, so the artifact genuinely
differs while the embedded kernels do not.

A "purge only when the task changed" marker was tried first and is not enough: it
records the task *requested*, so a poisoned build writes a clean-looking marker and the
next run trusts it. A rebuild is cheap next to a demo that lies. The fix belongs in the
build tool's cache key.
