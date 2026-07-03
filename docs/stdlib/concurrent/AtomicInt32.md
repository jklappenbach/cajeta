# AtomicInt32

`cajeta.concurrent.AtomicInt32` — atomic 32-bit signed integer: a lock-free
`int32` cell that multiple threads can read, write, and update without tearing
or data races. The class owns a heap-allocated cell that the compiler emits
inline LLVM atomic instructions against. The plain methods (`load`, `store`,
`fetchAdd`, `compareAndSet`) use sequentially consistent ordering; to opt into
a weaker ordering, call the variant whose name encodes it — there is no
ordering parameter. The 64-bit counterpart is [AtomicInt64](AtomicInt64.md).

```cajeta
AtomicInt32 counter = heap AtomicInt32(0);
int32 prior = counter.fetchAdd(1);          // 0; cell now holds 1
boolean won = counter.compareAndSet(1, 2);  // true; cell now holds 2
int32 n = counter.load();                   // 2
```

## Methods

| Signature | |
|---|---|
| `AtomicInt32(int32 initial)` ⚑ | Creates an atomic cell seeded with `initial` |
| `int32 load()` | Atomic load (seq_cst) |
| `void store(int32 v)` | Atomic store (seq_cst) |
| `int32 fetchAdd(int32 delta)` | Atomic fetch-and-add; returns the prior value |
| `boolean compareAndSet(int32 expected, int32 desired)` | Atomic compare-and-set |
| `int32 loadRelaxed()` | Load with `RELAXED` ordering — atomic, no cross-thread synchronization |
| `int32 loadAcquire()` | Load with `ACQUIRE` ordering — pairs with `storeRelease` |
| `void storeRelaxed(int32 v)` | Store with `RELAXED` ordering — no release semantics |
| `void storeRelease(int32 v)` | Store with `RELEASE` ordering — publishes prior writes to a paired `loadAcquire` |
| `int32 fetchAddRelaxed(int32 delta)` | `RELAXED` fetch-and-add; returns the value held before the add |
| `boolean compareAndSetAcquire(int32 expected, int32 desired)` | Compare-and-set with `ACQUIRE` ordering on success and failure |

⚑ = `@EntryPoint`

## See also

- Tour: [ConcurrentDemo](../../../samples/tour/src/main/cajeta/tour/concurrent/ConcurrentDemo.cajeta)
- Source: [`runtime/src/cajeta/concurrent/AtomicInt32.cajeta`](../../../runtime/src/cajeta/concurrent/AtomicInt32.cajeta)
- [AtomicInt64](AtomicInt64.md), [Mutex](Mutex.md)
