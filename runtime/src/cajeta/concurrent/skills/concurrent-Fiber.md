---
id: concurrent-Fiber
applies-to: [cajeta/concurrent/Fiber]
title: Fiber — sleep the calling fiber without blocking its carrier
description: Fiber.sleep(Duration) parks the calling fiber on the timer wheel. The carrier thread keeps running other fibers.
---

`Fiber` acts on the fiber that calls it. `Fiber.sleep(d)` parks that fiber on the
runtime's timer wheel for `d` and returns once the deadline passes. The carrier thread
under it is never blocked, so other fibers keep running. On the main thread, which is
not a fiber, the call waits on the runtime's task condition instead.

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

A zero or negative duration returns at once. A polling loop with backoff doubles its
duration between parks, as `Server.drainInflight` and `Tasks.selectReceive` do.
