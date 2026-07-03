# Server

`cajeta.io.net.Server` — the TCP server core: `bind` + `listen` over
[TcpListener](TcpListener.md), an accept loop over `acceptAsync`, a connection
handler contract, and graceful shutdown (stop accepting, drain in-flight
connections with a deadline). The handler is `(TcpStream) -> void`: it owns the
accepted connection and closes it when done; a handler that throws does not
tear down the server. A single atomic state runs the lifecycle machine
(`NEW → RUNNING → DRAINING → STOPPED`), and an atomic in-flight count is what
`shutdown` drains to zero. This core is the scaffolding both accept models
build on — Model A (fiber-per-connection, the default) ships here; Model B
(shared-pool) re-homes `dispatch` and is selected via
[ServerBuilder](ServerBuilder.md).

```cajeta
SocketAddress addr = SocketAddress.parse("127.0.0.1:0");
Server s = Server.bind(addr, (TcpStream conn) -> { conn.close(); });
s.serve();                                   // accept loop, until shutdown
boolean drained = s.shutdown(Duration.ofSeconds(30L));
```

## Methods

| Signature | |
|---|---|
| `static #ServerBuilder builder()` ⚑ | Open a [ServerBuilder](ServerBuilder.md) — the fluent model-selection surface |
| `static #Server bind(SocketAddress addr, (TcpStream) -> void handler)` ⚑ | Bind with the default listen backlog; the result is `NEW` — call `serve` (or spawn `runAsync`) to start |
| `static #Server bindWithBacklog(SocketAddress addr, int32 backlog, (TcpStream) -> void handler)` ⚑ | Bind with an explicit `listen(2)` backlog |
| `void serve()` | Run the accept loop synchronously on the calling fiber until `shutdown` latches the state out of `RUNNING` |
| `async int32 runAsync()` | The async sibling of `serve` — spawn it so the accept loop runs on its own fiber |
| `#TcpStream acceptNext()` | Accept the next connection, or `null` once the listener has been closed by `shutdown` |
| `void dispatch(#TcpStream conn)` | Dispatch one accepted connection to the handler (bumps the in-flight count; drops it in a `finally`) |
| `static async int32 serveConnection((TcpStream) -> void handler, AtomicInt32 inflight, #TcpStream conn)` | Run the handler for one connection, then drop the in-flight count — even if the handler throws |
| `boolean shutdown(Duration deadline)` | Graceful shutdown: stop accepting, close the listener, drain in-flight with `deadline`; idempotent |
| `boolean drainInflight(Duration deadline)` | Park the fiber until the in-flight count reaches zero or `deadline` elapses |
| `#SocketAddress localAddress()` | The bound local address (carries the kernel-assigned port for a `:0` bind) |
| `int32 currentState()` | The lifecycle state ordinal (`NEW` / `RUNNING` / `DRAINING` / `STOPPED`) |
| `boolean isRunning()` | `true` while the accept loop is live |
| `int32 inflightCount()` | Connection handlers dispatched but not yet returned |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/io/net/Server.cajeta`](../../../../runtime/src/cajeta/io/net/Server.cajeta)
- [ServerBuilder](ServerBuilder.md) — model selection; [TcpListener](TcpListener.md) — the socket underneath; [HttpServer](http/HttpServer.md) — the HTTP layer over this core
