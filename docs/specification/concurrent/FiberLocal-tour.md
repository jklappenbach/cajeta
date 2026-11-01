# Tour: ambient request state with `FiberLocal`

This tour walks you from threading a context parameter through every function to
having the request's state follow the work, across fan-out and across a worker
handoff, without a single extra parameter. It assumes you have met `scope` and
`spawn` from the concurrency tour. The spec is `docs/specification/concurrent/FiberLocal.md`.

The running example is a request handler that wants one thing visible everywhere
on the request's path, a request id, which is the same field a structured logger
stamps on every line. We will grow it through the three layers.

## 0. The pain we're removing

```cajeta
// Without FiberLocal: ctx rides every signature, forever.
Response handle(Request r, ReqCtx ctx) {
    User u = loadUser(r.userId, ctx);     // ctx
    Cart c = loadCart(r.userId, ctx);     // ctx
    return render(u, c, ctx);             // ctx
}
```

Every function below `handle` takes `ctx` only to pass it down. We want `ctx` to be
ambient, meaning set once, readable anywhere, and gone when the request ends.

## 1. One fiber owns the request — `where` + `get`

Declare a binding key once at module level, bind it for the request's extent, and
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

`where` binds `REQUEST_ID` for the dynamic extent of the closure and restores
the prior value when the closure returns or throws, so you cannot forget to clear
it. `get()` reads the current binding. That is the whole single-fiber story.

> **`where` returns `void` in v1.** The body's result comes back through a heap
> holder it writes, as above. A value-returning
> `<R> where(...)` is designed but deferred (spec § 4), and the holder is the idiom
> until then.

> **Why no `set`?** v1 has no imperative `set`/`remove`, only the scoped
> `where`. `set` would lean on the fiber ending to clean up, while `where`
> guarantees the restore structurally, since it rides the drop chain like
> `Mutex.withLock`, so "forgot to clear it" is unrepresentable.

## 2. The request fans out — inheritance is automatic

Now `handle` runs its sub-loads in parallel. You do nothing extra, because children
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
`spawn` as copy-on-write, so a child that re-binds with its own `where` does not
disturb its sibling or the parent. It is safe because `scope` does not exit until
both children finish, so the inherited context cannot outlive its owner.

## 3. The request is handed off — carry a `FiberContext`

Sometimes the request leaves its fiber entirely. You push it onto a queue and a
worker-pool fiber picks it up later. That worker serves many requests over
its life, so the ambient chain must not auto-follow, and you carry it explicitly:

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

`FiberContext.capture()` grabs whatever is bound right now. `#`-transferring it
makes the snapshot a single-owner value that crosses the boundary cleanly. The
instance method `ctx.run(body)` installs it for just one job and restores the
worker's own empty state afterward, so request N's id never bleeds into
request N+1.

## 4. When you'd reach for an ID cache instead (rare)

Some boundaries can carry only a string, such as an FFI call into C or a message
serialized to another process, and the live `FiberContext` object cannot travel
across them. Put the correlation id in the message, and have the receiver rebuild
context from it via your own `id → context` map. That is the one case where the
registry pattern earns its keep. Everywhere else, carrying the context directly
(steps 1–3) is simpler and leak-free. See the spec's "request-ID cache" note for
why it is the escape hatch.

## 5. What you learned

| You have | Use |
|---|---|
| One fiber, whole request | `where(value, body)` + `get()` (step 1) |
| Parallel fan-out under a `scope` | nothing — inheritance is automatic (step 2) |
| Handoff to an unrelated/worker fiber | `FiberContext.capture()` + `#`send + `run` (step 3) |
| A boundary that carries only a string | correlation id + your own registry (step 4) |

The same `FiberLocal<SystemLogEntry>` is how the logging framework keeps one
system-log event per request consistent across all three shapes. That is why
this primitive lands first.
