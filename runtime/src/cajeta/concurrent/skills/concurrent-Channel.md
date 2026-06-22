---
id: concurrent-Channel
applies-to: [cajeta/concurrent/Channel]
title: Channel<T> — bounded MPMC queue (blocking send/receive)
description: Fixed-capacity multi-producer/multi-consumer queue; blocking send/receive, one-way close, drain-after-close semantics, heap-T drain-before-drop caveat.
---

# Channel\<T\>

Entry-point type in `cajeta.concurrent`. A `Channel<T>` is a bounded
multi-producer/multi-consumer (MPMC) queue: a fixed-capacity ring buffer
guarded by the same lock + condition-variable intrinsics `Mutex`/`Semaphore`
use. Use it to hand values between fibers with backpressure — `send` blocks
when the buffer is full, `receive` blocks when it is empty. **You construct
it** (it is not handed to you by a factory).

For fan-in select across several channels, do *not* loop on `receive` (that
blocks on one channel) — use `tryReceive()` + `isClosed()`, or
`cajeta.concurrent.Tasks.selectReceive` (see `cajeta/concurrent/Tasks`).

## Construction & ownership

```cajeta
import cajeta.concurrent.Channel;
import cajeta.lang.Optional;

// Capacity-2 MPMC queue of primitives. A third send() would block.
Channel<int32> ch = heap Channel<int32>(2);
```

`Channel(int32 capacity)` allocates the ring buffer and the lock/condvar
intrinsics. The handle is the heap `Channel` itself; it frees on scope drop,
which destroys the lock and condvar.

## The methods that matter

- `void send(T item)` — enqueue, blocking (parking the fiber) while full.
  **Raises when the channel is closed** (see Errors). For a heap `T`, the
  buffered item is owned by the channel until received.
- `Optional<T> receive()` — dequeue, blocking while empty *and* open. Returns
  a **stack** `Optional<T>` (no per-item heap alloc): present with the item
  while items remain — *including after close, until drained* — and empty
  once the channel is closed AND drained. Extracted value is moved out
  (`#item`); for heap `T` you own it. Inspect with `isPresent()`/`isEmpty()`,
  read with `get()` (see `cajeta/lang/Optional`).
- `Optional<T> tryReceive()` — non-blocking dequeue. Returns empty
  immediately if nothing is currently buffered, *even when still open*. Never
  parks. Building block for select loops.
- `void close()` — one-way, idempotent. Wakes every blocked sender/receiver
  to re-check. Buffered items remain receivable.
- `boolean isClosed()` — snapshot of the closed bit. `true` does **not** imply
  drained.

## Concurrency, lifecycle, and sharp edges

- Fiber/thread-safe: every operation takes the internal lock. Designed for
  multiple producers and consumers on one shared instance.
- `close()` does not drain. After close, drain with `receive()` until empty;
  the terminal signal is an empty `Optional`.
- Distinguishing "transiently empty" from "closed and drained" requires both
  signals: `maybe.isEmpty() && ch.isClosed()` after a `tryReceive()` —
  `tryReceive` alone cannot tell them apart.
- **Drop caveat for heap `T`:** the destructor releases the lock/condvar and
  frees the ring-buffer *array*, but does NOT free elements still buffered.
  Items sent but never received leak when the channel drops. Workaround:
  drain explicitly (`receive()` in a loop until empty) before dropping.
  v1 targets value/primitive `T`; the heap-`T` element-drop refinement is
  future work.

## Errors

`send` on a closed channel raises (current codegen: `throw 1`, an integer —
there is no named exception type yet). Guard producers so the last `send`
cannot race a `close()`. `receive`/`tryReceive`/`close`/`isClosed` never raise.

## Worked example — drain after close

```cajeta
import cajeta.concurrent.Channel;
import cajeta.lang.Optional;

Channel<int32> ch = heap Channel<int32>(4);
ch.send(1);
ch.send(2);
ch.close();                      // one-way; a later send() raises

int32 sum = 0;
Optional<int32> o = ch.receive();
while (o.isPresent()) {          // drains buffered items after close
    sum = sum + o.get();
    o = ch.receive();           // empty once closed AND drained -> loop ends
}
// sum == 3
```
