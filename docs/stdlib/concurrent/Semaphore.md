# Semaphore

`cajeta.concurrent.Semaphore` — counting permit pool that bounds the number of
concurrent operations against an unbounded set of consumers. Built on the lock
+ condition-variable intrinsics: `acquire` waits for a free permit then takes
one, `release` returns one and wakes a waiter. If a workload is statically
batchable into "K at a time", a chunked `scope { spawn K times }` expresses the
limit in the program's shape; reach for `Semaphore` when consumers are
unbounded and no scope can serve as the batching boundary.

```cajeta
Semaphore gate = heap Semaphore(2);
gate.acquire();
int32 avail = gate.availablePermits();   // 1
gate.release();
```

## Methods

| Signature | |
|---|---|
| `Semaphore(int32 initial)` ⚑ | Create a semaphore seeded with `initial` free permits — the cap on concurrent holders |
| `void acquire()` | Take one permit, blocking (parking the fiber) until one is free |
| `void release()` | Return one permit, waking a waiter so it can re-check |
| `void withPermit(() -> void fn)` | Run `fn` while holding a permit, releasing it even if `fn` throws |
| `int32 availablePermits()` | Current free-permit count (snapshot; advisory under contention) |

⚑ = `@EntryPoint`

## See also

- Tour: [ConcurrentDemo](../../../samples/tour/src/main/cajeta/tour/concurrent/ConcurrentDemo.cajeta)
- Source: [`runtime/src/cajeta/concurrent/Semaphore.cajeta`](../../../runtime/src/cajeta/concurrent/Semaphore.cajeta)
- [Lock](Lock.md), [Mutex](Mutex.md)
