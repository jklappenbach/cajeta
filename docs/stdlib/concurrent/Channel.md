# Channel\<T\>

`cajeta.concurrent.Channel` — bounded MPMC queue: a fixed-capacity ring buffer
guarded by the lock + condition-variable intrinsics. `send` blocks when full
(and raises on a closed channel); `receive` blocks when empty while the channel
is open. `close()` is one-way and idempotent. `receive()` returns a stack
`Optional<T>`: present while items remain (it drains buffered items even after
close), empty once the channel is closed and drained. v1 targets
value/primitive `T`; for a heap class `T`, drain explicitly before drop —
the destructor frees the ring buffer array, not its elements.

```cajeta
Channel<int32> ch = heap Channel<int32>(2);
ch.send(1);
ch.send(2);
ch.close();                          // one-way; a later send() raises
Optional<int32> a = ch.receive();    // present -> 1 (drains after close)
Optional<int32> b = ch.receive();    // present -> 2
Optional<int32> end = ch.receive();  // empty -> closed and drained
```

## Methods

| Signature | |
|---|---|
| `Channel(int32 capacity)` ⚑ | Create an open channel whose ring buffer holds at most `capacity` items before `send` blocks |
| `void send(T item)` | Enqueue `item`, blocking (parking the fiber) while the buffer is full; raises if closed |
| `Optional<T> receive()` | Dequeue the next item, blocking while empty and open; empty `Optional` once closed and drained |
| `void close()` | One-way close; wakes every blocked sender/receiver to re-check |
| `Optional<T> tryReceive()` | Non-blocking dequeue; empty `Optional` when nothing is buffered, even if still open |
| `boolean isClosed()` | Snapshot of the closed bit; `true` does not imply the buffer is drained |

⚑ = `@EntryPoint`

## See also

- Tour: [AsyncDemo](../../../samples/tour/src/main/cajeta/tour/concurrent/AsyncDemo.cajeta),
  [ConcurrentDemo](../../../samples/tour/src/main/cajeta/tour/concurrent/ConcurrentDemo.cajeta)
- Source: [`runtime/src/cajeta/concurrent/Channel.cajeta`](../../../runtime/src/cajeta/concurrent/Channel.cajeta)
- [Tasks](Tasks.md) — `selectReceive` multiplexes across channels; [Optional](../lang/Optional.md)
