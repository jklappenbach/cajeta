# `FiberLocal<T>` — ambient per-request state — Specification v1

_Package: **`cajeta.concurrent`** (alongside `Mutex`, `Channel`, `Tasks`).
Companion to `docs/specification/concurrent/Concurrency.md`. Plan: `plans/FiberLocal-plan.md`.
Guided walkthrough: `docs/specification/concurrent/FiberLocal-tour.md`._

## 1. The problem

Some state is logically attached to a **unit of work** — an HTTP request, a job,
a command — not to a single function call. Threading it through every function
signature (`handle(req, ctx)`, `validate(input, ctx)`, `persist(row, ctx)`, …)
is the explicit-parameter tax. The canonical case is **logging**: a request-scoped
"system log" entry (one wide JSON event per request — see the logging spec) that
any code on the request's path can add fields to, without every layer taking a
`SystemLogEntry` parameter. Tracing/correlation IDs, the authenticated principal,
a per-request deadline, and a DB transaction handle are the same shape.

Two execution shapes must both work:

- **Single fiber owns the request end-to-end.** One fiber accepts the request,
  does the work, writes the response. State set at entry is visible everywhere
  it calls.
- **The request is handed off across fibers.** A fiber accepts the request, fans
  out to children (parallel sub-queries) and/or enqueues it onto a worker pool.
  The logging view must stay **consistent for the whole request** regardless of
  which fiber is currently running it.

Cajeta's existing model gives us strong cards and one hard constraint:

- **Fibers are single-use** (`Concurrency.md` § `async fn`): one fiber runs one
  task to completion, then is freed. A per-fiber slot therefore has naturally
  request-scoped lifetime and **cannot** leak into a later, unrelated request —
  unlike a thread-pool `ThreadLocal`, whose stale-value bug comes precisely from
  the OS thread outliving the work.
- **Carriers are pooled OS threads** (`min(nproc,4)`). So a *literal* thread-local
  (`__thread`) is exactly the wrong primitive — it would alias across the many
  fibers a carrier hosts and bleed one request's state into another. This is the
  same reason `scope_top` / `drop_top` / `exc_top` already live **per fiber** in
  the fiber control block, not in a `__thread` slot.
- **No data races at compile time.** Ambient state must not become shared mutable
  state across fibers.

This is why `Concurrency.md` § Goals banned `ThreadLocal` — and why a
**`FiberLocal`** is nonetheless sound. The ban was on the *thread*-keyed,
pool-aliasing, leak-prone Java primitive. A fiber-keyed, single-use,
scope-restored binding is a different animal.

## 2. What other languages do (and what we take)

| Mechanism | Lifetime / key | Cross-task propagation | What we take / reject |
|---|---|---|---|
| Java `ThreadLocal` / `InheritableThreadLocal` | OS thread; mutable | child *thread* inherits a copy at creation | **Reject** the mutable/thread-keyed core (the pool-leak footgun); keep the *inherit-on-spawn* idea. |
| Java 21 `ScopedValue` + `StructuredTaskScope` | dynamic extent of `where(v, body)`; immutable | child tasks of the structured scope see the binding | **Adopt as primary.** Immutable, auto-restored, structured-inherit — fits our drop chain + structured concurrency exactly. |
| .NET `AsyncLocal<T>` | flows down the async call-tree; copy-on-write | automatic across `await` continuations | **Adopt the COW snapshot-on-fork** semantics; reject implicit flow across *unrelated* fibers. |
| Go `context.Context` | explicit value, passed by hand | only where you pass it | **Adopt as the handoff discipline:** explicit at boundaries (`FiberContext`), ambient within. |
| Rust `thread_local!` + Tokio `tokio::task_local!` | thread / task; `task_local!` scoped via `.scope(v, fut)` | task-local scopes the future | Confirms the scoped-binding shape is the modern consensus. |
| Kotlin `CoroutineContext` element | coroutine; structured | inherited by child coroutines of the scope | Same structured-inherit lesson. |

**Consensus the field has converged on:** the modern, safe design is a
**scoped, immutable, structured-inherited** binding (ScopedValue / task_local /
CoroutineContext), *not* the mutable thread-keyed slot (`ThreadLocal`). We adopt
that, and add Go's explicit-context discipline for the unstructured handoff that
structured inheritance can't reach.

## 3. The three layers

### Layer 1 — ambient slot, scoped (primary API)

```cajeta
// Declare one binding key per kind of ambient state. Module-level / static.
static FiberLocal<RequestId> REQUEST_ID = heap FiberLocal<RequestId>();

// Bind it for the dynamic extent of `body`; the prior value is restored when
// `body` returns OR throws (the binding rides the drop chain).
REQUEST_ID.where(id, () -> {
    handle(req);          // anything called in here sees REQUEST_ID.get()
});                       // <- binding popped here, even on a throw
```

`where(value, body)` is the recommended form: the binding's lifetime is exactly
`body`'s dynamic extent, it is immutable for that extent, and cleanup is
automatic. This mirrors the library's existing closure-form philosophy
(`Mutex.withLock`, `Semaphore.withPermit`) — the value has no handle outside the
scoped block, so "forgot to clear it" is unrepresentable.

`get()` reads the current binding; it is an error to `get()` an unbound key
(use `orElse` / `isBound` when a binding may be absent).

### Layer 2 — inherit on `spawn` (structured fan-out)

A child fiber spawned **inside a `scope`** inherits the parent's bindings as they
stood at the `spawn`:

```cajeta
REQUEST_ID.where(id, () -> {
    scope {
        spawn () -> async void { queryUsers();   };  // both children see
        spawn () -> async void { queryOrders();  };  // REQUEST_ID.get() == id
    }
});
```

This is what makes the **multi-fiber-but-structured** case work with zero
ceremony: the parallel sub-queries of one request all observe the same request
context. Safety rests on the join the concurrency model already guarantees —
`scope` does not exit until every child completes, so a binding *borrowed* into a
child cannot outlive its owner (`Concurrency.md` § "Borrows can outlive the
spawn"). Inheritance is a **copy-on-write snapshot** of the binding stack head:
a child re-binding a key with its own `where` does not perturb the parent or its
siblings (no shared mutable ambient state → no race).

### Layer 3 — explicit capture across an unstructured handoff

When a request crosses to an **unrelated** fiber — pushed onto a `Channel` for a
worker pool, or `detach`ed — the ambient chain deliberately does **not** follow.
(Auto-flow there would be both unsound, the source fiber's borrows may already be
gone, and wrong, the worker fiber serves many requests over its life.) You carry
the context explicitly:

```cajeta
// Producer: snapshot the live bindings and hand them off with the work item.
FiberContext ctx = FiberContext.capture();   // immutable snapshot, # owns it
channel.send(WorkItem(payload, #ctx));

// Worker fiber: reinstall the snapshot for the extent of handling this item.
Optional<WorkItem> item = channel.receive();
item.get().ctx.run(() -> process(item.get().payload));
```

`ctx.run(body)` (an instance method on the captured snapshot) installs its bindings for `body`'s extent
and restores the worker's prior (empty) state on exit — so the worker fiber never
accretes one request's state into the next. The snapshot is transferred with `#`
(single new owner), matching the `detach`/channel captures rule
(`Concurrency.md` § Sendability): a request context that crosses an unstructured
boundary is *moved*, not aliased.

### Where the "request-ID cache" fits (and why it is the escape hatch)

A global `ID → context` **registry** (a concurrent map keyed by request/command
ID) is a *fourth*, heavier option, and is **deliberately not** the core
mechanism. Carrying the context directly (Layers 1–3) avoids a process-global
concurrent map and its attendant problems — entry **cleanup** (who deletes the
key when the request ends? leak it and you have the `ThreadLocal`-pool bug back,
at global scope), **contention** on the shared map, and **lifetime** (the map
holds the context alive past its owner). It is the right tool for exactly one
case: a boundary that can carry only a *string* — an FFI hop, or a serialized
queue message on another process — where the live `FiberContext` object cannot
travel. Then you put the correlation ID in the message, and the receiver
reconstructs context from the ID. v1 ships Layers 1–3; the registry is documented
as a pattern (`FiberContext.toToken()` / a user-owned map), not a core type.

## 4. Surface API (v1 as shipped)

```cajeta
package cajeta.concurrent;

public class FiberLocal<T> {          // T may be any type (primitive or reference)
    public FiberLocal();              // an unbound key

    // scoped binding (the only binding API in v1): bind for body's extent,
    // restore on return OR throw (rides the drop chain).
    public void where(#T value, () -> void body);

    // reads
    public T get();                 // current binding; throws when unbound
    public T orElse(T fallback);    // current binding, or fallback when unbound
    public boolean isBound();
}

public class FiberContext {
    public static #FiberContext capture();   // immutable snapshot of current bindings
    public void run(() -> void body);        // install for body's extent, restore after
    public ~FiberContext();                  // frees the snapshot
}
```

**`T` may be any type** — primitive or reference. The per-fiber runtime store
keys bindings to opaque pointers, so the value is boxed in a `FiberLocalBox<T>`
(a reference) on `where` and read back through that box's typed `value` field on
`get` — the same typed-slot approach `Optional<T>` uses, so a primitive `T`
round-trips correctly rather than being bitcast out of a raw pointer.

**Deferred to post-v1** (designed, not shipped — kept here so the surface leaves
room):
- `FiberLocal(#T initial)` — a process-wide default. An owned reference field
  that is legitimately null trips the drop chain; needs a borrow-or-guard design.
- `set(#T)` / `remove()` — imperative (no auto-restore) escape hatch. `where` is
  preferred regardless; ship `set` only if a concrete need appears.
- `<R> R where(...)` / `<R> R run(...)` — value-returning scoped forms (v1 is
  `void`-only; capture results in a heap accumulator the body writes, or a field).

For inherit-on-spawn and capture-on-handoff, the binding object's lifetime is
guaranteed either by the structured scope (Layer 2 — the scope outlives its
children) or by the `#`-transfer of the `FiberContext` snapshot (Layer 3).

## 5. Runtime design

The fiber control block (`runtime/native/cajeta_runtime.c` `struct cajeta_fiber`,
~line 1043) already carries per-fiber `scope_top`, `drop_top`, `exc_top`,
`cancel_with`. Add one more head of the same family:

```c
struct cajeta_fiber_local {          // one binding frame
    void* key;                       // the FiberLocal<T> identity
    void* value;                     // T (boxed/pointer; primitives by value-in-pointer)
    struct cajeta_fiber_local* prev; // immutable linked stack (newest first)
};
// in struct cajeta_fiber:  struct cajeta_fiber_local* fl_top;
// main thread: a __thread fallback head, same pattern as drop_top/exc_top.
```

Intrinsics (wrapped by the Cajeta class, never called by user code), resolved the
same way as `Cajeta.lockNew` et al.:

- `Cajeta.fiberLocalPush(key, value) -> token` — push a frame, return the prior
  head as a restore token. `where` pushes on entry.
- `Cajeta.fiberLocalPop(token)` — restore the head to `token`. Runs from the
  drop chain on `where`/`run` exit (normal **or** unwind).
- `Cajeta.fiberLocalGet(key) -> value` — walk the chain newest-first for `key`;
  fall back to the key's default; signal unbound otherwise.
- `Cajeta.fiberContextCapture() -> snapshot` — copy the current chain head
  (cheap: the frames are immutable, so capture is pointer-snapshot + refcount /
  shallow copy, not a deep walk).

**Inherit-on-spawn:** `__cajeta_task_run` (the spawn path) copies the parent's
`fl_top` head into the new fiber's `fl_top` (COW — frames are immutable and
shared; a child `where` pushes a *new* frame, never mutates a shared one). This
is a single pointer copy at spawn; no per-key work.

**Accessor:** `__cajeta_current_fiber` (line ~1303) selects the fiber's
`fl_top`; the main thread uses the `__thread` fallback head — identical to how
`__cajeta_drop_top_ptr` / `__cajeta_exc_top_ptr` already choose.

**No new capability.** Fiber-local state is pure in-process memory; it needs no
`network`/`filesystem`/`clock` capability and is hermetic by construction.

## 6. Safety properties (why this is sound where `ThreadLocal` isn't)

1. **No cross-request leak.** Bindings live on the *fiber*, and a fiber runs one
   task; `where`/`run` additionally restore on exit. The thread-pool stale-value
   bug is structurally absent.
2. **No forgotten cleanup.** The scoped forms restore via the drop chain on both
   the normal and the exception edge (same machinery as `Mutex.withLock`).
3. **No data race.** Frames are immutable; inheritance is COW; a child never
   mutates a binding another fiber can observe. Consistent with `Concurrency.md`
   § Goals ("no data races at compile time").
4. **Handoff is explicit and moved.** Crossing an unstructured boundary requires
   a `#`-transferred `FiberContext` — aliasing a request's context across
   unrelated fibers is unrepresentable, not merely discouraged.

## 7. Interaction with the rest of the language

| Feature | Interaction |
|---|---|
| `#` transfer | `where(#T, …)` and `FiberContext.capture()` move/borrow per the existing ownership rules; cross-fiber handoff is a `#` move (the established "send" marker). |
| Drop chain | `where`/`run` push a restore entry; it fires on return and on unwind — unbinding-on-exception is automatic. |
| `scope` / `spawn` | inherit-on-spawn snapshots the binding head; the scope join guarantees a borrowed context outlives its children. |
| `detach` | does **not** auto-inherit (no scope to anchor lifetimes); must `#`-capture a `FiberContext`, matching detach's borrow-capture ban. |
| Exceptions | a throw through a `where` body restores the prior binding on the unwind, before the catch runs. |
| Templates | `FiberLocal<T>` monomorphizes like `Mutex<T>` — no erasure, primitives allowed. |
| Capabilities | none required; pure memory. |

## 8. Lint / diagnostics

- Prefer `where` over `set` *(latent — v1 ships only `where`)*: if `set` ever
  ships, a `set` with no matching fiber-lifetime rationale → advisory lint
  (`fiber-local-prefer-scoped`).
- `get()` on a possibly-unbound key without a default → suggest `orElse`/`isBound`.
- Capturing a `#`-owned heap binding into a `detach`/channel without
  `FiberContext.capture()` → reuse the existing `CAJETA_ERROR_DETACH_BORROW_CAPTURE`
  class.

## 9. Non-goals (v1)

- The global `ID → context` registry as a core type (documented pattern only — § 3).
- Automatic flow across `detach`/channels (explicit `FiberContext` by design).
- Per-key change listeners / observers.
- `set`/`remove` are deliberately not shipped in v1 — scoped `where` only (§ 4);
  add `set` only if a concrete need appears.

## 10. The logging tie-in (why this lands first)

The logging framework's **system log** is one wide event per request whose fields
accrue as the request runs. Its storage is precisely a
`FiberLocal<SystemLogEntry>`: set at request entry (`where`), inherited by
structured sub-fibers (Layer 2), reinstalled across a worker handoff (Layer 3).
`FiberLocal` is therefore a prerequisite, which is why it ships before the
logging framework returns. See the logging spec's system-log section.

## 11. Design questions — resolved in v1

1. Ship `set`/`remove`, or scoped-only (`where`) first? **Resolved:** scoped-only
   (`where`); `set`/`remove` deferred (§ 4, § 9). `where` is preferred regardless.
2. Should `FiberContext.capture()` snapshot **all** keys, or take an explicit key
   set? **Resolved:** snapshots all live bindings (the chain head) — ergonomic and
   cheap since frames are immutable. An explicit-key form can be added later if a
   heavy binding ever needs to be kept off a handoff.
3. `get()` on unbound: throw, or require `Optional`-returning `find()` only?
   **Resolved:** `get()` throws when unbound; `orElse` / `isBound` cover the
   may-be-absent path (validated by `orElseUnboundReturnsFallback` / `constructUnbound`).
4. Token format for the Layer-3 registry escape hatch — ship a helper, or leave to
   user code? **Resolved:** left entirely to user code in v1 (no `toToken`/`fromToken`
   shipped); revisit if the FFI/string-boundary case becomes common.
