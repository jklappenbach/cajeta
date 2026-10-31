# The scheduler: what runs when, beside what

A kernel launch in Cajeta is a submission, and the runtime decides when it
runs, alongside what, on which queue, and whether it is captured for replay.
This page is the runtime half of the tile scheduling work. It covers what a
submission carries today, what the scheduler decides, which of those decisions
ship and which are design, and where the backends differ. The compile-time
half is what the compiler records about each kernel so that the scheduler
never has to guess it, and it is on the [optimization](optimization.md) page
under the manifest. The specifications are `xpu-tile-manifest` and
`xpu-tile-scheduling`, and every number behind them is sourced in
`xpu-tile-scheduling-findings`.

If you want to see all of this on one concrete model, read the
[walkthrough](walkthrough.md) first. The scheduler is what turns its 1,360
launches a token into a millisecond of host time on CUDA.

## 1. Every launch is a submission

```cajeta
k.launch(stream, grid: [workgroups], block: [threads], sharedBytes: [n])(args);
```

That line is a submission with the kernel's default policy. The runtime binds
the kernel's manifest, which is the compiler's record of its footprint, its
parameter access modes and its tile geometry, to the actual buffer handles.
That gives the submission its read set, its exclusive-write set, its
accumulate set and its indirect-bound set without the author restating them. A
`KernelSubmission` passed in place of the stream adds what a default cannot
know: a policy, a deadline window, a protection bit, a compute-unit budget, a
capture identity, an energy budget. `Scheduler.submit` is the explicit form.

The submission is value-typed, and it is never a closure over the launch site.
It is checked after every launch. `Device.checkLaunch()` throws
`XpuLaunchException` with one of seven named refusals (no code registered, a
misaligned buffer, the scalar twin of a wave kernel would have run, an
unsupported declared wave width, a block that differs from the pinned one, a
missed deadline, too many threads or too much shared memory). A refused
submission is returned to the caller with its reason. It is never dropped and
never run late in silence.

## 2. What the scheduler decides

There are four decisions, listed in the order the design takes them. The
status column says what the runtime does today (cajeta 0.46.3).

| decision | the rule | status |
|---|---|---|
| **classification** | each submission's arithmetic class (bandwidth-bound, compute-bound, latency-bound) is derived from the manifest's cost expressions against the measured device, per submission, and the author never declares it. The same attention kernel spans two orders of magnitude of arithmetic intensity between prefill and decode | design; today the engine's routes classify by regime (prefill against decode) in library code |
| **ordering and admission** | the dependency graph comes from the access sets. A protected submission (`latency`, `frameBudget`) is never deferred or throttled, and a best-effort one may be. Admission refuses what would oversubscribe the device or a resident pool before it launches | design; today submissions execute in stream order immediately, and admission is the engine's (the LLM scheduler admits a request only when its K/V blocks fit) |
| **co-residency and partition** | the cheapest control that meets the requirement wins. The controls are to refuse a launch you would regret, to cap what you launch, and to stop only what can be safely stopped. Compute-unit masks on HIP and green contexts on CUDA serve where the backend offers them. Bandwidth is the shared resource nothing partitions, and every admission accounts for it | design; measured in the findings (soft rate control beat fixed hardware slices; preemption is a 100 µs-class operation and never a tier) |
| **capture and replay** | repeated submissions with the same identity, stable geometry and stable buffer addresses are one replayable graph node. The unit of scheduling is the chain, because real kernels run 1 to 12 µs and a per-kernel decision costs a tenth of a 100 µs node | **shipped on CUDA** as deferred streams. The first sight of a launch sequence runs direct, the third is recorded as a graph, and later sightings replay it with changed grids and arguments patched in. `deferStat` reports queued, replayed, direct, graphs, patches and flushes |

Three invariants hold across all four, shipped or not. Results are invariant,
so no scheduling decision changes a kernel's output, and order-dependent float
accumulation is documented and never co-run. The scheduler is off the critical
path, so its per-decision cost is budgeted and reported, and a runtime switch
bypasses it for A/B measurement. A single process is the advantage. There is
no context-switch tax and no cross-address-space channel, and the buffer sets
are complete at submit, which removes the deadlock that made earlier systems
reject kernel-granularity scheduling.

## 3. Deferred streams, the shipped half

On CUDA, `KernelStream.setDeferred(true)` queues a stream's launches and
flushes them at the next point where a result becomes observable (a sync, a
download, an event wait). The LLM engine's decode stream is deferred. A decode
step is about 1,360 launches, and replayed as a graph it is issued in about a
millisecond of host time. That is how single-sequence decode reached
llama.cpp's rate once the kernels were already at the memory bus.

Two kinds of launch go direct. One is a launch carrying a texture, an image, a
buffer array, or more than 48 arguments or 384 bytes of them. The other is
every launch while a profiler's device sink is live, because the profiler
attributes each launch at submission. `CAJETA_XPU_DEFER=0` never defers, `all`
defers every stream from its first launch, and unset leaves it to
`setDeferred`. Deferral is a no-op on HIP, Vulkan and the CPU backend, so
there the host pays for every launch. The engine's answer on HIP is the
batched pipeline, which overlaps the host's share of a step with the device's.

## 4. Where Vulkan differs

The Vulkan backend serializes dispatch. Today there is one command buffer per
launch, submitted and then waited on under the submit mutex, so the host
blocks for the full kernel duration on every launch and two streams never
overlap. The `vulkan-batch-pipelining` work (approved 2026-10-24) submits a
batch's first chunk early and brackets dispatches inside the batch instead of
eagerly. Chunk 16 measured at +2.4 percent at four sequences and +2.8 percent
at one on the 8B. Vulkan also has no event clock outside the profiler's
timestamp seam, so a timed choice there is timed on the host.

## 5. The engine's scheduler above it

The LLM engine has a scheduler of its own, `Scheduler` in `dev.cajeta.llm`,
one level up. It does continuous batching over requests. It admits a request
only when free K/V blocks cover its whole uncached suffix plus one block per
layer of decode headroom. It advances a prefilling request by a chunk and a
decoding one by a token. The chunk is learned, 32 to 512, when nothing else
decodes, and it is sized to a 150 ms pause budget when something does. It
preempts the youngest running request and never an older one, and it resumes
preempted requests first-in first-out ahead of new admissions. Each of its
steps becomes one prefill over every prompt chunk, then either the pipelined
single-sequence decode or one batched decode over every decoding row. [Inside
the engine](llm/inside.md) is its page, and the [walkthrough](walkthrough.md)
draws twenty of its steps.
