---
id: concurrent-Tasks
applies-to: [cajeta/concurrent/Tasks]
title: Tasks — timeout, deadline, runBlocking, and select utilities
description: Static entry point routing among withTimeout/withDeadline (cooperative-cancel Optional<R>), runBlocking sync->async bridge, and selectReceive.
---

`Tasks` is a static utility class (no instances — call everything as `Tasks.foo<...>(...)`). It is a thin layer over the runtime task primitives; you bring the `Task<R>`/lambda, it adds timeout, deadline, blocking-wait, or channel-select semantics on top.

## Pick the method

| Want to… | Call | Returns |
| --- | --- | --- |
| Bound an already-spawned task by a duration | `withTimeout<R>(Duration d, Task<R> t)` | `Optional<R>` — present iff body finished in time |
| Bound a task by an absolute instant (shared budget across fan-out) | `withDeadline<R>(int64 deadlineNanos, Task<R> t)` | `Optional<R>` |
| Same as withTimeout but pinned to `int32` (legacy) | `withTimeoutInt32(Duration d, Task<int32> t)` | `Optional<int32>` |
| Drive async work from a non-async caller (block until done) | `runBlocking<R>(() -> R body)` | `R` (the value) |
| Receive from whichever of N channels is ready first | `selectReceive<T>(Channel<T>[] channels)` | `Optional<SelectResult<T>>` |

What this class does **not** do: it does not spawn the timed task for you — you `spawn` it first and pass the `Task<R>`. There is no `withTimeout(d, () -> ...)` overload that both spawns and times (wrap the call in a lambda, `spawn` it, then pass the `Task`). `runBlocking` supports primitive/value `R` only — heap-class returns (`() -> #R`) are not yet wired; use `await spawn body()` for those. `selectReceive` is receive-only (no select-on-send).

## Cooperative cancellation (withTimeout / withDeadline / withTimeoutInt32)

On timeout these return an **empty** `Optional` and signal the body for cooperative cancellation via the runtime (`Cajeta.taskCancel`). The body observes the cancel at its **next yield point** (`await`, channel receive, `Lock.acquire`, etc.) where the runtime throws a sentinel; `Tasks` awaits and **consumes** that exception internally, so the empty `Optional` is the only observable signal — nothing propagates to your scope.

Critical gotcha: a body with **no yield points** (a pure CPU loop) is **not interruptible** in v1 — it runs to natural completion before the empty `Optional` returns. Timeout shortens the wall-clock wait only if the body yields. `withTimeout(d, t)` is exactly `withDeadline(now + d, t)`.

Ownership/lifecycle: the **surrounding scope still owns the `Task` `t`** — `Tasks` borrows it. The returned `Optional<R>` is constructed by value into the caller's slot (sret/NRVO) and is owned by the receiving local under normal scope-exit drop. On the timeout path the result slot is empty (`null`/zero) — never read `.get()` without checking `.isPresent()`. The scope drop of `t` itself waits on the task's done flag before freeing.

## Example — withTimeout

```cajeta
import cajeta.concurrent.Tasks;
import cajeta.lang.Optional;
import cajeta.time.Duration;

Task<int32> t = spawn compute();
Optional<int32> r = Tasks.withTimeout(Duration.ofMillis(50), t);
if (r.isPresent()) { use(r.get()); }
else { handleTimeout(); }   // body was signalled for cancellation and drained
```

`withDeadline` is identical but takes `int64 deadlineNanos` (e.g. `Cajeta.currentTimeNanos() + Duration.ofMillis(50).toNanos()`), useful when several fan-out sub-operations share one absolute budget.

## Example — runBlocking (sync -> async bridge)

```cajeta
import cajeta.concurrent.Tasks;

() -> int32 body = () -> { return 42; };
int32 result = Tasks.runBlocking<int32>(body);
```

Spawns `body` on the carrier pool and blocks the calling thread until it settles, returning the value. The caller need **not** be `async` — with no current fiber the runtime falls through to a condvar wait — so a plain non-async `main` can drive async work. The body is a real fiber context: nested `await spawn ...` inside it parks/wakes normally.

## Example — selectReceive

```cajeta
import cajeta.concurrent.Tasks;
import cajeta.concurrent.Channel;
import cajeta.concurrent.SelectResult;
import cajeta.lang.Optional;

Channel<int32>[] chs = heap Channel<int32>[2];
Optional<SelectResult<int32>> opt = Tasks.selectReceive<int32>(chs);
if (opt.isPresent()) {
    SelectResult<int32> hit = opt.get();   // hit.index, hit.value
} else {
    // every channel was closed AND drained
}
```

Returns **present** wrapping a `SelectResult<T>` (`index` = zero-based position of the winning channel, `value` = the item; lowest index wins on ties — deterministic in v1). Returns **empty** only when all channels are closed and drained. Implementation is poll-and-backoff over `tryReceive()`/`isClosed()` with exponential sleep (100us -> 10ms cap) via `Cajeta.fiberSleepNanos`; it parks on the timer wheel, never pinning the carrier. The `SelectResult<T>` is heap-allocated and handed to you inside the `Optional` (owned by the receiving local). `SelectResult` fields and `Channel` mechanics live in their own skills (`cajeta/concurrent/SelectResult`, `cajeta/concurrent/Channel`).
