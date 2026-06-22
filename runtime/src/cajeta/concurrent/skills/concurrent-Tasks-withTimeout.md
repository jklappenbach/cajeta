---
id: concurrent-Tasks-withTimeout
applies-to: [cajeta/concurrent/Tasks.withTimeout]
title: Tasks.withTimeout — deadline-bounded await with cooperative cancellation
description: Await an already-spawned Task<R> with a timeout; present Optional iff it finished in time, else cancel-and-drain.
---

# `Tasks.withTimeout<R>(Duration d, Task<R> t) -> Optional<R>`

Wait on an **already-spawned** task until `d` elapses. The return is the whole
protocol: the `Optional<R>` is **present iff the body's done flag flipped before the
deadline** (`r.get()` holds the result); **empty on timeout**. On timeout the body is
signalled for cooperative cancellation and drained before returning. You do not spawn
the task here — `withTimeout` layers a deadline over the existing await.

```cajeta
import cajeta.lang.Optional;
import cajeta.time.Duration;
import cajeta.concurrent.Tasks;

Task<int32> t = spawn compute();
Optional<int32> r = Tasks.withTimeout(Duration.ofMillis(50), t);
if (r.isPresent()) { use(r.get()); }   // finished in time
else { handleTimeout(); }              // timed out (body cancelled + drained)
```

`R` is inferred from the `Task<R>` argument. Prefer this generic form;
`withTimeoutInt32(Duration, Task<int32>)` is a legacy specialization kept only for
callers already pinned to it.

## Parameters & ownership
- `d` — relative timeout; the absolute deadline is computed as
  `Cajeta.currentTimeNanos() + d.toNanos()`. For an absolute instant you already hold,
  use the sibling `withDeadline(int64 deadlineNanos, Task<R> t)` —
  `withTimeout(d, t)` ≡ `withDeadline(now + d, t)`.
- `t` — **not** consumed: the surrounding scope keeps ownership of the task. The
  caller's scope drop still waits on the task's done flag before freeing it.
- Return — `Optional<R>` by value (sret/NRVO), constructed directly into the caller's
  slot; the caller's local owns it under normal scope-exit drop. On the present path the
  result moves in (`#val`); on the empty path the value slot is unused (`null`, i.e.
  zero for primitive `R`, null ptr for class `R`).

## The cooperative-cancellation protocol (the load-bearing part)
On timeout, `withTimeout` calls `Cajeta.taskCancel(t)` (sets `cancel_with` on the
task's fiber), then `await t` inside a `try/catch`:

- The body sees the cancellation **only at its next yield point** (`await`, channel
  receive, `Lock.acquire`, etc.), where the runtime throws the cancellation sentinel
  inside the body's frame, short-circuiting it.
- A body with **no yield points runs to natural completion** — v1 does **not** preempt
  CPU loops (the underlying `Cajeta.taskWaitTimeout` waits without preempting). So for
  the timeout to actually shorten the wall-clock wait, the body must contain at least
  one yield point. A pure CPU body will still make `withTimeout` block until it finishes,
  then report empty because the deadline already passed.

## Failure modes / what you observe
- The cancellation sentinel thrown into the drained body is **caught and consumed**
  inside `withTimeout` (the runtime clears `task->exception` on re-raise so the
  enclosing scope drop won't re-propagate it). **Empty `Optional` is the only
  observable signal of a timeout** — you never see the sentinel.
- If the body raises its own exception after cancel, the value is not safely readable;
  the empty path returns with an unused slot. Either way `r.isPresent()` is the contract.

## Side effects
Mutates the task's fiber (`cancel_with`) on the timeout path; always awaits/drains `t`
to settle it before returning. Does not free `t`.

See the class skill (`cajeta/concurrent/Tasks`) for `runBlocking`, `selectReceive`, and
the `withDeadline` sibling.
