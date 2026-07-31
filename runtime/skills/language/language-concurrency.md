---
id: language-concurrency
applies-to: [cajeta/language/concurrency, cajeta/language/async, cajeta/language/spawn]
title: Structured concurrency — async, spawn, scope, await, detach
description: Fibers over a carrier pool; scope joins its children at the closing brace; what may cross a spawn boundary (borrows scoped, detach transfer-only) and the bare-call spawn rule.
---

# Structured concurrency — fibers, not threads

`async` methods run as **stackful fibers** on a carrier pool
(`CAJETA_CARRIERS`, default `min(nproc, 4)`). There are no OS threads to
spawn: a blocked primitive parks the fiber and yields its carrier, so
thousands of fibers share a few threads. This is Java 21's virtual-thread
model — **no function coloring**: any function can `await`.

- **`async T f()`** — returns a `Task<T>`. Calling without `await` or `spawn`
  produces an *unstarted* task that does nothing.
- **`scope { ... }`** — owns every task spawned inside; control does not pass
  the closing `}` until all children finish or cancel. Every function body is
  also an implicit scope, so no task leaks.
- **`spawn f(...)`** — launches a child `Task<T>` registered with the
  enclosing scope. **`await t`** suspends until it completes.
- **`detach f(...)`** — fire-and-forget, explicitly outside the scope. Rare.

## The rules that bite

- **`spawn` takes a BARE class-method call.** `spawn score(10)` compiles;
  `spawn ConcurrencyDemo.score(10)` and any instance-method call fail with
  `CAJETA_ERROR_ASYNC_R3A` ("use a bare class-method invocation").
- **Ownership across the boundary**: a `spawn` body may **borrow** — the
  scope's join guarantees the source outlives it. A `detach` body may
  **not**: it can capture only `#`-transferred values, because no scope
  anchors the lifetime.
- **Cancellation is cooperative.** When one child throws, siblings are
  cancelled and observe it at their next yield point (`await`, channel
  receive, `Lock.acquire`) — a pure CPU loop is never preempted. There is no
  `CancellationException`: the *trigger* throwable is re-raised in the
  cancelled fiber.
- **Errors propagate up**: a child's throw is recorded on its `Task` and
  re-raised at `await` / at `scope` exit. A throw inside a `detach`ed task is
  **silently lost** — nothing awaits it.
- **A fiber is pinned to the carrier that first ran it.** Parallelism comes
  from fanning out across distinct spawned fibers, not from migrating one.
  `CAJETA_CARRIERS=1` gives deterministic single-carrier ordering for
  debugging.

## Worked example (verified: returns 54)

```cajeta
package dev.cajeta.skills;

import cajeta.concurrent.Channel;
import cajeta.concurrent.Mutex;
import cajeta.lang.Optional;

public class ConcurrencyDemo {
    public static async int32 score(int32 n) {
        return n * 2;
    }

    public static int32 run() {
        int32 total = 0;
        scope {
            // spawn takes a BARE method call; the scope joins at its closing brace.
            Task<int32> a = spawn score(10);
            Task<int32> b = spawn score(11);
            total = await a + await b;      // 20 + 22
        }

        // Mutex<T> fuses lock and data: reachable only inside withLock.
        Mutex<int32> counter = heap Mutex<int32>(0);
        counter.withLock((v) -> v + 5);
        int32 held = counter.get();          // 5

        // Channel: bounded MPMC queue; receive returns a stack Optional.
        Channel<int32> ch = heap Channel<int32>(4);
        ch.send(7);
        ch.close();
        Optional<int32> got = ch.receive();
        int32 fromCh = 0;
        if (got.isPresent()) { fromCh = got.get(); }

        return total + held + fromCh;        // 54
    }
}
```

## Library surface

Locks, channels, atomics, semaphores, `Tasks`, `FiberLocal` and the rest live
in `cajeta.concurrent` — read `concurrent-overview` for the primitive-picking
table and its invariants (guards release on unwind; `tryAcquire` is the one
primitive with no guard; receives return **stack** `Optional`, never null).
