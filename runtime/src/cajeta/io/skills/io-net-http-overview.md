---
id: io-net-http-overview
applies-to: [cajeta/io/net/http]
title: cajeta.io.net.http — HTTP/1.1 server, client, messages, codec, limits
description: Neighborhood map for HTTP/1.1 — pick HttpServer/Router (serve), HttpClient (call out), the message data types, or the incremental codec; ownership + the HttpException family.
---

# cajeta.io.net.http — HTTP/1.1

The HTTP/1.1 slice of `cajeta.io.net`: an async **server** stack, an async
**client**, the pure-data **message** types they exchange, the incremental
**codec** that turns bytes ⇄ messages, the abuse/hardening **limits**, and the
`HttpException` fault family. Everything above the codec is layered over the
NET transport (`Server`, `TcpStream`, `AsyncReader`/`AsyncWriter`,
`TlsStream`) — this package adds no new socket code, only HTTP framing.

**HTTP/1.1 only.** No HTTP/2 or HTTP/3. Sibling packages own the adjacent
protocols, do not look here for them: WebSocket → `cajeta/io/net/ws`, TLS →
`cajeta/io/net/tls`, URI parsing → `cajeta/io/net/uri`. `Headers` lives one
level up in `cajeta/io/net` (it is shared), not in this package.

## Task → entry point

| You want to… | Start with |
| --- | --- |
| Run an HTTP server | `HttpServer.bind(addr, handler)` then `serve()`/`spawn runAsync()` |
| Pick the accept model (fiber-per-conn vs shared pool) | `HttpServer.builder()….model(ServerModel.sharedPool(n))….build()` |
| Route by `(method, /path/{param})` | `Router` — mount as `(req) -> router.dispatch(req)` |
| Make an outbound request | `heap HttpClient()` then `.get(url)` / `.send(uri, req)` |
| Serve over HTTPS | bind a `TlsListener` (tls pkg), feed accepted streams to `HttpServer.serveTlsStream` |
| Test server logic with no socket | `HttpServer.handleRequestBytes(handler, bytes, len)` → response wire bytes |
| Parse/serialize bytes yourself | `HttpParser` + `BodyReader` (read) · `HttpSerializer` (write) |
| Build/inspect a request or response | `HttpRequest` / `HttpResponse` (over `Headers`) |
| Cap body size / add slowloris deadlines / `Expect: 100-continue` | `ServerLimits` on the server; head limits via `HttpParserLimits` |

Not here: no middleware/filters, no content negotiation, no regex/wildcard
routes (the `Router` is deliberately minimal). The client v1 is **one exchange
per socket** (no connection pool/keep-alive reuse) and buffers the whole
response body in memory. There is no streaming/chunk-at-a-time *handler* yet.

## Inventory

**Entry-point types (you instantiate / call):**
- `HttpServer` + `HttpServerBuilder` — the accept-loop + keep-alive request
  loop; handler is `(HttpRequest) -> #HttpResponse`. See its class skill.
- `Router` — first-match `(method, path-pattern)` dispatch with `/users/{id}`
  path params; returns the `404`/`405` defaults itself.
- `HttpClient` — single request/response exchange, `http://` and `https://`.

**Message data types (pure values, golden-testable, no socket):**
- `HttpRequest`, `HttpResponse` — fluent builders over a `Headers`
  (`cajeta/io/net/Headers`); fields are public, `getX()`/classifier accessors
  too. `PathParams` carries a request's bound route params.

**Codec (incremental, feed-then-drain):**
- `HttpParser` — head parser (`forRequest`/`forResponse`); `feed` → bytes,
  `isComplete`, `getRequest`/`getFraming`, `leftover()` (body bytes that
  arrived with the head terminator).
- `BodyReader` — body decoder for a `BodyFraming` (content-length / chunked /
  close-delimited / none).
- `HttpSerializer` — message → wire bytes (`request`/`response`, plus the
  `…Chunked` variants); `ChunkedEncoder` is its chunk framer.
- `BodyFraming` (which body model), `KeepAlive` (reuse decision +
  `Connection` header), `ExpectContinue` (the `100-continue`/`413`/`417`
  decision), `Exchange` (`response` + `keepAlive` pair the server loop
  returns).

**Limits / support:** `HttpParserLimits` (max head bytes, line length, header
count → `431`), `ServerLimits` (head/body read deadlines, `maxBodyBytes` →
`413`, expect-continue toggle).

**Exception family** — root `HttpException` (`extends NetException`,
`kind == KIND_INVALID` = 12). The status a server returns is the **virtual**
`httpStatus()` on the leaf (no RTTI):
- `MalformedMessageException` → `400` · `UnexpectedEofException` → `400` ·
  `InvalidChunkEncodingException` → `400`
- `HeadersTooLargeException` → `431` · `PayloadTooLargeException` → `413`

`NetException extends RecoverableException`, so every parse/exchange call site
must `catch (HttpException e)` (or a leaf, or `NetException`) or declare it.

## How they collaborate

The server loop is the canonical wiring: per connection it runs a fresh
`HttpParser.forRequest`, feeds reader bytes until `isComplete()`, reads
`getRequest()`/`getFraming()`, **seeds a `BodyReader.forFraming(framing)` with
`parser.leftover()`** then pulls more until `body.isComplete()`, attaches the
decoded bytes via `request.body(...)`, calls the handler, serializes the
returned `#HttpResponse` with `HttpSerializer.response`, and decides reuse with
`KeepAlive.canReuse`. A parse fault is mapped to a status response by the
leaf's `httpStatus()` and the connection is closed; a handler *throw* becomes
`500` but keeps the connection. `HttpClient` runs the same parser/body-reader
chain on the response side. **Always seed the `BodyReader` with the parser
leftover** — those bytes are already consumed off the wire and lost otherwise.

## Ownership & lifecycle (package-wide)

- `#` on a type is an **ownership transfer**. `HttpServer.bind` /
  `HttpClient.get`/`send` return `#…` (owned — you hold it). The handler
  `(HttpRequest) -> #HttpResponse` **borrows** the request and **returns an
  owned** response.
- `HttpRequest.body(data, len)` / `HttpResponse.body(data, len)` **copy** into
  a fresh owned `int8[]` — your source buffer is not transferred and stays
  yours. `getHeaders()` returns the **live** header map (mutating it mutates
  the message). `pathParam(name)` returns an **owned** `#String` (or `null`).
- `HttpParser` / `BodyReader` parse **exactly one** message — construct a fresh
  one per request on a keep-alive connection (no implicit reset across
  messages). They feed-then-drain; `drain()`/`leftover()` hand back owned
  `int8[]`.
- Server stream ownership: `serveConnection` **borrows** the `TcpStream` (the
  NET-4 `Server` core owns and closes it — do not close it yourself).
  `serveTlsStream` **takes ownership** of its `#TlsStream` and closes it.
- `HttpServer.shutdown(Duration)` drains gracefully; returns `true` if all
  connections drained in time.

## Idiomatic example — serve, with imports

```cajeta
import cajeta.io.net.http.HttpServer;
import cajeta.io.net.http.HttpResponse;
import cajeta.time.Duration;

HttpServer srv = HttpServer.bind("0.0.0.0:8080", (req) -> {
    return HttpResponse.ok()
        .header("Content-Type", "text/plain")
        .body(req.body, req.bodyLength());     // echo; body() copies the bytes
});
scope {
    spawn srv.runAsync();
    // ... later, graceful drain:
    srv.shutdown(Duration.ofSeconds(30));
}
```

Routing mounts as a plain handler (the lambda is written at the call site,
where the `Router` local is captured by borrow — `Router` exposes `dispatch`
directly, not a closure factory):

```cajeta
import cajeta.io.net.http.Router;
import cajeta.io.net.http.HttpResponse;

Router r = heap Router();
r.route("GET", "/users/{id}", (req) -> HttpResponse.of(200).body(...));
HttpServer srv = HttpServer.bind("0.0.0.0:8080", (req) -> r.dispatch(req));
// inside a handler: String id = req.pathParam("id");  // percent-decoded, or null
```

Outbound:

```cajeta
import cajeta.io.net.http.HttpClient;

HttpClient client = heap HttpClient();
HttpResponse resp = client.get("https://example.test/path");  // follows up to 10 redirects
// resp.statusCode(), resp.getHeaders(), resp.body
```

## Deeper

Read the per-class skills for construction/lifecycle detail: `HttpServer`
(builder, both accept models, the hardened limits loop), `Router`,
`HttpClient` (trust anchors, redirects), `HttpParser` + `BodyReader`
(feed/drain protocol), `HttpSerializer`. Transport ownership rules live in the
`cajeta/io/net` library skill; `Headers` has its own.
