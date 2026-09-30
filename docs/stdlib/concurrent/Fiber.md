# Fiber

`cajeta.concurrent.Fiber` acts on the fiber that calls it. Its one method,
`sleep`, is the only sleep in the standard library. It parks the calling fiber
on the runtime timer wheel for the given `Duration`. The carrier thread under
the fiber is never blocked, so other fibers keep running while it waits. On the
main thread, which is not a fiber, the call waits on the runtime task condition
instead. A zero or negative duration returns at once.

```cajeta
import cajeta.concurrent.Fiber;
import cajeta.time.Duration;

public final class Poll {
    public static int32 waitFor(int32 tries) {
        int32 i = 0;
        while (i < tries) {
            Fiber.sleep(Duration.ofMillis(5));
            i = i + 1;
        }
        return i;
    }
}
```

A polling loop with backoff doubles its duration between parks. `Server.drainInflight`
and `Tasks.selectReceive` work this way.

## Methods

| Signature | |
|---|---|
| `static void sleep(Duration d)` | Park the calling fiber for `d` without blocking its carrier. A zero or negative `d` returns at once |

## Removed

`Tasks.sleepMillis(int64)` and the compiler intrinsic `Cajeta.fiberSleepNanos(int64)`
are gone, and nothing forwards to them. Write `Fiber.sleep(Duration.ofMillis(n))`
and `Fiber.sleep(Duration.ofNanos(n))` in their place, with
`import cajeta.concurrent.Fiber;` and `import cajeta.time.Duration;`.

## See also

- Source: [`runtime/src/cajeta/concurrent/Fiber.cajeta`](../../../runtime/src/cajeta/concurrent/Fiber.cajeta)
- [Tasks](Tasks.md), [Duration](../time/Duration.md)
