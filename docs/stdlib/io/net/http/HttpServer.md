# HttpServer

`cajeta.io.net.http.HttpServer` — the HTTP/1.1 server. It layers a typed
request/response handler loop over the [Server](../Server.md) TCP core: per
accepted connection it reads and parses requests, dispatches each to the
handler, writes the response, and keeps the connection alive when the
keep-alive policy permits. The handler is `(HttpRequest) -> #HttpResponse`; a
handler that throws maps to a `500` rather than tearing down the connection.
The same protocol loop runs on either accept model — Model A
(fiber-per-connection, the default) or Model B (shared-pool) — selected via
`bindWithModel` or the fluent [builder](HttpServerBuilder.md). The `serve*` and
`handleRequest*` statics are the loop's factored layers: the byte-level forms
run the exact protocol path without a socket, which is how the server
golden-tests.

```cajeta
HttpServer srv = HttpServer.bind("127.0.0.1:8080",
    (HttpRequest req) -> HttpResponse.of(200));
srv.serve();                                    // accept loop, until shutdown
boolean stopped = srv.shutdown(Duration.ofSeconds(30L));
```

## Methods

| Signature | |
|---|---|
| `static #HttpServer bind(String addr, (HttpRequest) -> #HttpResponse handler)` ⚑ | Bind on a `host:port` string with the default accept model (Model A) |
| `static #HttpServer bindAddress(#SocketAddress addr, (HttpRequest) -> #HttpResponse handler)` | Bind on an already-parsed [SocketAddress](../SocketAddress.md), default model |
| `static #HttpServer bindWithModel(String addr, ServerModel model, (HttpRequest) -> #HttpResponse handler)` ⚑ | Bind on the accept stack `model` selects |
| `static #HttpServer bindAddressWithModel(#SocketAddress addr, ServerModel model, (HttpRequest) -> #HttpResponse handler)` | The model-aware bind on a parsed address |
| `void serve()` | Run the accept loop synchronously (delegates to the TCP core) |
| `async int32 runAsync()` | The async sibling of `serve` — spawn it so the accept loop runs on its own fiber |
| `boolean shutdown(Duration deadline)` | Graceful shutdown: stop accepting + drain in-flight connections with `deadline` |
| `#SocketAddress localAddress()` | The bound local address (carries the ephemeral port for a `:0` bind) |
| `ServerModel serverModel()` | The accept model this server was bound on |
| `static #HttpServerBuilder builder()` ⚑ | A fluent [builder](HttpServerBuilder.md) for an `HttpServer` |
| `static void serveConnection((HttpRequest) -> #HttpResponse handler, HttpParserLimits limits, TcpStream conn)` | Serve one accepted connection: loop reading + handling requests while keep-alive permits |
| `static void serveLoop((HttpRequest) -> #HttpResponse handler, HttpParserLimits limits, AsyncReader reader, AsyncWriter writer)` | The keep-alive request loop over a buffered reader/writer |
| `static void serveConnectionWithLimits((HttpRequest) -> #HttpResponse handler, HttpParserLimits parseLimits, ServerLimits limits, TcpStream conn)` | The hardened per-connection loop: threads `ServerLimits` (deadlines, body cap, expect policy) through |
| `static void serveTlsStream((HttpRequest) -> #HttpResponse handler, HttpParserLimits parseLimits, ServerLimits limits, #TlsStream tls)` | The HTTPS per-connection worker over an already-handshaked TLS stream (from [TlsListener](../tls/TlsListener.md)) |
| `static void serveLoopWithLimits((HttpRequest) -> #HttpResponse handler, HttpParserLimits parseLimits, ServerLimits limits, AsyncReader reader, AsyncWriter writer)` | The hardened keep-alive loop |
| `static #int8[] handleRequestBytes((HttpRequest) -> #HttpResponse handler, int8[] requestBytes, int32 length)` | Pure request→response over byte buffers — the golden-vector test entry point |
| `static #Exchange handleRequest(HttpParserLimits limits, (HttpRequest) -> #HttpResponse handler, int8[] requestBytes, int32 length)` | The structured form: parse + body-read + dispatch + keep-alive over an in-memory buffer |
| `static #Exchange handleRequestWithLimits(HttpParserLimits parseLimits, ServerLimits limits, (HttpRequest) -> #HttpResponse handler, int8[] requestBytes, int32 length)` | The hardened byte path: additionally enforces `ServerLimits` (`413`/`417` short-circuits) |
| `static int32 expectAction(HttpParserLimits parseLimits, ServerLimits limits, int8[] requestBytes, int32 length)` | The `Expect: 100-continue` action for a request head under `limits` |
| `static #int8[] continueResponse()` | The interim `100 Continue` wire bytes flushed before reading a body |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/io/net/http/HttpServer.cajeta`](../../../../../runtime/src/cajeta/io/net/http/HttpServer.cajeta)
- [HttpServerBuilder](HttpServerBuilder.md) — fluent construction; [Router](Router.md) — path-pattern dispatch for handlers; [Server](../Server.md) — the TCP core; [HttpClient](HttpClient.md) — the client side
