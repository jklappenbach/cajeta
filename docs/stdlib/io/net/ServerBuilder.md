# ServerBuilder

`cajeta.io.net.ServerBuilder` — the model-selection builder for a
[Server](Server.md), opened with `Server.builder()`. It is the single fluent
surface for picking which accept stack a server uses — Model A
(fiber-per-connection, the default) or Model B (event-driven shared-pool, via
`ServerModel.sharedPool(n)`) — without naming the concrete server class.
`bind` (or `bindAddress`) and `handler` are required; `backlog` defaults to
the platform listen backlog (`UNSET_BACKLOG`). The builder is a pure value
until `build()` is called — no I/O happens while recording choices.

```cajeta
ServerBuilder b #= Server.builder()
    .bind("127.0.0.1:0")
    .model(ServerModel.fiberPerConnection())
    .handler((TcpStream conn) -> { conn.close(); });
Server s #= b.build();
```

## Methods

| Signature | |
|---|---|
| `ServerBuilder bind(String addr)` | Bind to a `host:port` (or `[v6]:port`) string, parsed via [`SocketAddress.parse`](SocketAddress.md) |
| `ServerBuilder bindAddress(SocketAddress addr)` | Bind to an already-parsed [SocketAddress](SocketAddress.md) |
| `ServerBuilder model(ServerModel m)` | Select the accept model — `ServerModel.fiberPerConnection()` (Model A) or `ServerModel.sharedPool(n)` (Model B) |
| `ServerBuilder handler((TcpStream) -> void h)` | Set the per-connection handler, invoked once per accepted connection with the owned [TcpStream](TcpStream.md) |
| `ServerBuilder backlog(int32 backlog)` | Set an explicit `listen(2)` backlog (the accept-queue depth) |
| `#Server build()` ⚑ | Build the configured [Server](Server.md); either model returns a `Server`, so the caller drives the same `serve()` / `shutdown()` surface |
| `void serve()` | Terminal convenience: `build` the selected server, then run its accept loop synchronously |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/io/net/ServerBuilder.cajeta`](../../../../runtime/src/cajeta/io/net/ServerBuilder.cajeta)
- [Server](Server.md) — what it builds; [HttpServerBuilder](http/HttpServerBuilder.md) — the HTTP-layer counterpart
