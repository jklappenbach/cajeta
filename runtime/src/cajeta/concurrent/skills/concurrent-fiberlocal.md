---
id: concurrent-fiberlocal
applies-to: [cajeta/concurrent/FiberLocal, cajeta/concurrent/FiberContext, cajeta/concurrent/FiberLocalBox]
title: FiberLocal — ambient per-fiber state with scoped binding and explicit handoff
description: Bind request-scoped ambient state per fiber (FiberLocal.where/get/orElse), inherited by structured spawn and carried across unstructured boundaries via FiberContext.
---

Ambient per-fiber state: the sound replacement for a thread-pool `ThreadLocal`/MDC.
Bind a value for a dynamic scope on the current fiber; read it deep on the call path
with no parameter threading. Carriers are pooled OS threads, so a `__thread` slot would
alias across the many fibers one carrier hosts — these bindings live on the **fiber**,
which is single-use, so they are naturally request-scoped and cannot leak into a later
request.

## Pick the right tool

- **One fiber owns the request, or you `spawn` children inside a `scope`** → use
  `FiberLocal<T>` alone (`where` to bind, `get`/`orElse`/`isBound` to read). A child
  spawned inside a structured `scope` **inherits the binding automatically** — the
  runtime snapshots the spawner's chain. Nothing extra to do.
- **Unstructured handoff** — pushing work onto a `Channel` for a worker pool, or
  `detach` → the chain deliberately does **NOT** auto-follow. `FiberContext.capture()`
  on the producer, `#`-transfer the snapshot with the work item, `ctx.run(body)` on the
  consumer.
- `FiberLocalBox<T>` is **internal** — never reference it in your code. It is how `where`
  boxes the value so any `T` (including primitives like `int64`) round-trips through the
  per-fiber store. You only ever see `T`.

`T` may be **any type** — reference or primitive. A `FiberLocal<int32>` returns a real
`int32` from `get()`, not a bitcast pointer.

## Members and roles

- `FiberLocal<T>` — the key. Identity-only: a binding is keyed by this object's address,
  so the object carries no per-instance binding state (one key, many fibers each with
  their own binding). Construct one per logical slot, typically `static`.
- `FiberContext` — an **immutable snapshot** of the current fiber's whole binding chain,
  for explicit handoff. Owns a deep-copied chain; frees it in `~FiberContext`.
- `FiberLocalBox<T>` — internal value box; not part of the public surface.

## FiberLocal — the methods

- `public FiberLocal()` — construct a key. A fresh key is unbound on every fiber.
- `public void where(T value, () -> void body)` — bind `value` for the dynamic extent of
  `body`, restoring the prior binding when `body` returns **OR throws**. Cleanup is
  structural (rides the drop chain, like `Mutex.withLock`) so a binding can never leak
  past its scope. The formal is plain, so the mode is the caller's choice: `where(value, ...)`
  lends the value for the extent of `body` (the caller keeps title and must keep it
  alive), `where(#value, ...)` hands the binding the title. **`where` is `void`** in
  v1 — to get a result out of `body`, write it through a captured heap holder.
- `public T get()` — the current binding. **Throws when unbound** (v1 throws `1`). Guard
  with `isBound()` or use `orElse`.
- `public T orElse(T fallback)` — the current binding, or `fallback` when unbound. Never
  throws.
- `public boolean isBound()` — whether a binding is in effect on the current fiber.

## Ownership & lifecycle

- `where(T value, ...)`: transfer is the caller's opt-in (`where(#v, ...)`). Internally a
  `FiberLocalBox<T>` is allocated, owned by the `where` scope, and dropped after the
  binding is popped. When title was tendered the drop frees the box **and its value**;
  when the value was merely lent, only the box is freed and the caller keeps the title —
  and must keep the value alive for the whole `where` extent. The enclosing `scope`'s
  join guarantees any child fiber that inherited the binding has finished before the box
  drops, so an inherited pointer never dangles. **Do not** use a value you transferred
  with `#` past the `where` scope.
- `FiberContext.capture()` returns `#FiberContext` (owned) — you own it; transfer it with
  `#` onto the work item. It deep-copies the chain at capture time, so the producer's
  borrows may go away without affecting the consumer. The snapshot frees on drop
  (`~FiberContext`).
- `ctx.run(body)` installs the snapshot **layered on top of** the consumer fiber's
  current bindings and restores prior state on return OR throw. Request N's context never
  bleeds into request N+1.

## What it does NOT do

- No process-wide default value (post-v1). A fresh key is unbound everywhere; `get()` on
  an unbound fiber throws — reach for `orElse`/`isBound`.
- The binding chain does **not** auto-follow an unstructured boundary (channel/detach) —
  that is what `FiberContext` is for. It **does** auto-follow a structured `spawn` inside
  a `scope`.
- You do not construct or touch `FiberLocalBox<T>`.

## Worked example — bind, inherit on structured spawn, hand off

```cajeta
import cajeta.concurrent.FiberLocal;
import cajeta.concurrent.FiberContext;

// Identity-only key; typically static. T here is a primitive — round-trips fine.
static FiberLocal<int64> REQUEST_ID = heap FiberLocal<int64>();

// Read deep on the call path, no parameter threaded:
int64 rid = REQUEST_ID.orElse(-1);

// Bind for a dynamic scope; a child spawned in the scope inherits it automatically:
async int32 child() {
    return REQUEST_ID.get();          // sees the inherited binding
}
void handle(int64 id) {
    REQUEST_ID.where(id, () -> {       // binding restored on return OR throw
        scope {
            spawn child();             // child inherits REQUEST_ID == id
        }
    });
}

// Unstructured handoff across a Channel to a worker pool:
//   producer (on the request's fiber):
FiberContext ctx #= FiberContext.capture();
channel.send(Job(payload, #ctx));      // # transfers ownership of the snapshot
//   worker fiber:
Optional<Job> j = channel.receive();
j.get().ctx.run(() -> process(j.get().payload));   // sees the producer's bindings
```

See `docs/specification/concurrent/FiberLocal.md` for the layered model
(single-fiber / structured fan-out / unstructured handoff).
