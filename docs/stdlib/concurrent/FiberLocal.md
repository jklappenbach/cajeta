# FiberLocal\<T\>

`cajeta.concurrent.FiberLocal` — ambient per-request state: a fiber-keyed,
scope-restored binding, the sound replacement for a thread-pool
`ThreadLocal`/MDC. Fibers are single-use, so a per-fiber binding has naturally
request-scoped lifetime and cannot leak into a later, unrelated request. A
child spawned inside a `scope` inherits the binding automatically; `T` may be
any type, reference or primitive — the value is boxed before being handed to
the per-fiber runtime store, so a primitive round-trips correctly.

```cajeta
FiberLocal<int64> requestId = heap FiberLocal<int64>();
requestId.where(42L, () -> {
    int64 rid = requestId.orElse(0L);   // 42 anywhere inside the extent
});
boolean bound = requestId.isBound();    // false — binding restored on return
```

## Methods

| Signature | |
|---|---|
| `FiberLocal()` ⚑ | A FiberLocal key; `get()` on an unbound fiber throws — use `orElse`/`isBound` |
| `void where(#T value, () -> void body)` | Bind `value` for the dynamic extent of `body`, restoring the prior binding when `body` returns or throws |
| `T get()` | The current binding; throws when unbound |
| `T orElse(T fallback)` | The current binding, or `fallback` when unbound; never throws |
| `boolean isBound()` | Whether a binding is in effect on the current fiber |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/concurrent/FiberLocal.cajeta`](../../../runtime/src/cajeta/concurrent/FiberLocal.cajeta)
- [Mutex](Mutex.md), [Tasks](Tasks.md)
