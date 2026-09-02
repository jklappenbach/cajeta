# 23 — Profiling

Cajeta ships a sampling profiler in the runtime. It needs no rebuild, no
recompile, and no agent: set one environment variable and run the binary you
already have.

```bash
$ CAJETA_PROFILER=1 ./build/exe/myapp
$ # -> ./cajeta.pftrace, in the current directory
```

Open the result at [ui.perfetto.dev](https://ui.perfetto.dev) — drag the file
in. Nothing is uploaded; the UI runs the trace processor in your browser.

## Why no rebuild is needed

Every Cajeta build already carries **per-call line-info probes**: runtime calls at
each method prologue and statement boundary that maintain a shadow stack. They
are what produces a semantic stack trace from a `Throwable`, and they are what
the debugger reads. The profiler samples that same shadow stack from a
background thread.

So the frames are already there — profiling just starts reading them. With
`CAJETA_PROFILER` unset, nothing is armed, no thread starts, and the cost is
exactly zero.

The one build flag that matters is the one that takes the probes *away*:

```bash
$ cajeta build --debug-info=off     # no shadow stack -> nothing to sample
```

A binary built that way refuses to profile, loudly, rather than producing an
empty trace:

```
cajeta.profiler: refusing to arm — this binary was built with --debug-info=off,
so there are no frames to sample. Rebuild with --debug-info=line (the default) to
profile, or --debug-info=full to also get exact line numbers.
```

## Configuration

| Variable | Default | |
|---|---|---|
| `CAJETA_PROFILER` | unset | Set to anything to profile the run. Unset = off. |
| `CAJETA_PROFILER_HZ` | `1000` | Samples per second. |
| `CAJETA_PROFILER_RING` | `4096` | Samples buffered between the sampler and the writer. |
| `CAJETA_PROFILER_GPU_RING` | capture default (`8192`) | **Device** records buffered between the GPU capture layer and the writer. Separate ring, separate limit: a busy kernel loop overflows this long before the sampler's. |
| `CAJETA_PROFILER_OUT` | `cajeta.pftrace` | Where to write the trace. |

The trace is written when `main` returns, and also when a program ends through
`System.exit`. A run killed part-way still leaves a **readable** trace of
everything up to the moment it died — every packet carries its own length, so a
truncated file is a short trace rather than a corrupt one.

## Reading the trace

Each host thread and each fiber is its own track, named `cajeta.thread.N` and
`cajeta.fiber.N`. A fiber's number is its **debugger** id, so a profile and a
debug session call the same fiber the same thing.

Slices carry their source position: click one and the file (and, under
`--debug-info=full`, the line) are in the
argument panel.

### Check the drop count first

The trace opens with an instant on a `cajeta.profiler` track holding the run's
own configuration — sample rate, ring capacity, samples taken, **samples
dropped**, and `dropped_per_mille`.

Read that before trusting the shape. The sampler drops samples when the ring
fills rather than blocking, because blocking would perturb the very program it
is measuring — but a profile that lost a third of its samples looks exactly as
authoritative as a complete one. If `dropped_per_mille` is not near zero, raise
`CAJETA_PROFILER_RING` or lower `CAJETA_PROFILER_HZ` and run again.

A program that keeps a GPU busy needs a bigger ring than the default: a tight
kernel loop measured **40% sample drops** at `4096`, and `CAJETA_PROFILER_RING=65536`
brought it to nothing.

### Device records drop separately

The run record carries a second set for the device side — **`gpu_records_dropped`**,
`gpu_records_kept`, and `gpu_dropped_per_mille`. They count what the GPU capture
ring lost, which is a *different* ring from the sampler's and fills far faster:
one kernel launch is one record, so a loop issuing tens of thousands of launches
overruns the `8192` default while `samples_dropped` stays at zero. A run that
kept 8,192 of 56,843 launches is not a quiet profile — it is a profile missing
six launches in seven.

The capture ring **overwrites**, so what survives is the *most recent* records
and what is lost is the oldest — a truncated profile of the end of the run, not
a thinned sample of all of it. Raise `CAJETA_PROFILER_GPU_RING` until
`gpu_dropped_per_mille` reads zero; it costs one `CajetaGpuEvent` per slot and
nothing at all when the profiler is off.

### A per-kernel table without opening the IDE

The common question about a GPU run — *which kernel cost what* — is a query, not
a picture. `cajeta profile summary` answers it from the trace:

```
$ cajeta profile summary cajeta.pftrace
kernel    count        total          avg          max   share
saxpy        48    761.43 us     15.86 us     66.17 us   37.7%
vecAdd       48    712.02 us     14.83 us     32.14 us   35.2%
scale        48    547.19 us     11.40 us     16.71 us   27.1%

144 slice(s) over 2 track(s); summed 2.02 ms, wall 29.31 ms
```

Rows are cost-ordered, so the answer to "where did the time go" is the first
one. Summed device time can **exceed** the wall span when queues run
concurrently — the run above has two streams — which is why both numbers are
printed rather than a single utilisation figure that would be wrong on any
multi-queue run.

| option | |
|---|---|
| `--from=<dur>`, `--to=<dur>` | Window, **relative to the first slice**. Absolute timestamps are host-clock nanoseconds and differ every run, so a relative window is the only one reusable between two runs of the same program. |
| `--host` | Total host frames instead of device kernels. |
| `--csv` | Machine-readable output. |

`<dur>` is nanoseconds unless suffixed: `500us`, `20ms`, `1s`.

```
$ cajeta profile summary cajeta.pftrace --to=5ms --csv
name,count,total_ns,avg_ns,max_ns
saxpy,24,399994,16666,66165
vecAdd,24,360437,15018,32141
scale,24,271832,11326,15669
```

Device and host are reported separately on purpose. Host frames are wall-clock
spans that *contain* the kernels, so summing them into the same table would both
dominate it and double-count the time it is reporting.

Two outcomes that are not the same thing, and are not reported as one:

- **No device queue in the trace** — a run that never touched an accelerator.
- **Tracks present, but the window excluded every slice** — a bad window.

Neither prints an empty table; both exit non-zero.

### What sampling can and cannot tell you

A sampling profiler answers *where did the time go*, not *how many times was
this called*. Every slice boundary lands on a sample tick, so a slice's
duration is "this frame was on the stack across these ticks" — not a
measurement of one call. Two consequences worth internalizing:

- **Durations are statistical.** A function that appears for 30 ms was on the
  stack for about 30 ms worth of samples. It is not 30 ms of one invocation.
- **Short calls may not appear at all.** Anything that never happens to be on
  the stack at a tick is invisible. Raising `CAJETA_PROFILER_HZ` narrows the
  gap; it does not close it.

Exact call counts are what the instrumentation tier is for; it is a build-time
mode, because it changes what codegen emits.

## Optimized builds still show your source

Attribution follows the program's **source** structure, not its machine frames:

```bash
$ cajeta --emit=exe --opt=O3 -o myapp com.example.Main::main src arch
$ CAJETA_PROFILER=1 ./myapp
```

Optimization level and line-info are independent: `--opt` does not turn the
probes off, and the release flavors leave `--debug-info=line` (the default) in
place. Only `--debug-info=off` removes them.

### Function-level by default, line-level on request

`--debug-info=line` emits a probe per CALL, which is what gives a slice its
`Type.method` and `File.cajeta`. Exact line numbers come from a second probe at
every STATEMENT, and that one is emitted only under `--debug-info=full`.

The split is a measurement, not a preference. At `-O3`, per-call probes are at
parity with an uninstrumented build; adding the per-statement probe costs
**3.5x on ordinary code and up to 9.4x on call-dense code** — and not because
the probe does much work. Emptying its body changes nothing: an opaque call at
every statement boundary is what forbids inlining and folding.

So a default profile tells you which METHOD is hot, for free. When you need the
line, rebuild that run with `--debug-info=full` and accept the slowdown for the
duration of the investigation:

```console
$ cajeta build --debug-info=full
$ CAJETA_PROFILER=1 ./build/exe/myapp
```

Note this makes the profile less representative of production, which is the
usual bargain with instrumentation — the hot method is the reliable answer, the
hot line is the expensive one.

A one-line forwarder that `-O3` inlines out of existence still appears in the
profile, because the probes are calls with side effects — inlining carries them
into the caller rather than deleting them. A profiler reading DWARF or frame
pointers loses that function; this one keeps it.

`tools/profiler/attribution-check.sh` re-checks this end to end against a built
toolchain.

## Exact counts: `--profiler=instrument`

Sampling answers *where does wall time go*. It cannot answer *how many times* —
a method called four million times for a nanosecond each may not appear in a
single sample. When that is the question, build with instrumentation:

```console
$ cajeta build --profiler=instrument
$ ./build/exe/myapp
$ # -> ./cajeta.pftrace, with a cajeta.instrumentation track
```

Each selected method gets an enter/exit probe pair, and the trace carries its
exact entry count and inclusive time. This is a **separate tier**, not a better
sampler: the two coexist in one trace and stay distinguishable — sampling as
nested slices on per-thread tracks, instrumentation as records on
`cajeta.instrumentation`, each stating `source` outright.

Unlike sampling, this **needs a rebuild** and it is not free. That is the
trade: the probe is inside the callee's body, so it counts calls the machine
actually made — including calls into methods the optimizer inlined away, which
a profiler reading DWARF or frame pointers cannot see at all.

The run reports what it cost itself. `instr_probe_ns` on the run record is the
per-pair cost **measured on the machine that ran**, and `instr_overhead_ns` is
that times the number of pairs — so you can judge how much the measurement
distorted what it measured instead of guessing.

### What it costs

Measured 2026-08-22, `-O3` native build, 20 M calls:

| | uninstrumented | instrumented |
|---|---|---|
| tight loop over a one-line callee | 0.06 s | 1.18 s |

The useful form of that number is not the ratio. **A probe pair costs a fixed
~57 ns per call**, so the overhead is entirely a function of how often you call,
and nothing else:

| calls/second in the hot path | added wall time |
|---|---|
| 100 K | 0.6 % |
| 1 M | 6 % |
| 10 M | 57 % |

Pick the tier from that table, not from a rule of thumb. The 19x above is what
20 M calls/second of a method that does almost nothing looks like; a method
doing a microsecond of real work pays 6 % at the same call count.

Two independent measurements agree on the 57 ns, which is why it is quoted
rather than estimated: the run's own `instr_probe_ns` self-calibration reported
57, and timing the same program with and against the flag gave 56.

About three quarters of it is the two timestamps — `clock_gettime(CLOCK_MONOTONIC)`
measures 21 ns/call on this machine, and a span needs one at each end. The rest
is two relaxed atomic adds and the calls themselves. So if you need counts and
not durations, that is where the saving would come from; today the tier always
measures both.

The direct lever is `--profiler-select`: probes are emitted only for selected
code, so narrowing the selection removes the cost rather than hiding it.

### Instrument part of a program

Probes are emitted only for selected code. This is a compile-time decision, so
unselected code carries nothing to skip — a narrow selection is a real
reduction in overhead, not a display filter.

```console
$ cat prof.select
# the codec, but not its logging
include dev.myapp.codec.**
exclude dev.myapp.codec.Trace

$ cajeta build --profiler=instrument --profiler-select=prof.select
```

`**` crosses package boundaries, `*` stays within one name. Include defines the
universe (no `include` lines means everything) and `exclude` subtracts from it.
That is the whole rule — membership never depends on line order or on which
pattern is more specific, so a file can be read in any order and mean the same
thing.

Two things the trace records so you do not have to remember them:

- **The selection in force.** A profile that silently omits code otherwise
  reads as though that code were free.
- **The optimization level the build used.** `--profiler=instrument` pins no
  level, so the level is part of what every number means.

A method entered with no probed frame beneath it — called from excluded code,
or from the runtime at the program's root — records that as
`outside_selection_calls` rather than being attributed to the nearest probed
ancestor, which would draw a call edge that never happened.

## GPU work

When a program dispatches kernels, each device, context, and queue is its own
track, and an arrow runs from the host call site that launched a kernel to the
kernel's execution on the device — click through to see which line started
which piece of GPU work.

Where the numbers come from depends on the backend. On AMD, kernel spans come
from rocprofiler's own dispatch records — the device saying what it did. On
Vulkan, they come from timestamp queries bracketing each dispatch — real
device time, one step coarser. When neither mechanism is available the span
is the host's submit-to-complete window, which is true and about a wider
thing than the kernel. Every span is annotated with the **tier** that
measured it, the **confidence** of the clock correlation behind it, and any
**integrity flags** the runtime raised (a timestamp register that reset on a
low-power transition, a span outside its own launch-to-resolution bracket) —
so a degraded or untrustworthy measurement renders as one instead of
blending in.

Device timing degrades rather than disappearing when a vendor profiler is
absent: a run whose rocprofiler is missing, whose Vulkan queue family cannot
timestamp, or whose driver refuses calibration still profiles — the tier
says what each number actually is.

On the Vulkan backend, each dispatch also emits an explicit **"host blocked
on GPU"** span for the time the host spent waiting on the queue. That wait is
the backend's current per-dispatch cost, and the labelled span is there so it
reads as what it is rather than as an unexplained gap.

## See also

- [05 Debugging](05-debugging.md) — the shadow stack this reads, from the other side.
- `specs/cajeta-profiler-spec.md` — the requirements this implements.
