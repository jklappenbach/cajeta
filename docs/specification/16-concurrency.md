# 16 — Concurrency

This chapter defines the concurrency model: `async` functions running as fibers, structured `scope` blocks that own the tasks spawned inside them, `spawn`, the ownership rules that make data races unrepresentable by construction, and the synchronization primitives whose critical sections are closures. There is no `synchronized`, no `volatile`, and no `ThreadLocal` (§16.7).

## 16.1 `async` Functions and `await`

`async` is a method modifier: an `async` method may suspend, and runs as a fiber on the runtime's carrier pool. `await expr` on an async call suspends the awaiting fiber until the result is ready and yields the value; an exception in the awaited task re-raises in the awaiter's frame.

Ordinary (non-`async`) methods never suspend at the language level — when one blocks on a synchronization primitive (§16.5), the runtime parks the calling fiber under the hood; the method's signature stays synchronous.

## 16.2 `scope` — Structured Concurrency

`scope { … }` is a block that owns the lifetime of every task spawned inside it: control does not leave the block until every child has finished or been cancelled. Joining is implicit at the closing brace.

**Example 16.2-1.** Spawned children joined by scope exit; a guarded counter.

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

`spawn call` starts an async call as a child task of the enclosing `scope`; the scope joins it. `detach call` starts a task not bound to a scope; a detached task must own everything it touches (§16.4).

> *Discussion.* As of 0.27.0 `spawn` accepts a bare class-method invocation only — an instance-method spawn is rejected with `CAJETA_ERROR_ASYNC_R3A`. Cancellation semantics are implemented in stages; the running status is recorded in the internal concurrency status document and this section binds them when they settle.

## 16.4 Ownership Across Task Boundaries

What may cross into a task follows from the ownership model, with no separate `Send`/`Sync` vocabulary:

- A value **borrowed** into a scoped task is sound: the borrow's lifetime is the scope's lifetime, and the scope blocks until the task finishes, so the source outlives every use.
- A value handed to a **detached** task, or stored anywhere that outlives the spawning frame, must be **transferred** (`#`) — the task becomes the owner and drops it.

The last-use advisory (Ownership §5.10) exists for exactly this boundary: a lend at a local's final use before a spawn is usually a transfer the author did not spell.

## 16.5 Synchronization Primitives

The primitives live in `cajeta.concurrent`. The data-owning ones expose no acquire/release API — the critical section is a closure, the protected value never escapes it, and the lock releases when the closure returns *or unwinds*:

- **`Mutex<T>`** owns the protected value. `withLock((T) -> #T)` runs the closure with the current value and stores back what it returns; `get()` snapshots.
- **`RwLock<T>`** — `withRead` / `withWrite` for read-heavy state.
- **`Semaphore`** — `withPermit`, a counting permit pool.
- **`Lock`** is the one primitive with no associated data; it hands out a `LockGuard` whose drop releases — on both the normal and the exceptional path (Allocation §4.2).

**Wait-for-condition.** There is no standalone condition variable. `Mutex<T>.withLockWhen(predicate, body)` parks the caller until the predicate holds, re-checking it on every wake — the spurious-wakeup defense — and any `withLock` store wakes the waiters.

Acquisition methods are ordinary, non-`async` calls: a fiber that finds the lock held parks and yields its carrier; a main-thread caller blocks. No signature changes color because a lock is involved.

## 16.6 Channels, Atomics, and Fiber-Local State

- **`Channel<T>`** — a bounded multi-producer multi-consumer queue, the message-passing alternative to shared state; select-style receives are typed through `SelectResult`.
- **`AtomicInt32` / `AtomicInt64`** — single-word atomics.
- **`FiberLocal`** — ambient per-fiber state, the sound replacement for the `ThreadLocal` pattern (per-request context and the like).

The full API surface of the package is the Stdlib Reference's; the semantics stated in this chapter — closure scoping, release-on-unwind, park-not-block — are language guarantees.

## 16.7 Absent Constructs

Cajeta has no `synchronized` method or block, no `volatile` fields, and no `ThreadLocal`. Each exists in Java to manage mutable state shared across threads by default; Cajeta's ownership model makes that sharing the explicit, unusual case, and the closure-scoped primitives above are the sanctioned form. Locks that could be acquired and forgotten are unrepresentable: only `Lock` yields a guard object at all, and its release is a drop.

## 16.8 Memory Model

> *Discussion.* The cross-thread memory-model protocol (a Send/Sync-style discipline for types) is deferred; today's guarantees are structural — scope-bounded borrows, transfer-for-detach, and the primitives of §16.5. The scheduler runs a multi-carrier work-stealing pool by default (`CAJETA_CARRIERS` overrides the count; `1` gives deterministic single-carrier execution); a fiber is pinned to the carrier that first ran it, so parallelism comes from fanning out across distinct fibers. These are implementation properties recorded here for orientation, not yet normative guarantees.
