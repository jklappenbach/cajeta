# Tasks

`cajeta.concurrent.Tasks` — static task utilities layered on the fiber
scheduler. `withTimeout` waits on an already-spawned `Task<R>` with a deadline,
reporting completion vs. timeout via `Optional<R>`; on timeout the body is
signalled for cooperative cancellation and drained (bodies need at least one
yield point — await, channel receive, lock acquire — for cancellation to
shorten the wait; pure CPU loops run to completion). `runBlocking` is the
sync-to-async bridge, and `selectReceive` is Go's `select` over an array of
channels.

```cajeta
public static async int32 compute() {
    return 42;
}

public static void demo() {
    Task<int32> t = spawn compute();
    Duration budget = Duration.ofMillis(50L);
    Optional<int32> r = Tasks.withTimeout(budget, t);
    boolean finished = r.isPresent();
}
```

## Methods

| Signature | |
|---|---|
| `static Optional<int32> withTimeoutInt32(Duration d, Task<int32> t)` | `int32` specialization kept for callers already pinned to it; new code should prefer the templated form |
| `static Optional<R> withTimeout<R>(Duration d, Task<R> t)` ⚑ | Canonical timeout API: wait on an already-spawned `t` until `d` elapses; present with the result, or empty on timeout (after cooperative cancellation) |
| `static Optional<R> withDeadline<R>(int64 deadlineNanos, Task<R> t)` | Absolute-instant sibling of `withTimeout`: `withTimeout(d, t)` is `withDeadline(now + d, t)` |
| `static R runBlocking<R>(() -> R body)` | Sync→async bridge: spawn `body` on the carrier pool, block the calling thread until it completes, return the value |
| `static Optional<SelectResult<T>> selectReceive<T>(Channel<T>[] channels)` ⚑ | Multiplexed receive: present with the lowest-index ready channel's value, empty once every channel is closed and drained |

⚑ = `@EntryPoint`

## See also

- Tour: [AsyncDemo](../../../samples/tour/src/main/cajeta/tour/concurrent/AsyncDemo.cajeta)
- Source: [`runtime/src/cajeta/concurrent/Tasks.cajeta`](../../../runtime/src/cajeta/concurrent/Tasks.cajeta)
- [Channel](Channel.md), [Duration](../time/Duration.md), [Optional](../lang/Optional.md)
