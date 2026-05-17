# `cajeta.thread` — Fibers + OS threads

Full design lives in `cajeta-docs/ThreadModel.md`. Implementation
status lives in `cajeta-docs/AsyncStatus.md`.

Status: **fiber runtime substantially shipped** (R1-R5-A' per
AsyncStatus.md). Class wrappers below are stubs around the runtime
helpers.

## `Fiber` — cooperative concurrency

```cajeta
public final class Fiber {
    // Cooperative sleep. Parks the calling fiber on the timer wheel
    // for at least `millis`; the carrier runs other fibers in the
    // meantime. Wakeup is best-effort — actual delay >= requested.
    public static void sleep(int64 millis);

    // Yield the carrier to other ready fibers without blocking.
    public static void yield();

    public static Fiber current();
    public int64 id();
}
```

The runtime backing (stackful fibers, work-stealing scheduler,
timer wheel, async I/O reactor) is shipped per AsyncStatus.md
R1-R5-A'.

### Example

```cajeta
import cajeta.thread.Fiber;

spawn task1();
spawn task2();
Fiber.sleep(100);    // yields the carrier; other fibers run
```

## `Thread` — OS-thread fallback

```cajeta
public final class Thread {
    // Blocking sleep on the OS thread. Used outside fiber context
    // (the harness's main / control thread). Inside a fiber, prefer
    // Fiber.sleep so other fibers can run.
    public static void sleep(int64 millis);
}
```

## `Task<T>` — already shipped

`spawn fn(args)` lowers to a `Task<T>` instance backed by a fresh
fiber. See `cajeta-docs/AsyncStatus.md` and `cajeta-docs/ThreadModel.md`
for the full design.

```cajeta
Task<int32> t = spawn computeSomething();
int32 result = t.await();
```

Pinned by `test/parser/TaskTypingTests.cpp`,
`test/parser/SpawnDropTests.cpp`,
`test/parser/AsyncSyntaxTests.cpp`,
`test/parser/DetachTests.cpp`,
`test/parser/PerFiberDropChainTests.cpp`.

## Locks

Designed in `cajeta-docs/ThreadModel.md` § Synchronization.
Implementation status:

- `Cajeta.lockNew()` / `Cajeta.lockAcquire()` etc. runtime
  intrinsics — shipped (`test/parser/LockIntrinsicTests.cpp`).
- `Lock` cajeta class wrapping the intrinsics — shipped
  (`test/parser/LockClassTests.cpp`).

## Open items

Tracked in Features.md:

- The `Fiber` / `Thread` cajeta-source classes (the runtime is
  shipped; the surface classes haven't been declared yet).
- R5-C / R5-D runtime items (see AsyncStatus.md).
