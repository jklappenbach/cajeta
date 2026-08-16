# Tour: ambient request state with `FiberLocal`

This walks you from "I'm threading a context parameter through every function" to
"the request's state follows the work — across fan-out and across a worker
handoff — without a single extra parameter." It assumes you've met `scope` /
`spawn` from the concurrency tour. Spec: `docs/specification/concurrent/FiberLocal.md`.

The running example is a request handler that wants one thing visible everywhere
on the request's path: a **request id** (the same field a structured logger
stamps on every line). We'll grow it through the three layers.

## 0. The pain we're removing

```cajeta
// Without FiberLocal: ctx rides every signature, forever.
Response handle(Request r, ReqCtx ctx) {
    User u = loadUser(r.userId, ctx);     // ctx
    Cart c = loadCart(r.userId, ctx);     // ctx
    return render(u, c, ctx);             // ctx
}
```

Every function below `handle` takes `ctx` only to pass it down. We want `ctx`
**ambient** — set once, readable anywhere, gone when the request ends.

## 1. One fiber owns the request — `where` + `get`

Declare a binding key once (module-level), bind it for the request's extent, and
read it wherever you need it:

```cajeta
import cajeta.concurrent.FiberLocal;

static FiberLocal<String> REQUEST_ID = heap FiberLocal<String>();

Response handle(Request r) {
    Holder<Response> out = heap Holder<Response>();   // v1 `where` is void — the
    REQUEST_ID.where(r.id, () -> {                     // body writes its result
        User u = loadUser(r.userId);                   // into a small heap holder
        Cart c = loadCart(r.userId);                   // (see the note below).
        out.value = render(u, c);
    });
    return out.value;
}

// Anywhere on the call path, however deep:
void loadUser(int64 userId) {
    log.info("loading user " + userId + " for req " + REQUEST_ID.get());
    // ...
}
```

`where` binds `REQUEST_ID` for the dynamic extent of the closure and **restores
the prior value when the closure returns or throws** — you can't forget to clear
it. `get()` reads the current binding. That's the whole single-fiber story.

> **`where` returns `void` in v1.** The body's result comes back through a heap
> holder it writes (above), not as a return value of `where`. A value-returning
> `<R> where(...)` is designed but deferred (spec § 4); the holder is the idiom
> until then.

> **Why no `set`?** v1 has **no** imperative `set`/`remove` — only scoped
> `where`. `set` would lean on the fiber ending to clean up, while `where`
> guarantees restore structurally (it rides the drop chain, like
> `Mutex.withLock`), so "forgot to clear it" is unrepresentable.

## 2. The request fans out — inheritance is automatic

Now `handle` runs its sub-loads in parallel. You do **nothing** extra — children
spawned inside a `scope` inherit the binding:

```cajeta
Response handle(Request r) {
    Holder<Response> out = heap Holder<Response>();
    REQUEST_ID.where(r.id, () -> {
        Mutex<Parts> parts = heap Mutex(Parts());
        scope {
            spawn () -> async void {
                User u = loadUser(r.userId);   // REQUEST_ID.get() == r.id  ✅
                parts.withLock((p) -> p.withUser(u));
            };
            spawn () -> async void {
                Cart c = loadCart(r.userId);    // REQUEST_ID.get() == r.id  ✅
                parts.withLock((p) -> p.withCart(c));
            };
        }
        out.value = render(parts.get());
    });
    return out.value;
}
```

Both child fibers see `r.id`. The binding is snapshotted into each child at
`spawn` (copy-on-write — a child that re-binds with its own `where` doesn't
disturb its sibling or the parent). It's safe because `scope` doesn't exit until
both children finish, so the inherited context can't outlive its owner.

## 3. The request is handed off — carry a `FiberContext`

Sometimes the request leaves its fiber entirely: you push it onto a queue and a
**worker-pool** fiber picks it up later. That worker serves many requests over
its life, so the ambient chain must *not* auto-follow — you carry it explicitly:

```cajeta
import cajeta.concurrent.FiberContext;

// Producer (on the request's fiber): snapshot the live bindings, send them along.
void enqueue(Channel<Job> q, Payload p) {
    FiberContext ctx #= FiberContext.capture();   // immutable snapshot
    q.send(Job(p, #ctx));                         // # moves it across the boundary
}

// Worker fiber: reinstall the snapshot just for this job, then it's gone again.
async void worker(Channel<Job> q) {
    Optional<Job> j = q.receive();
    while (j.isPresent()) {
        Job job = j.get();
        job.ctx.run(() -> {
            process(job.payload);     // REQUEST_ID.get() is the producer's id  ✅
        });                            // <- worker's prior (empty) state restored
        j = q.receive();
    }
}
```

`FiberContext.capture()` grabs whatever's bound right now; `#`-transferring it
makes the snapshot a single-owner value that crosses the boundary cleanly; and
`ctx.run(body)` (instance method) installs it for just one job and restores the
worker's own (empty) state afterward — so request N's id never bleeds into
request N+1.

## 4. When you'd reach for an ID cache instead (rare)

If the boundary can carry only a **string** — an FFI call into C, or a message
serialized to another process — the live `FiberContext` object can't travel. Put
the correlation id in the message, and have the receiver rebuild context from it
via your own `id → context` map. That's the one case the registry pattern earns
its keep; everywhere else, carrying the context directly (steps 1–3) is simpler
and leak-free. See the spec's "request-ID cache" note for why it's the escape
hatch, not the default.

## 5. What you learned

| You have | Use |
|---|---|
| One fiber, whole request | `where(value, body)` + `get()` (step 1) |
| Parallel fan-out under a `scope` | nothing — inheritance is automatic (step 2) |
| Handoff to an unrelated/worker fiber | `FiberContext.capture()` + `#`send + `run` (step 3) |
| A boundary that carries only a string | correlation id + your own registry (step 4) |

The same `FiberLocal<SystemLogEntry>` is how the logging framework keeps one
system-log event per request consistent across all three shapes — which is why
this primitive lands first.
