---
id: io-net-http-server
applies-to: [cajeta/io/net/http/HttpServer, cajeta/io/net/http/Router, cajeta/io/net/http/Route, cajeta/io/net/http/Exchange]
title: HTTP/1.1 server stack — connection loop, router, route matching, exchange
description: Wire an HTTP server: HttpServer keep-alive loop + Router method/path dispatch (/users/{id}, 404/405) + Exchange verdict.
---

# HTTP/1.1 server stack

Use this when you need to **stand up an HTTP/1.1 server and route requests**. Four
cooperating classes:

- **`HttpServer`** — the entry point. The keep-alive **connection loop** over a
  `ByteChannel` (plaintext `TcpStream` or terminated TLS `TlsStream`): read+parse a
  request, run your handler, write the response, reuse-or-close. Construct via
  `HttpServer.bind(...)` / `.builder()`. The handler is plain `(HttpRequest) -> #HttpResponse`.
- **`Router`** — the dispatch primitive you **mount inside a handler**. Maps
  `(method, path)` to a registered handler by literal + `{param}` segment matching, with
  `404`/`405` defaults. Construct with `heap Router()`; register with `route(...)`.
- **`Route`** — one compiled `(method, pattern, handler)` triple inside a `Router`. You
  **do not construct it** — `Router.route(...)` builds it. Internal; rarely referenced.
- **`Exchange`** — a tiny immutable carrier (`response` + `keepAlive` verdict) the
  pure-logic byte path returns so a test can inspect both off one call. You receive it,
  you do not normally build it.

## Object graph & who owns whom

```
HttpServer ──holds──> Server core (NET-4)  // owns each accepted TcpStream, closes it
   │                                        // on the borrowed conn — your loop must NOT close it
   └─ handler: (HttpRequest) -> #HttpResponse   // YOU supply this; returns an OWNED response

Router ──holds──> Route[]   // each Route pre-splits its pattern into match segments
   dispatch(req): per candidate Route → fresh PathParams → tryMatch →
                  on full match: req.bindPathParams(#params); return route.handler(req)
```

`Router` is **not** a field of `HttpServer`. They meet at the handler boundary: the
handler `HttpServer` runs is just `(req) -> theRouter.dispatch(req)`. (Escaping closures
that capture a borrow are a pending runtime item, so `Router` exposes `dispatch` directly
rather than a closure factory — write the forwarding lambda at the call site where the
router is a live local.)

## Ownership across the boundary (read this first)

- **Handler return is owned** — your handler returns `#HttpResponse`; the loop takes it.
  `Router.dispatch` likewise returns `#HttpResponse` (owned).
- **`bind*` returns `#HttpServer`** (owned). `Exchange(#HttpResponse, boolean)` **pins**
  (takes ownership of) the response.
- **Plaintext conn is borrowed** — the `Server` core owns the accepted `TcpStream` and
  closes it when the per-connection fiber unwinds; `serveConnection` must not close it
  (no double-close).
- **TLS conn is owned** — `serveTlsStream(#TlsStream)` takes ownership and `close()`s it
  (flushing `close_notify`) after the loop. TLS is pure termination above the
  `ByteChannel` seam; the HTTP loop is byte-for-byte identical to plaintext.
- **Path params transfer** — `dispatch` calls `request.bindPathParams(#params)`, moving
  the captured map onto the request; the handler reads `req.pathParam("id")`.

## Routing & decision rules

A pattern matches a path **iff equal segment count and every segment matches** — a
literal byte-equals the request segment (case-sensitive), a `{name}` captures any
**non-empty** segment (percent-decoded). `dispatch`:
1. splits the target (drops query/fragment, trims empty segments; `"/"` → 0 segments),
2. walks routes in **registration order**; first route whose path **and** method match
   wins (path params bound, handler invoked),
3. else if some route's **path** matched but not the method → `405` with an `Allow`
   header listing the methods for that path,
4. else → `404`.

A captured segment with a malformed `%XX` escape is treated as a **non-match**, not an
error.

## What this stack does NOT do

- **No web-framework features** — no middleware chain, no wildcards/regex/optional
  segments, no route groups, no content negotiation. Layer those yourself.
- **No streaming bodies** here — the buffered request body (head + full body in memory)
  is the baseline; chunk-at-a-time streaming is a separate concern.
- A **handler throw never crashes the connection** — it maps to `500` and (if the
  request parsed clean) keeps keep-alive. A handler returning `null` → `500`.
- **Hardening lives on the `*WithLimits` paths** — body-size cap (`413`),
  `Expect: 100-continue` (`100`/`417`), and head/body read deadlines (slowloris) are
  enforced by `serveConnectionWithLimits` / `handleRequestWithLimits`, not the bare
  `serveConnection` / `handleRequest` primitives (those stay limit-free for tests).

## Worked example — server mounting a router

```cajeta
import cajeta.io.net.http.HttpServer;
import cajeta.io.net.http.Router;
import cajeta.io.net.http.HttpRequest;
import cajeta.io.net.http.HttpResponse;
import cajeta.lang.String;
import cajeta.time.Duration;

Router r = heap Router();
r.route("GET", "/users/{id}", (req) -> {
    String id = req.pathParam("id");                 // bound by dispatch, percent-decoded
    return HttpResponse.ok().header("Content-Type", "text/plain").body(id.bytes, id.byteLength);
});
r.route("GET", "/health", (req) -> HttpResponse.of(204));   // chainable: returns the Router

// Mount: the forwarding lambda is the (HttpRequest) -> #HttpResponse the server runs.
HttpServer srv = HttpServer.bind("0.0.0.0:8080", (req) -> r.dispatch(req));
scope {
    spawn srv.runAsync();          // accept loop on its own fiber
    // ... serve ...
    srv.shutdown(Duration.ofSeconds(30));   // graceful drain
}
```

Shared-pool model instead of fiber-per-connection: `HttpServer.builder().bind(addr)
.model(ServerModel.sharedPool(8)).handler(h).build()` — the HTTP loop is identical on
both accept models.

## Testing without a socket

`HttpServer.handleRequest(limits, handler, requestBytes, length)` is a pure function that
parses → reads body → dispatches → decides keep-alive and returns an **`Exchange`**;
inspect `ex.getResponse()` (or `.response`) and `ex.isKeepAlive()`. `handleRequestBytes`
is the same path returning serialized wire bytes. `Router.dispatch` is itself pure logic
over `HttpRequest`/`HttpResponse`, so route tests build a request, register routes, and
assert status + bound param with no socket.

For per-type depth see the class docs: `HttpServer.cajeta`, `Router.cajeta`,
`Route.cajeta`, `Exchange.cajeta`; request/response shapes in `HttpRequest.cajeta` /
`HttpResponse.cajeta`; keep-alive policy in `KeepAlive.cajeta`.
