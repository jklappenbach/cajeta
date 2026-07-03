# AtomicInt64

`cajeta.concurrent.AtomicInt64` — atomic 64-bit signed integer: a lock-free
`int64` cell that multiple threads can read, write, and update without tearing
or data races. Same design as [AtomicInt32](AtomicInt32.md) (a heap cell plus
compiler-emitted inline LLVM atomic instructions); the 64-bit variant matters
for sequence numbers, timestamps, and generation counters. The default
`load`/`store`/`fetchAdd`/`compareAndSet` use sequentially consistent
ordering; the named `*Relaxed`/`*Acquire`/`*Release` variants opt into weaker
guarantees.

```cajeta
AtomicInt64 seq = heap AtomicInt64(1000000000000L);
int64 prior = seq.fetchAdd(1L);   // returns the prior value
int64 n = seq.load();             // 1000000000001
```

## Methods

| Signature | |
|---|---|
| `AtomicInt64(int64 initial)` ⚑ | Creates an atomic cell seeded with `initial` |
| `int64 load()` | Atomically reads the current value (sequentially consistent) |
| `void store(int64 v)` | Atomically overwrites the value (sequentially consistent) |
| `int64 fetchAdd(int64 delta)` | Atomically adds `delta`; returns the value held before the add |
| `boolean compareAndSet(int64 expected, int64 desired)` | Sets `desired` only if the value equals `expected`; `true` on success |
| `int64 loadRelaxed()` | `load` with relaxed ordering: atomicity only, no synchronization |
| `int64 loadAcquire()` | `load` with acquire ordering: pairs with a release store |
| `void storeRelaxed(int64 v)` | `store` with relaxed ordering: atomicity only |
| `void storeRelease(int64 v)` | `store` with release ordering: publishes prior writes to acquiring readers |
| `int64 fetchAddRelaxed(int64 delta)` | `fetchAdd` with relaxed ordering; returns the prior value, wraps on overflow |
| `boolean compareAndSetAcquire(int64 expected, int64 desired)` | `compareAndSet` with acquire ordering on success; `true` if it swapped |

⚑ = `@EntryPoint`

## See also

- Tour: [ConcurrentDemo](../../../samples/tour/src/main/cajeta/tour/concurrent/ConcurrentDemo.cajeta)
- Source: [`runtime/src/cajeta/concurrent/AtomicInt64.cajeta`](../../../runtime/src/cajeta/concurrent/AtomicInt64.cajeta)
- [AtomicInt32](AtomicInt32.md), [Mutex](Mutex.md)
