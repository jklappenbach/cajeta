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
KernelStream s = KernelStream.current();   // per-thread default stream
KernelStream fresh = KernelStream.create();
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

⚑ = `@EntryPoint`

## See also

- Tour: [XpuTour](../../../samples/tour/xpu/src/tour/xpu/XpuTour.cajeta)
- [KernelBuffer](KernelBuffer.md) — the buffers whose launch borrows `sync()` releases
- [Device](Device.md) — host-side capability queries
- Source: [`runtime/src/cajeta/xpu/KernelStream.cajeta`](../../../runtime/src/cajeta/xpu/KernelStream.cajeta)
