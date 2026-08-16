---
id: xpu-execution
applies-to: [cajeta/gpu/GpuStream, cajeta/gpu/Event, cajeta/gpu/Fence]
title: GPU host-side execution & synchronization (GpuStream, Event, Fence)
description: Order GPU work on a GpuStream and synchronize it — device-side cross-stream waits via Event, host-observable signals via Fence, plus the launch-borrow released at sync().
---

# GPU execution & synchronization

These three classes are the host-side control plane for GPU work. Pick by *who waits*:

- **GpuStream** — an ordered queue. Submit launches/async copies to it; they complete
  in submission order. Different streams run in parallel. This is your starting point.
- **Event** — a *device-side* dependency. One stream records an `Event`; another stream
  `waitFor`s it. The GPU resolves the wait with **no host roundtrip**. Use to order work
  *across streams* without stalling the CPU.
- **Fence** — a *host-observable* signal. The CPU polls (`query()`) or blocks
  (`waitHost()`) on it. Use when the **host** needs to know GPU work finished.

Decision: cross-stream ordering on the device → **Event**; CPU needs to observe
completion → **Fence**; just "drain this one stream now" → `GpuStream.sync()`.

## Members and roles

| Class | Role | Who unblocks |
|-------|------|--------------|
| `GpuStream` | in-order submission queue; borrow-scope anchor for launched buffers | n/a (the queue itself) |
| `Event` | marks a point in a stream's order; other streams wait on it device-side | another GpuStream via `waitFor`, or host via `waitHost`/`query` |
| `Fence` | host-facing twin of Event; signaled after queued work completes | the host via `waitHost`/`query` |

## How they collaborate

```
import cajeta.xpu.GpuStream;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.Event;

// Two independent streams; producer must finish before consumer reads.
GpuStream producer #= GpuStream.create();   // owned handle — must destroy()
GpuStream consumer #= GpuStream.create();
Event ready #= Event.create();              // owned handle — must destroy()

KernelBuffer<float32> x = heap KernelBuffer<float32>(n);   // device mem, RAII-freed
x.uploadAsync(hx, producer);
produceKernel.launch(producer, n)(x, ...);   // borrows x until producer.sync()

ready.recordOn(producer);                    // mark producer's current tail
consumer.waitFor(ready);                     // consumer stalls (device-side) for it
consumeKernel.launch(consumer, n)(x, ...);

consumer.sync();                             // host blocks; releases launch-borrows
                                             // on BOTH streams ordered before here
ready.destroy();
producer.destroy();
consumer.destroy();
// x's device memory freed when it leaves scope
```

Object graph: streams and events/fences are **independent owned handles** — neither owns
the other. An `Event`/`Fence` references a stream only transiently inside
`recordOn(stream)` / `signal(stream)` / `waitFor(event)`; those calls pass the peer's
`int64 handle` and retain nothing. There is no parent/child lifetime relationship, so
destroy order between a stream and its events is unconstrained.

## The cross-class call sequence

1. `Event.create()` (or `Fence.create()`) — owned handle.
2. submit work to stream A (async copy / `kernel.launch(streamA, ...)`).
3. `event.recordOn(streamA)` — pins the event to A's current tail.
4. either `streamB.waitFor(event)` (device-side, cross-stream) **or**
   `fence.signal(streamA)` then host `fence.waitHost()` / `fence.query()`.
5. `streamX.sync()` when the host must block and release borrows.
6. `destroy()` every created stream/event/fence.

`Event` is reusable: calling `recordOn(stream)` again overwrites the recording point with
the new tail — do **not** create a fresh Event per launch.

## Ownership & lifecycle (the part that crashes you)

- **`create()` returns `#GpuStream` / `#Event` / `#Fence` — an owned, moved-out value.**
  The caller owns it and **must call `destroy()`** when done. None of these are freed by
  a drop chain — there is no destructor; forgetting `destroy()` leaks the backend object.
- **`GpuStream.current()` returns `#GpuStream`** wrapping the per-thread default stream
  (handle 0). Safe to retain across launches. `destroy()` on it is a **no-op** (handle 0
  is not yours to free), and `sync()` on it **drains the whole context**, not just one
  queue.
- `destroy()` is **idempotent** (runtime null-guard) — a second call, or destroy of the
  default stream, no-ops.
- `recordOn`, `waitFor`, `signal`, `waitHost`, `query`, `sync` take their peer
  **by borrow** — no ownership transfer, nothing retained after the call.

### The launch-borrow released at sync()

A `GpuStream` is the borrow-scope anchor for launched buffers. A kernel launch
**borrows each `KernelBuffer` argument until the next `sync()` ordered after that launch on
that stream** (and async copies likewise). Letting a borrowed buffer reach its drop or an
explicit `free()` before `sync()` is a **compile error (XPU-K02)** — not a runtime fault.
So: call `stream.sync()` (which releases the deferred-borrow tokens) before the buffer
goes out of scope. See `cajeta/gpu/KernelBuffer`.

## What these do NOT do

- **No async submit helper / no destructor.** `sync()`, `waitHost()`, and `waitFor` are
  the only blocking primitives; there is no callback/futures API and no drop-on-scope —
  you call `destroy()` yourself.
- **`Event.waitHost()` is not the host-fast path.** It blocks the CPU on a device event;
  prefer cross-stream `GpuStream.waitFor(event)` for ordering, and use `Fence` when the
  host genuinely needs the signal. The doc comment says "use sparingly."
- **No error return / no exceptions surfaced here.** These methods return `void`/`boolean`
  (`query()` only); they wrap `@Native("__cajeta_xpu_*")` primitives and carry no
  Optional or sentinel. Kernel-launch errors surface elsewhere (see
  `cajeta/gpu/KernelError`).
- **Spelling quirks (Cajeta keywords).** The default stream is `GpuStream.current()`
  (spec's `default` is reserved); `Event.recordOn(stream)` (spec's `record` is reserved).
- On the synchronous CPU/Vulkan paths an `Event` is an always-signaled sentinel and
  `Fence` lowers onto `GpuStream.sync()`, so `query()` may return true immediately —
  don't rely on observing an un-signaled state.

For device memory and the launch idiom see `cajeta/gpu/KernelBuffer`; for the package map see
the `cajeta.xpu` library/package skills.
