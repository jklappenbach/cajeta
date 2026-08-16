---
id: io-net-server
applies-to: [cajeta/io/net/Server, cajeta/io/net/SharedPoolServer, cajeta/io/net/ServerBuilder, cajeta/io/net/ServerModel, cajeta/io/net/ConnectionLimiter]
title: TCP server core, accept-model selection, and connection backpressure
description: Build/serve/drain a TCP server, choose fiber-per-connection vs shared-pool accept models, and cap concurrent connections.
---

# TCP server core + accept-model selection

To stand up a TCP server, go through the **builder**: `Server.builder().bind(...).model(...).handler(...).serve()`. The builder is the *only* place that decides which of two accept models you get; everything downstream drives one polymorphic `Server` surface (`serve()` / `shutdown()` / `localAddress()`).

Pick the model by what you're optimizing:

- **Model A — fiber-per-connection** (`ServerModel.fiberPerConnection()`, the default): one fiber per accepted socket. Lowest latency, simplest. **Costs ~64 KB stack per connection** — so it does *not* bound memory under huge fan-in. Use for moderate, mostly-active connection counts.
- **Model B — shared-pool** (`ServerModel.sharedPool(n)`): `n` worker fibers drain a bounded work `Channel`; peak concurrent handler fibers stays ≈ `n` no matter how many clients connect. **Bounded memory** under C10K fan-in, at one extra `Channel` hop per turn. Use when many mostly-idle connections fan in and memory matters more than per-connection latency.

`SharedPoolServer extends Server`, so `build()` returns a `Server` either way and the model differences dispatch through the vtable.

## Members and roles

- **`Server`** — the NET-4.1 core: `bind`+`listen`, the `acceptAsync` loop, the `(TcpStream) -> void` handler contract, in-flight accounting, and graceful drain. Also the Model-A dispatch (one fiber per connection).
- **`SharedPoolServer`** — Model B. A thin dispatch-seam override on `Server`: reuses the lifecycle state machine, accept loop, in-flight drain, and bind path unchanged; overrides only `serve` (spawns the worker pool) and `dispatch` (enqueue instead of spawn).
- **`ServerBuilder`** — the fluent selector. A pure value type until `build()` (records choices, no I/O). Obtain via `Server.builder()`.
- **`ServerModel`** — an immutable carrier `{kind, poolSize}` naming which stack to materialize. Factories: `fiberPerConnection()` / `sharedPool(n)`.
- **`ConnectionLimiter`** — a standalone, lock-free permit gate enforcing a max-concurrent-connections cap with a `REFUSE`/`BLOCK` load-shed policy. See the wiring caveat below.

## Construction & ownership

- `Server.builder()` returns `#ServerBuilder` (owned). The `.bind/.model/.handler/.backlog` steps return `this` for chaining; `.bind(addr)` and `.handler(h)` are **required**.
- `build()` returns an owned `#Server` (bound, in `ServerState.NEW`, *not* yet serving). `serve()` builds and runs synchronously.
- Direct factories also exist: `Server.bind(SocketAddress, handler)` and `SharedPoolServer.bind(SocketAddress, poolSize, handler)` both return owned (`#`) and bound.
- **The handler `(TcpStream) -> void` owns the accepted stream and must `close()` it.** The server hands off ownership and does not double-close. A handler that throws is isolated to its own fiber/turn and never tears the server down.
- `dispatch(#TcpStream)` takes ownership transfer of the connection.

## The lifecycle / call sequence

`ServerState` runs one-way: `NEW → RUNNING → DRAINING → STOPPED` (a single `AtomicInt32`, every transition a `compareAndSet`).

1. `serve()` (or `spawn runAsync()`) flips `NEW → RUNNING`. A second `serve()` is a no-op (idempotent guard). The accept loop owns all connection work in one `scope { }`, so its closing `}` joins every connection fiber / worker before returning.
2. `shutdown(Duration deadline)` latches `RUNNING → DRAINING`, closes the listener (which wakes the parked `acceptAsync` so the loop exits), drains in-flight handlers by polling `inflightCount()` to zero (parking the caller fiber, never the carrier) until zero or the deadline, then settles `STOPPED`. Returns `true` on a clean drain, `false` if the deadline forced the stop. `deadline` of `0` ⇒ stop immediately without draining. `shutdown` is idempotent.
3. A `STOPPED` server is **terminal — not restartable**; build a fresh one.

To keep control while serving (the usual shape), `spawn` the loop and hold the reference:

```cajeta
import cajeta.io.net.Server;
import cajeta.io.net.ServerModel;
import cajeta.io.net.TcpStream;
import cajeta.time.Duration;

void handle(#TcpStream conn) {
    // ... read/write ... then close — the handler owns the stream:
    conn.close();
}

Server s #= Server.builder()
    .bind("0.0.0.0:8080")
    .model(ServerModel.sharedPool(8))   // or .fiberPerConnection() (default)
    .handler((conn) -> handle(conn))
    .build();
scope {
    spawn s.runAsync();                  // accept loop (+ workers) on a fiber
    // ... later, from another fiber / signal handler:
    boolean clean = s.shutdown(Duration.ofSeconds(30));
}
```

Port `0` ⇒ an ephemeral port; read it back after `build()` via `s.localAddress()`. A bind to a taken port raises `AddressInUseException`; a malformed address string raises `MalformedAddressException` at `.bind(...)` (configuration) time. `build()` with no address also raises `MalformedAddressException`.

## In-flight vs queued (Model B)

`inflightCount()` counts accepted-but-not-completed connections (queued *or* in a handler) — this is the drain signal. On `SharedPoolServer`, `queuedCount()` is the subset *staged in the work queue awaiting a free worker*, and `workerCount()` is the pool size. A full queue **parks the accept-loop fiber** in `Channel.send` (back-pressure) — surplus connections wait in the kernel `listen` backlog rather than spawning unbounded fibers.

## Connection cap — `ConnectionLimiter`

`ConnectionLimiter` is a semaphore-style permit pool (lock-free over one `AtomicInt32`) used by both models for the `maxConnections` cap:

- `tryAdmit()` — non-blocking; returns `true` having taken one permit, `false` if the cap is full. The `REFUSE` primitive.
- `admit()` — dispatches on the configured `LoadShedPolicy`: `REFUSE` ⇒ one `tryAdmit`; `BLOCK` ⇒ parks the fiber (never the carrier) until a permit frees, always eventually `true`.
- `admitOrThrow()` — always the non-blocking try; raises `TooManyConnectionsException` (carrying the cap) on a full cap, e.g. for an HTTP front-end mapping to 503.
- `release()` — **return exactly one permit per successful admit**, from the handler's `finally` (same `finally` that completes the connection), so a throwing handler still returns its permit. Clamped at `capacity` so a stray double-release can't inflate the pool. A *refused* admission took no permit and must **not** be released.
- `capacity == 0` ⇒ **unbounded**: every `tryAdmit` succeeds, `release` is a no-op. A negative capacity degrades to unbounded rather than refusing everything. Build via `ConnectionLimiter.fromLimits(ConnectionLimits)`; `ConnectionLimits.defaults()` is 1024 conns, 128 backlog, 64 KiB read/write caps, `REFUSE`.

**Wiring caveat (does NOT happen automatically):** the current `Server`/`SharedPoolServer` `dispatch` do *not* call `admit`/`release`, and `ServerBuilder` does not thread a `ConnectionLimits`/`ConnectionLimiter` through. The limiter is a standalone gate — to enforce a cap today you call `admit()` / `release()` yourself around your handler. Do not assume `.handler(...)` is rate-limited by default.

## What this component does NOT do

- No request parsing or protocol — this is raw TCP. For HTTP routing use the external `dev.cajeta.http` library's `HttpServer` (which runs the same handler shape on either model).
- No automatic `close()` of the accepted stream — the handler owns and closes it.
- No restart of a `STOPPED` server.
- No backlog/limits wiring from the builder yet (see caveat above).

## See also

- `cajeta/io/net/TcpListener`, `cajeta/io/net/TcpStream` — the bound socket and per-connection stream the handler receives.
- `cajeta/io/net/ServerState`, `cajeta/io/net/LoadShedPolicy`, `cajeta/io/net/ConnectionLimits` — the ordinals/config carried above.
- `cajeta/concurrent/Channel`, `cajeta/concurrent/AtomicInt32` — Model B's work queue and the shared counters.
