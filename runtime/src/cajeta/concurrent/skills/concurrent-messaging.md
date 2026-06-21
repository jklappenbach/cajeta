---
id: concurrent-messaging
applies-to: [cajeta/concurrent/Channel, cajeta/concurrent/SelectResult, cajeta/concurrent/AsyncIterator, cajeta/concurrent/Tasks.selectReceive]
title: Inter-fiber messaging — Channel, selectReceive, and the AsyncIterator drain
description: How Channel<T> feeds Tasks.selectReceive to multiplex receives, and Channel's relationship to the AsyncIterator drain loop.
---

# Inter-fiber messaging

Passing values between fibers. Route by what you need:

- **One producer → one consumer, backpressured** → `Channel<T>` directly: `send`/`receive`.
- **Drain a channel (or any async source) to exhaustion** → the `AsyncIterator` loop: `receive()`/`next()` returning `Optional<T>` until empty.
- **Wait on several channels at once, take whichever fires first** → `Tasks.selectReceive<T>(Channel<T>[])`, which reports the winner as `Optional<SelectResult<T>>`.

These are fiber-blocking primitives — call them from inside a fiber context (an `async` method body, a `spawn`ed task, or a `Tasks.runBlocking` body). Blocking parks the fiber and frees the carrier; it does not block the OS thread.

## Members and roles

- **`Channel<T>`** — bounded MPMC ring-buffer queue. The transport. `send` blocks when full, `receive` blocks when empty. `close()` is one-way and idempotent.
- **`SelectResult<T>`** — a plain `(int32 index, T value)` pair. The result carrier from `selectReceive`: `index` is the position in the input array of the channel that won; `value` is the dequeued item. Constructed by `selectReceive`; you read its public fields, you don't build it.
- **`AsyncIterator<T>`** — interface with one method, `Optional<T> next()`. The drain protocol. `Channel.receive()` already matches this shape (present until closed+drained, empty after).
- **`Tasks.selectReceive<T>`** — the multiplexer. Static method over a `Channel<T>[]`.

## How they cooperate

`selectReceive` does NOT use `receive()` — it is built on the **non-blocking** pair `tryReceive()` + `isClosed()`. Each poll pass it calls `tryReceive()` on every channel; the first present result wins (lowest index on a tie — deterministic in v1). If every channel returns empty but at least one is still open, the fiber sleeps with exponential backoff (100us → 10ms cap) and re-polls. Only when every channel is **closed AND drained** does it return an empty `Optional`. So:

- present `Optional<SelectResult<T>>` → a value was taken from `chans[result.index]`.
- empty `Optional` → all channels terminal. This is the loop's exit condition; there is no sentinel `index == -1`.

## Ownership & lifecycle

- `Channel<T>` is `heap`-allocated; the owning scope drops it. Its destructor releases the lock/condvar intrinsics but **does NOT drain or free buffered items** — for a heap-class `T`, `receive()` in a loop until empty before drop or you leak the elements (the ring-buffer array frees, not its contents). v1 targets value/primitive `T`.
- `send(item)` / `receive()` move the item through the buffer (`#` transfer internally); `receive()` returns a **stack** `Optional<T>` (no per-item heap allocation).
- `selectReceive` heap-allocates each winning `SelectResult<T>` and hands ownership to the caller via the returned `Optional`; the caller's local owns it under normal scope drop.
- `Channel<T>` is **not** auto-bound to `AsyncIterator<T>` yet (the `implements` clause is pending an M5 ripple). You drain a channel with its own `receive()` loop; bind to an `AsyncIterator<int32>` local only for types that declare the interface.

## When to use which

- Use **`receive()`** when you own the consume loop for one channel and want to block.
- Use **`tryReceive()` + `isClosed()`** when you must poll without parking (you're building your own select-like loop).
- Use **`selectReceive`** to wait across many channels — don't hand-roll the backoff.

## Worked example — multiplex two channels

```cajeta
import cajeta.lang.Optional;
import cajeta.concurrent.Channel;
import cajeta.concurrent.Tasks;
import cajeta.concurrent.SelectResult;

public final class D {
    public static async int32 publishOnB(Channel<int32> b) {
        b.send(42);
        return 0;
    }
    public static int32 run() {
        Channel<int32> a = heap Channel<int32>(2);
        Channel<int32> b = heap Channel<int32>(2);
        Channel<int32>[] chs = heap Channel<int32>[2];
        chs[0] = a;
        chs[1] = b;

        spawn publishOnB(b);                                  // produces on b only
        Optional<SelectResult<int32>> opt = Tasks.selectReceive<int32>(chs);
        a.close();
        b.close();

        if (opt.isPresent()) {
            SelectResult<int32> r = opt.get();
            return 1000 * (r.index + 1) + r.value;           // -> 2042 (index 1, value 42)
        }
        return -1;                                            // every channel closed and drained
    }
}
```

## Worked example — drain a channel (AsyncIterator pattern)

`Channel.receive()` is the `next()` shape, so the v1 drain loop reads:

```cajeta
import cajeta.lang.Optional;
import cajeta.concurrent.Channel;

Optional<int32> opt = ch.receive();
while (opt.isPresent()) {
    int32 x = opt.get();
    // ... handle x ...
    opt = ch.receive();
}
// loop exits once the channel is closed AND drained
```

After the first empty result the source stays empty (terminal). There is no `for (x in ch)` desugaring yet — write the explicit `while (opt.isPresent())` loop.

## Gotchas

- `send` to a closed channel **raises** (do not send after `close()`). `close()` itself is safe to call repeatedly and wakes all blocked senders/receivers to re-check.
- `isClosed() == true` does **not** mean drained — a closed channel can still hold buffered items. To detect terminal state per channel, combine a `tryReceive()` that returned empty with `isClosed()`. `receive()` already folds this in (it drains buffered items even after close).
- `selectReceive` busy-tolerates but does not spin hard: a quiescent select wakes ~100x/sec under the backoff cap, trading latency for CPU. It never holds the carrier while sleeping.
