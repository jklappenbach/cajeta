# 16 — Concurrency

This chapter defines the concurrency model: `async` functions running as fibers, structured `scope` blocks that own the tasks spawned inside them, `spawn`, the ownership rules that make data races unrepresentable by construction, and the synchronization primitives whose critical sections are closures. There is no `synchronized`, no `volatile`, and no `ThreadLocal` (§16.7).

## 16.1 `async` Functions and `await`

`async` is a method modifier. An `async` method may suspend, and runs as a fiber on the runtime's carrier pool. `await expr` on an async call suspends the awaiting fiber until the result is ready and yields the value. An exception in the awaited task re-raises in the awaiter's frame.

Ordinary (non-`async`) methods never suspend at the language level — when one blocks on a synchronization primitive (§16.5), the runtime parks the calling fiber under the hood, and the method's signature stays synchronous.

## 16.2 `scope` — Structured Concurrency

`scope { … }` is a block that owns the lifetime of every task spawned inside it: control does not leave the block until every child has finished or been cancelled. Joining is implicit at the closing brace.

**Example 16.2-1.** Spawned children joined by scope exit, and a guarded counter.

```cajeta
import cajeta.concurrent.Mutex;
public final class C {
    public static async int32 slow(int32 v) { return v + 1; }
    public static int32 run() {
        scope {
            spawn slow(1);
            spawn slow(2);
        }                                  // joins both children
        Mutex<int32> counter = heap Mutex<int32>(0);
        counter.withLock((n) -> n + 1);
        counter.withLock((n) -> n + 1);
        counter.withLock((n) -> { System.stdout.println("count " + n); return n; });
        return await slow(21);
    }
}
System.stdout.println(C.run());            // count 2, then 22
```

## 16.3 `spawn` and `detach`

`spawn call` starts an async call as a child task of the enclosing `scope`, and the scope joins it at the closing brace. The expression yields a `Task<T>`. Binding it gives a handle to `await` for the result, and leaving it unbound hands the join to the scope.

A value borrowed into a spawned task is sound. The scope does not exit until the task finishes, so the borrow's source outlives every use (§16.4).

**Example 16.3-1.** Two children joined at the closing brace. The counter is borrowed into both.

```cajeta
import cajeta.concurrent.Mutex;
public final class C {
    public static async void bump(Mutex<int32> m) {
        m.withLock((n) -> n + 1);
        return;
    }
    public static int32 run() {
        Mutex<int32> counter = heap Mutex<int32>(0);
        scope {
            spawn bump(counter);        // borrowed into a scoped task
            spawn bump(counter);
        }                               // joins both children
        return counter.get();           // 2
    }
}
```

**Example 16.3-2.** A spawned call bound to a `Task<T>` and awaited for its result.

```cajeta
public final class C {
    public static async int32 work(int32 v) { return v + 1; }
    public static int32 run() {
        scope {
            Task<int32> a = spawn work(1);
            Task<int32> b = spawn work(20);
            return await a + await b;   // 23
        }
    }
}
```

`detach call` starts a task bound to no scope. A detached task can outlive the frame that started it, so it must own everything it touches (§16.4). A class-typed argument must be transferred. A fresh `heap T(...)` argument promotes implicitly with no `#` written (Ownership §5.3), and a primitive argument is copied and carries no obligation.

**Example 16.3-3.** A detached task takes the title, and drops the value when it finishes.

```cajeta
public class Payload {
    public int32 n;
    public Payload() { this.n = 0; }
}
public final class C {
    public static async void consume(Payload p) { return; }
    public static int32 run() {
        Payload p = heap Payload();
        detach consume(#p);             // the task owns p
        return 1;
    }
}
```

**Example 16.3-4.** A rejected program. A borrow into a detached task would dangle when the frame's local drops.

<!-- snippet: skip -->
```cajeta
public class Payload {
    public int32 n;
    public Payload() { this.n = 0; }
}
public final class C {
    public static async void consume(Payload p) { return; }
    public static int32 run() {
        Payload p = heap Payload();
        detach consume(p);              // CAJETA_ERROR_DETACH_BORROW_CAPTURE
        return 1;
    }
}
```

> *Discussion.* As of 0.27.0 `spawn` accepts a bare class-method invocation only — an instance-method spawn is rejected with `CAJETA_ERROR_ASYNC_R3A`. Cancellation semantics are implemented in stages. The running status is recorded in the internal concurrency status document, and this section binds them when they settle.

## 16.4 Ownership Across Task Boundaries

What may cross into a task follows from the ownership model, with no separate `Send`/`Sync` vocabulary:

- A value **borrowed** into a scoped task is sound: the borrow's lifetime is the scope's lifetime, and the scope blocks until the task finishes, so the source outlives every use.
- A value handed to a **detached** task, or stored anywhere that outlives the spawning frame, must be **transferred** (`#`) — the task becomes the owner and drops it.

**Example 16.4-1.** Both rules in one frame. The scoped task borrows, and the detached task takes the title.

```cajeta
public class Data { public int32 n; public Data() { this.n = 7; } }
public final class C {
    public static async void observe(Data d) { return; }
    public static async void consume(Data d) { return; }
    public static int32 run() {
        Data d = heap Data();
        scope {
            spawn observe(d);       // borrow — the scope joins before d drops
        }
        detach consume(#d);         // transfer — the task outlives this frame
        return 1;
    }
}
```

The last-use advisory (Ownership §5.10) exists for exactly this boundary. A lend at a local's final use before a spawn is usually a transfer the author did not spell.

## 16.5 Synchronization Primitives

The primitives live in `cajeta.concurrent`. The data-owning ones expose no acquire/release API — the critical section is a closure, the protected value never escapes it, and the lock releases when the closure returns *or unwinds*.

**`Mutex<T>`** owns the protected value. `withLock((T) -> #T)` runs the closure with the current value and stores back what it returns. `get()` snapshots the value under the lock.

**Example 16.5-1.** A guarded value, mutated under the lock and read back.

```cajeta
import cajeta.concurrent.Mutex;
public final class C {
    public static int32 run() {
        Mutex<int32> m = heap Mutex<int32>(1);
        m.withLock((n) -> n + 41);
        return m.get();             // 42
    }
}
```

**Wait-for-condition.** There is no standalone condition variable. `Mutex<T>.withLockWhen(predicate, body)` parks the caller until the predicate holds, re-checking it on every wake — the spurious-wakeup defense — and any `withLock` store wakes the waiters. The predicate must be free of side effects, since it is re-evaluated on every wake.

**Example 16.5-2.** A consumer parks until a producer pushes, then takes one.

```cajeta
import cajeta.concurrent.Mutex;
public final class C {
    public static async void push(Mutex<int32> q) {
        q.withLock((n) -> n + 1);
        return;
    }
    public static int32 run() {
        Mutex<int32> queue = heap Mutex<int32>(0);
        scope {
            spawn push(queue);
            queue.withLockWhen((n) -> n > 0, (n) -> n - 1);
        }
        return queue.get();         // 0 — pushed once, consumed once
    }
}
```

**`RwLock<T>`** is the read-heavy form. `read()` snapshots under a shared lock, and `withWrite((T) -> #T)` mutates and stores back under an exclusive one.

**Example 16.5-3.** A counter behind a read-write lock.

```cajeta
import cajeta.concurrent.RwLock;
public final class C {
    public static int32 run() {
        RwLock<int32> hits = heap RwLock<int32>(1);
        hits.withWrite((n) -> n + 41);
        return hits.read();         // 42
    }
}
```

> *Discussion.* The v1 surface of `RwLock<T>` is `read` plus `withWrite`. A closure form, `withRead((T) -> R)`, for computing under a shared lock without copying the value out, is designed and not yet implemented.

**`Semaphore`** is a counting permit pool. `withPermit(() -> void)` takes a permit for the closure and returns it on the way out, on both the normal and the exceptional path.

**Example 16.5-4.** A bounded section. The permit is back when the closure returns.

```cajeta
import cajeta.concurrent.Semaphore;
public final class C {
    public static int32 run() {
        Semaphore sem = heap Semaphore(2);
        sem.withPermit(() -> { System.stdout.println("in critical section"); });
        return sem.availablePermits();      // 2
    }
}
```

**`Lock`** is the one primitive with no associated data. `acquire()` returns a `#LockGuard`, received with `#=` (Ownership §5.5.2), and the guard's drop releases the lock on both the normal and the exceptional path (Allocation §4.2). `tryAcquire()` is the non-blocking form. It returns `1` when the lock was taken and hands out no guard, so each success pairs with one `releaseLock()`.

**Example 16.5-5.** A guard released by its drop, and the non-blocking form.

```cajeta
import cajeta.concurrent.Lock;
import cajeta.concurrent.LockGuard;
public final class C {
    public static int32 run() {
        Lock gate = heap Lock();
        {
            LockGuard g #= gate.acquire();
            System.stdout.println("held");
        }                                   // g drops, the lock releases
        int32 free = gate.tryAcquire();     // 1 — the lock was free again
        if (free == 1) { gate.releaseLock(); }
        return free;
    }
}
```

Acquisition methods are ordinary, non-`async` calls. A fiber that finds the lock held parks and yields its carrier, and a main-thread caller blocks. No signature changes color because a lock is involved.

## 16.6 Channels, Atomics, and Fiber-Local State

**`Channel<T>`** is a bounded multi-producer multi-consumer queue, the message-passing alternative to shared state. `send` blocks while the buffer is full, `receive` blocks while it is empty and the channel is open, and `close` is one-way. A `receive` returns a present `Optional<T>` with the item, or an empty one once the channel is closed and drained. Select-style receives are typed through `SelectResult`.

**Example 16.6-1.** Two sends, a close, and a drain.

```cajeta
import cajeta.concurrent.Channel;
public final class C {
    public static int32 run() {
        Channel<int32> ch = heap Channel<int32>(4);
        ch.send(20);
        ch.send(22);
        ch.close();
        int32 total = 0;
        Optional<int32> a = ch.receive();
        Optional<int32> b = ch.receive();
        if (a.isPresent()) { total = total + a.get(); }
        if (b.isPresent()) { total = total + b.get(); }
        return total;               // 42
    }
}
```

**`AtomicInt32` / `AtomicInt64`** are single-word atomics — `load`, `store`, `fetchAdd`, and `compareAndSet`.

**Example 16.6-2.** A counter without a lock.

```cajeta
import cajeta.concurrent.AtomicInt32;
public final class C {
    public static int32 run() {
        AtomicInt32 counter = heap AtomicInt32(0);
        counter.fetchAdd(41);
        counter.compareAndSet(41, 42);
        return counter.load();      // 42
    }
}
```

**`FiberLocal<T>`** is ambient per-fiber state, the sound replacement for the `ThreadLocal` pattern — per-request context and the like. `where(value, body)` binds for the dynamic extent of `body` and restores the prior binding when it returns or throws. `get()` on an unbound fiber throws, so `orElse` and `isBound` are the safe readers.

**Example 16.6-3.** A binding that exists only inside its `where` body.

```cajeta
import cajeta.concurrent.FiberLocal;
public final class C {
    public static int32 run() {
        FiberLocal<int32> current = heap FiberLocal<int32>();
        current.where(42, () -> { System.stdout.println("bound " + current.get()); });
        return current.orElse(7);   // 7 — unbound again outside the body
    }
}
```

The full API surface of the package is the Stdlib Reference's. The semantics stated in this chapter — closure scoping, release-on-unwind, park-not-block — are language guarantees.

## 16.7 Absent Constructs

Cajeta has no `synchronized` method or block, no `volatile` fields, and no `ThreadLocal`. Each exists in Java to manage mutable state shared across threads by default. Cajeta's ownership model makes that sharing the explicit, unusual case, and the closure-scoped primitives above are the sanctioned form. Locks that could be acquired and forgotten are unrepresentable — only `Lock` yields a guard object at all, and its release is a drop.

## 16.8 Memory Model

> *Discussion.* The cross-thread memory-model protocol (a Send/Sync-style discipline for types) is deferred. Today's guarantees are structural — scope-bounded borrows, transfer-for-detach, and the primitives of §16.5. The scheduler runs a multi-carrier work-stealing pool by default (`CAJETA_CARRIERS` overrides the count, and `1` gives deterministic single-carrier execution). A fiber is pinned to the carrier that first ran it, so parallelism comes from fanning out across distinct fibers. These are implementation properties recorded here for orientation, not yet normative guarantees.
