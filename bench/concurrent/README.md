# cajeta.concurrent

Atomics, locks, channels and fibers. Fibers are stackful, so the spawn, footprint and wake rows are read against goroutines and virtual threads.

| Page | |
|---|---|
| [AtomicInt64](AtomicInt64.md) | The lock-free atomic int64 cell |
| [AtomicInt32](AtomicInt32.md) | The lock-free atomic int32 cell |
| [Mutex](Mutex.md) | Fused mutual exclusion and the data it protects |
| [RwLock](RwLock.md) | The reader-writer lock for read-heavy shared state |
| [Lock](Lock.md) | The no-data RAII gate |
| [Semaphore](Semaphore.md) | The counting permit pool |
| [Channel](Channel.md) | The bounded multi-producer multi-consumer queue, measured with the crossbeam-channel harness shapes: 5M messages, four threads |
| [Fiber](Fiber.md) | Sleeping the calling fiber |
| [FiberLocal](FiberLocal.md) | Ambient per-request state |
| [Tasks](Tasks.md) | Task utilities: spawn and await |

## Standing

Written after the first published run.
