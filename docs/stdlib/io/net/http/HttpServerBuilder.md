# HttpServerBuilder

`cajeta.io.net.http.HttpServerBuilder` — fluent builder for an
[HttpServer](HttpServer.md), opened with `HttpServer.builder()`. It records a
bind address (`host:port` string, parsed at `build` time), an accept model
(fiber-per-connection by default, or a shared pool via `sharedPool(n)`), and
the request handler, then `build()` materializes the server —
`serve()` is the build-then-run one-shot.

```cajeta
HttpServerBuilder b = HttpServer.builder()
    .bind("0.0.0.0:8080")
    .model(ServerModel.sharedPool(8))
    .handler((HttpRequest req) -> HttpResponse.of(200));
ServerModel m = b.selectedModel();    // shared-pool, size 8
```

## Methods

| Signature | |
|---|---|
| `HttpServerBuilder()` ⚑ | Start with the default (fiber-per-connection) model and no address/handler |
| `HttpServerBuilder bind(String addr)` | Set the bind address from a `host:port` string (parsed at `build` time) |
| `HttpServerBuilder model(ServerModel model)` | Pick the accept model — Model A (fiber-per-connection) or Model B (shared-pool) |
| `HttpServerBuilder fiberPerConnection()` | Shorthand for `model(ServerModel.fiberPerConnection())` — the default |
| `HttpServerBuilder sharedPool(int32 poolSize)` | Shorthand for `model(ServerModel.sharedPool(poolSize))` |
| `HttpServerBuilder handler((HttpRequest) -> #HttpResponse handler)` | Set the request handler |
| `ServerModel selectedModel()` | The accept model this builder will materialize on `build` |
| `#HttpServer build()` | Materialize the [HttpServer](HttpServer.md) on the accumulated address + model + handler |
| `void serve()` | `build` the server, then run its accept loop synchronously |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/io/net/http/HttpServer.cajeta`](../../../../../runtime/src/cajeta/io/net/http/HttpServer.cajeta) (declared alongside `HttpServer`)
- [HttpServer](HttpServer.md) — what it builds; [ServerBuilder](../ServerBuilder.md) — the TCP-core counterpart
