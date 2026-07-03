# Lock

`cajeta.concurrent.Lock` — the no-data RAII gate over the async-aware lock
intrinsics. `acquire()` returns a `#LockGuard` whose drop releases the lock;
`tryAcquire()` is non-blocking (1 = acquired, 0 = already held) and pairs with
a manual `releaseLock()`. A fiber that hits a held lock parks and yields to its
carrier rather than blocking the OS thread. Use a plain `Lock` to guard state
not bundled into a single value; reach for [Mutex](Mutex.md) when there is one
piece of data to fuse the lock with.

```cajeta
Lock gate = heap Lock();
LockGuard g = gate.acquire();   // held while g is alive; auto-release on drop

Lock gate2 = heap Lock();
if (gate2.tryAcquire() == 1) {  // non-blocking attempt
    gate2.releaseLock();        // manual pairing with tryAcquire
}
```

## Methods

| Signature | |
|---|---|
| `Lock()` ⚑ | Create a fresh, unheld lock |
| `#LockGuard acquire()` | Block until the lock is held, then return a `#LockGuard` that releases it on drop; bind the guard to a scope |
| `int32 tryAcquire()` | Non-blocking acquire: `1` if taken (caller must `releaseLock()`), `0` if already held |
| `void releaseLock()` | Manually release a lock taken via `tryAcquire()`; locks taken via `acquire()` are released by their `LockGuard` |

⚑ = `@EntryPoint`

## See also

- Tour: [ConcurrentDemo](../../../samples/tour/src/main/cajeta/tour/concurrent/ConcurrentDemo.cajeta)
- Source: [`runtime/src/cajeta/concurrent/Lock.cajeta`](../../../runtime/src/cajeta/concurrent/Lock.cajeta)
- [Mutex](Mutex.md), [RwLock](RwLock.md), [Semaphore](Semaphore.md)
