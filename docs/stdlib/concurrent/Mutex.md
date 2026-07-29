# Mutex\<T\>

`cajeta.concurrent.Mutex` — fused mutual-exclusion + protected data. The mutex
owns a single piece of data `T`, and the only way to touch it is to hold the
lock: access goes through `withLock`, whose closure is the critical section —
the current value is handed in, the closure's result is stored back, and the
lock is released on return or unwind. "Read/write the data without the lock"
is structurally unrepresentable. Acquiring a held lock from a fiber parks the
fiber and yields to its carrier rather than blocking the OS thread.

```cajeta
Mutex<int32> counter = heap Mutex<int32>(0);
counter.withLock((int32 v) -> v + 1);   // increment inside the critical section
counter.withLock((int32 v) -> v + 1);
int32 now = counter.get();              // snapshot under the lock -> 2
```

## Methods

| Signature | |
|---|---|
| `Mutex(#T initial)` ⚑ | Wrap `initial` in a fresh mutex, taking ownership of the value |
| `void withLock((T) -> #T fn)` | Run `fn` as the critical section: acquire, pass the current value, store `fn`'s result, release; exception-safe, wakes `withLockWhen` waiters |
| `void withLockWhen((T) -> boolean cond, (T) -> #T fn)` | Wait-for-condition critical section: block until `cond(value)` holds, then run `fn` and store its result; `cond` must be side-effect-free |
| `T get()` | Snapshot the protected value under the lock; intended for value/primitive `T` |

⚑ = `@EntryPoint`

## See also

- Tour: [ConcurrentDemo](../../../samples/tour/src/main/cajeta/tour/concurrent/ConcurrentDemo.cajeta)
- Source: [`runtime/src/cajeta/concurrent/Mutex.cajeta`](../../../runtime/src/cajeta/concurrent/Mutex.cajeta)
- [Lock](Lock.md) — the no-data gate; [RwLock](RwLock.md) — read-heavy sibling; [AtomicInt32](AtomicInt32.md)
