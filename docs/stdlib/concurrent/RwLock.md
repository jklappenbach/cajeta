# RwLock\<T\>

`cajeta.concurrent.RwLock` — read-heavy shared state. Like [Mutex](Mutex.md)
it owns the protected value, but distinguishes shared reads from exclusive
writes: many readers may hold the read lock at once, a writer holds it
exclusively, and writer-preference in the runtime keeps a steady stream of
readers from starving a writer. Same closure/scoped shape as `Mutex` — the
protected value doesn't escape a write region. Reach for it over `Mutex` only
when reads vastly dominate writes.

```cajeta
RwLock<int32> hits = heap RwLock<int32>(5);
hits.withWrite((int32 v) -> v + 9);   // exclusive write lock, store v + 9
int32 snapshot = hits.read();         // shared read lock, returns a copy -> 14
```

## Methods

| Signature | |
|---|---|
| `RwLock(#T initial)` ⚑ | Wrap `initial` in a fresh lock, taking ownership of it (`#T`) |
| `T read()` | Snapshot the protected value under a shared read lock; intended for value/primitive `T` |
| `void withWrite((T) -> #T fn)` | Exclusive write critical section: acquire the write lock, pass the current value to `fn`, store its result, release |

⚑ = `@EntryPoint`

## See also

- Tour: [ConcurrentDemo](../../../samples/tour/src/main/cajeta/tour/concurrent/ConcurrentDemo.cajeta)
- Source: [`runtime/src/cajeta/concurrent/RwLock.cajeta`](../../../runtime/src/cajeta/concurrent/RwLock.cajeta)
- [Mutex](Mutex.md), [Lock](Lock.md)
