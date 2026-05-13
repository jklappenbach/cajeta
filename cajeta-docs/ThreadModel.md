# Cajeta Threading Model — Specification v1

## Goals

- **No data races at compile time.** The ownership/borrow machinery already used for single-threaded memory safety extends to thread boundaries; sharing mutable state without serialization is rejected before codegen.
- **Structured concurrency by default.** Every spawned task has a scope. Scopes don't return until their children complete or are cancelled. "Fire and forget" requires explicit opt-in.
- **No SAM interfaces for tasks.** Threading primitives consume function-typed values — there is no `Runnable`, no `Callable`, no `Future.get()` vs `CompletableFuture.then*` schism.
- **No locks, no `synchronized`, no `volatile`, no `ThreadLocal`.** Each of those Java warts comes from sharing mutable state across threads; Cajeta makes that the unusual case, not the default.
- **No GC, no runtime bootstrap.** Task state machines and actor mailboxes lower to plain structs allocated on the single-owner heap. The runtime is a small set of C functions (executor + scheduler + queue primitives), in the same style as the existing `__cajeta_*` helpers.

## Non-goals (v1)

- Preemptive cancellation. Cancellation is cooperative — a task observes a cancel signal at its next `await`.
- Custom executors beyond the built-in work-stealing pool.
- Distributed actors (RPC across machines).
- Effect handlers / algebraic effects.
- Async iteration / streaming (`AsyncIterator` style); covered separately if added.

---

## The three primitives

### 1. `async fn` — suspendable functions

```
async int32 fetchScore(Url u) {
    Response r = await http.get(u);
    return r.statusCode;
}
```

Every `async` function has return type `Task<T>`. Awaiting a `Task<T>` either:
- returns the inner `T` immediately if the task has completed, or
- suspends the current task and resumes when the awaited task completes.

`await` is only legal inside `async fn` / inside an `actor` method / inside a `spawn` body. Calling an `async fn` without `await` produces an unstarted `Task<T>` — it does nothing until awaited or `spawn`ed.

Under the hood: `async fn` lowers to a state machine — same shape as Rust/Swift/C# — with the captures and locals living in a heap-allocated frame. The frame is owned by whichever scope launched the task.

### 2. `actor` — types with serialized state

An `actor` is a type kind, distinct from `class`:

```
actor Counter {
    int32 value = 0;
    public int32 next() { value++; return value; }
    public int32 read()  { return value; }
}
```

Rules:

- All fields are private. Only the actor's own methods touch them.
- All public methods are implicitly `async` and have return type `Task<T>` from the caller's perspective.
- Calls from **outside** the actor go onto its mailbox queue and resume the caller when the actor's executor picks up the message.
- Calls from **inside** the actor (one of its methods calling another) are direct synchronous calls — there's no queue trip, no race; the actor's single thread of execution is already inside.
- The actor itself is `Sendable` (see below) once constructed; passing an `actor` reference across thread boundaries is always safe.

No locks. No `synchronized`. Mutual exclusion comes from the actor having exactly one thread of execution at a time — the executor guarantees it.

```
async int32 useCounter() {
    Counter c = new Counter();
    int32 a = await c.next();   // mailbox message, may yield
    int32 b = await c.next();
    return a + b;               // 1 + 2 = 3
}
```

### 3. `scope { ... }` — structured concurrency blocks

`scope` is a block that owns the lifetime of every task spawned inside it. Control doesn't leave the block until every child task has finished or been cancelled.

```
async void processAll(List<Url> urls) {
    scope {
        for (Url u in urls) {
            spawn fetchScore(u);   // launches a child Task<int32>
        }
        // (control reaches here only after every spawn'd task completes)
    }
}
```

Properties:

- **No leaked tasks.** A `spawn` without an enclosing `scope` is a compile error.
- **Cancellation propagates down.** Cancelling the scope cancels every child task.
- **Errors propagate up.** If a child throws, the scope cancels its other children and re-throws once they've finished unwinding.
- **Borrows can outlive the spawn.** Because the scope is guaranteed to join all children before returning, borrows captured by `spawn`'d tasks are valid for the lifetime of the scope. This is the structural escape-hatch that lets parallel-with-borrows work safely.

`spawn` returns a `Task<T>` that you can optionally `await`. Without `await`, the result is dropped at scope-end (still after the task completes).

To deliberately run a task that outlives the current scope, use `detach`:

```
detach backgroundWork();   // explicit opt-out of scope; rare
```

`detach` requires the spawned function to capture only by `#` transfer (no borrows) — there's no scope to anchor lifetimes to.

---

## Sendability — derived from the existing ownership model

A value crosses a thread boundary in exactly two ways:
- **Transferred** via `#` (the existing memory-model operator). The originating thread can't touch the value after; the receiving thread is the new owner. Always safe.
- **Borrowed** into a scoped task (a `spawn` inside a `scope`). The borrow's lifetime is the scope's lifetime; the scope blocks until the task finishes, so the borrow is valid.

The compiler enforces:
- A `spawn` body whose captures include a borrow of `x` requires `x` to live at least as long as the enclosing scope. (The same lifetime analysis used for `MemoryModel.md`'s borrow rules.)
- A `detach` body cannot capture borrows — only `#`-transferred values.
- Sending a borrow to an actor is allowed *only if* the borrow's lifetime extends past the latest possible point at which the actor might use it. In practice this means borrows to actors are only safe for the duration of a single `await`. The compiler infers when this fails and forces a `#` transfer.

No `Send` / `Sync` traits, no `Sendable` protocol — sendability is what the borrow checker already says about a value's lifetime relative to the consuming thread's lifetime.

---

## Cancellation

Cancellation is cooperative and lattice-shaped:

- Every `Task<T>` has a cancellation token, threaded implicitly.
- `await` checks the token on resume. If set, the await raises a `CancellationException`.
- A scope's parent task cancelling causes the scope to cancel all children, which causes their next `await` to raise.
- Cleanup happens via the standard `try` / `finally` machinery; the existing drop-chain machinery fires owners on the unwind.

The token is propagated automatically; user code rarely sees it explicitly. For explicit cancel-on-condition, `withTimeout(d) { ... }` and `withDeadline(t) { ... }` wrap a scope with a token that fires at the deadline.

---

## Message passing — channels

For producer / consumer patterns the model provides typed channels:

```
Channel<int32> ch = new Channel<int32>(capacity: 8);

scope {
    spawn () -> async void {
        for (int32 i = 0; i < 10; i++) {
            await ch.send(i);
        }
        ch.close();
    };
    spawn () -> async void {
        while (true) {
            int32 v = await ch.receive();
            // ...
        }
    };
}
```

- Bounded by default; `send` blocks when full, `receive` blocks when empty.
- `close` is one-way; subsequent `send`s throw, subsequent `receive`s return the remaining buffered items then throw on empty.
- Channel ownership follows the standard model — a `Channel<T>` is a heap value owned by one scope; passing it to spawn'd tasks borrows it for the scope's lifetime.

Channels are not a replacement for actors — they're complementary. Use a channel when the producer/consumer roles are clear; use an actor when the state needs serialization across many access patterns.

---

## Examples

### Single-threaded `async` (event-loop style)

A program with no `spawn` is just an event loop; `await` is a coroutine yield.

```
async int32 main() {
    Response a = await http.get(urlA);
    Response b = await http.get(urlB);
    return a.bytes + b.bytes;
}
```

### Parallel fan-out with structured joins

```
async int32 sumScores(List<Url> urls) {
    int32 total = 0;
    Mutex<int32> totalLock = new Mutex<>(total);  // see below — not idiomatic; see actor example
    scope {
        for (Url u in urls) {
            spawn () -> async void {
                int32 s = await fetchScore(u);
                await totalLock.update(v -> v + s);
            };
        }
    }
    return totalLock.take();
}
```

Better:

```
actor Tally {
    int32 total = 0;
    public void add(int32 s) { total += s; }
    public int32 final() { return total; }
}

async int32 sumScores(List<Url> urls) {
    Tally tally = new Tally();
    scope {
        for (Url u in urls) {
            spawn () -> async void {
                int32 s = await fetchScore(u);
                await tally.add(s);
            };
        }
    }
    return await tally.final();
}
```

### Bounded producer / consumer

```
async void pipeline(Source src, Sink sink) {
    Channel<Record> ch = new Channel<>(capacity: 64);
    scope {
        spawn () -> async void {
            while (Record r = await src.next()) {
                await ch.send(r);
            }
            ch.close();
        };
        spawn () -> async void {
            while (true) {
                Record r = await ch.receive();
                await sink.write(r);
            }
        };
    }
}
```

### Timeout

```
async Response getWithTimeout(Url u, Duration d) {
    return await withTimeout(d) {
        await http.get(u);
    };
}
```

---

## Runtime requirements

A small C runtime ships in `runtime/native/`, paralleling the existing exception / drop-chain helpers:

- **Task allocator** — `__cajeta_task_new(size, state_machine_fn)` allocates a task frame on the heap. Owned by the spawning scope.
- **Executor** — work-stealing pool, one OS thread per core by default. `__cajeta_task_schedule(task)` puts a task in the run queue.
- **Mailbox primitive** — lock-free MPSC queue used as an actor's inbox.
- **Channel primitive** — bounded MPMC queue.
- **Token primitive** — atomic flag for cancellation; tasks check on resume.

`async fn` and `actor` methods lower to state machines whose suspension points are LLVM coroutine intrinsics or hand-rolled equivalents. Each suspension saves locals into the task frame; resume restores them and dispatches on the saved state.

---

## Interaction with the rest of the language

| Feature | Threading interaction |
|--|--|
| `#` transfer | The ownership-move marker doubles as the "send across threads" marker. Cross-thread transfer is what `#` already means at scope boundaries. |
| Templates | `Task<T>`, `Channel<T>`, `actor` typeParameters monomorphize identically to class templates — no erasure, primitives allowed. |
| Interfaces | An actor can `implements` an interface; calls through the interface go through the actor's mailbox. |
| Drop chain | Task frame drop fires the scope-end drops in the right order — same machinery as ordinary returns. |
| Exceptions | Async exceptions raise on `await`; the existing setjmp/longjmp-based catch chain unwinds through the suspension stack. |

---

## Known gaps / open questions

- **Async iteration.** `for (T x in asyncIterable) { ... }` syntax. Deferred — requires designing `AsyncIterator<T>` and the desugaring rule.
- **Pinning.** Some runtime objects (e.g. interop with OS callbacks) may need to be pinned to a thread. v1 has no pinning; revisit if FFI threading becomes a concern.
- **Inlining `await` in a non-async context.** Today `await` is illegal outside async functions / actor methods / spawn bodies. A `runBlocking { ... }` escape hatch (Kotlin-style) is open for v2.
- **Channel select.** Multiplexed `select { case <- a: ...; case <- b: ... }` syntax — deferred.
- **Reentrant actors.** What happens when actor A calls a method on actor B, which calls back into A? v1 says: deadlock — the call from B back into A blocks waiting for A's queue to drain, which can't happen because A is blocked waiting for B. Solutions (priority queues, re-entrance detection) are open design space; document the rule and move on.
- **Detached task ownership and leaks.** `detach` consumes captures by `#`. The detached task's lifetime is the runtime's lifetime; nothing reclaims its result. Use sparingly.
