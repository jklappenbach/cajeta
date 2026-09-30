# KernelStream

`cajeta.xpu.KernelStream` — ordered queue of XPU work: a backend-tagged handle
to an in-order command stream (CUstream / hipStream / VkQueue). Kernel
launches submitted to the same stream complete in submission order; launches
on different streams run in parallel, subject to cross-stream synchronization
via `Event`. The stream is also the borrow-scope anchor for launched buffers:
a buffer passed as a kernel argument is borrowed for the lifetime of the
launch, released at the next `sync()` ordered after it. (The spec's
`KernelStream.default()` is spelled `current()` here — `default` is a Cajeta
keyword.)

```cajeta
KernelStream s #= KernelStream.current();   // per-thread default stream
KernelStream fresh #= KernelStream.create();
// enqueue async copies and launches on `fresh` ...
fresh.sync();      // block until everything submitted has completed
fresh.destroy();
```

## Methods

| Signature | |
|---|---|
| `static #KernelStream current()` ⚑ | The per-thread default stream (handle 0); always the same stream for the same thread |
| `static #KernelStream create()` ⚑ | Create a fresh stream — a real backend stream object on CUDA/HIP, the default stream elsewhere; the caller owns it and must `destroy()` it |
| `void sync()` | Block until every operation submitted to this stream — async copies and kernel launches — has completed; releases all deferred-borrow tokens |
| `void waitFor(Event e)` | Insert a wait on `e`: future launches on this stream will not start until `e` has been recorded and signaled |
| `void destroy()` | Destroy this stream's backend object; no-op for the default stream, idempotent |
| `void setDeferred(boolean on)` | Queue this stream's launches and submit them together at the next point their results can be observed (see below) |
| `void flush()` | Submit every launch this stream has queued, without waiting for them |
| `int64 pendingLaunches()` | Launches queued on this stream and not yet submitted |
| `static int64 deferStat(int32 which)` | Process-wide deferral counters by `DEFER_*` key: `DEFER_QUEUED`, `DEFER_REPLAYED`, `DEFER_DIRECT`, `DEFER_GRAPHS_BUILT`, `DEFER_GRAPH_LAUNCHES`, `DEFER_PATCHES`, `DEFER_FLUSHES` |

## Deferred launches

A launch is a driver call, and on the measured box (an RTX 4090 under WSL2)
that call costs 6 to 13 microseconds whatever the kernel does. A decode token
of an 8B model is 356 launches, so the floor alone is milliseconds a token,
and the sequence of kernels is the same every token. `setDeferred(true)`
makes the stream QUEUE its launches, arguments copied by value, and submit
the queue at the next point where its results can be observed: a `sync()`, a
host transfer, an event record, a buffer release, `flush()`, or turning
deferral off. Nothing a program can observe changes; what changes is the
cost of submitting.

A queue whose kernel sequence the stream has submitted before is replayed as
one recorded submission with only the grids, blocks and arguments that
changed patched in. On nvptx the recording is a CUDA graph: measured on the
same box, 600 kernels replay in 0.6 ms against 5 to 6 ms submitted one by
one, and a patch costs 0.2 microseconds. The first sight of a sequence goes
launch by launch, the second records it. A backend without such a thing
submits the queue launch by launch, and a synchronous backend (cpu) queues
nothing: there the switch is accepted and has no effect.

A launch that cannot be queued (a texture, an image, a buffer array, a
launch-time specialization) submits the stream's queue ahead of itself and
goes directly, so order on the stream is always the order of the launch
calls. An armed device profiler keeps every launch direct, since it
attributes each one as it is submitted; `CAJETA_PROFILER_GPU=0` keeps host
sampling with capture off.

One consequence to know: a queued launch has not started. Host work that
used to overlap the device now precedes it, so defer where the launches are
many and small, which is where the submit cost dominates.

`CAJETA_XPU_DEFER=0` never queues (the A/B control); `CAJETA_XPU_DEFER=all`
defers every stream from its first launch (the whole-suite validation
lever). `CAJETA_XPU_DRIVER_PROFILE=1` prints, at exit, how many times each
driver entry that observes device state was called and how long the host
spent in it, with the direct launches and the deferred submissions beside
them; `CAJETA_XPU_DRIVER_SITES=1` adds the cajeta source line of each call.

⚑ = `@EntryPoint`

## See also

- Tour: [XpuTour](../../../samples/tour/xpu/src/tour/xpu/XpuTour.cajeta)
- [KernelBuffer](KernelBuffer.md) — the buffers whose launch borrows `sync()` releases
- [Device](Device.md) — host-side capability queries
- Source: [`runtime/src/cajeta/xpu/KernelStream.cajeta`](../../../runtime/src/cajeta/xpu/KernelStream.cajeta)
