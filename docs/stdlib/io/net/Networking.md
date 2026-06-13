# Networking — `cajeta.net.http` / `cajeta.net.ws`

> **Status — forward design.** What ships **today** is a leaner HTTP/1.1
> client + server and an RFC 6455 WebSocket, **in stdlib** under
> `cajeta.net.http` and `cajeta.net.ws` — documented in
> [`docs/Net.md`](../../../Net.md), verified against
> [`runtime/src/cajeta/net/http/`](../../../../runtime/src/cajeta/net/http/)
> and [`.../ws/`](../../../../runtime/src/cajeta/net/ws/). This document
> is the **forward design** for the richer surface layered on top:
> HTTP/2, the builder-driven client, the `Body` abstraction, composable
> middleware, routing with typed path parameters, and Server-Sent
> Events. Treat the API shapes below as **planned** — the shipped names
> are flatter (e.g. `Router.route(method, pattern, handler)` with
> handlers typed as `(HttpRequest) -> #HttpResponse`, not an abstract
> `Handler` class; `HttpClient()` + `get`/`send`, not `HttpClient.Builder`).

A design for `cajeta.net.http` covering HTTP/1.1 + HTTP/2 + WebSockets,
both client and server, with TLS for HTTPS / WSS. Built on `cajeta.net`
(TCP / TLS sockets) and `cajeta.concurrent` (fiber scheduling). HTTP and
WebSocket are DCE-linked stdlib roots, so programs that don't touch them
don't pay for them.

The HTTP/2, middleware, SSE, and richer-client work lands incrementally
on top of the shipped HTTP/1.1 core — the spec churn around HTTP/2 /
HTTP/3, header-handling edge cases, and performance work iterates faster
than the transport layer beneath it.

## Why HTTP lives in stdlib (with the boundary DCE enforces)

The "small executables" / "most things need networking" tension
resolves cleanly once you separate transport from application protocol.
`cajeta.net` (sockets) is what most networked code needs at the
transport layer; HTTP is one specific application protocol on top of it.
Cajeta keeps both in stdlib but leans on **dead-code elimination** for
the footprint guarantee:

- Programs that don't touch HTTP get no HTTP bytes — LLVM DCE drops the
  unused `cajeta.net.http` / `cajeta.net.ws` code, so the import is
  effectively pay-for-what-you-use without a separate package.
- HTTP still iterates on its own cadence — HTTP/2 features, header-edge
  handling, performance tuning — behind the same client/server surface.
- WebSockets sit next to HTTP (`cajeta.net.ws`) since WS rides the HTTP
  upgrade handshake to start.

One canonical implementation, doc / test / release coordination with the
language, ecosystem cohesion. Users reach it via `import
cajeta.net.http.X` / `import cajeta.net.ws.X`.

## Goals (v1)

- **HTTP/1.1 server + client** — full request / response lifecycle,
  chunked transfer encoding, keep-alive connection reuse,
  pipelining where it matters.
- **HTTP/2 server + client** — multiplexed streams over a single
  connection, header compression (HPACK), server push (opt-in),
  flow control. Negotiated via ALPN where TLS is in use, h2c
  upgrade for cleartext.
- **WebSocket server + client (RFC 6455)** — full frame protocol,
  fragmentation, control frame handling, close handshake. Hooked
  into the HTTP server via the upgrade mechanism so a single
  endpoint can serve both HTTP and WS.
- **TLS for HTTPS + WSS** — server-side cert + key loading,
  client-side cert verification against the system trust store,
  ALPN for protocol negotiation, SNI for multi-tenant hosting.
- **Two connectivity models, one Handler abstraction.**
  Fiber-per-connection (sync handlers, blocking-looking code,
  the easy default) and event-driven (async handlers, single
  reactor multiplexing many connections, the high-density
  choice) are both first-class. A hybrid mode — reactor accepts
  + framing, fiber pool runs handlers — covers the middle. The
  mode is a property of each `HttpServer` instance, not a
  global framework setting: one program can stand up a fiber
  server on `:8080` for its REST API and an event-driven
  server on `:9090` for its WebSocket fan-out, both in the
  same process. See "Connectivity model" under
  `cajeta.net.http.server` for guidance.
- **Streaming request / response bodies** — chunked input and
  output exposed as readers / writers, not "load the whole body
  into a Buffer." Critical for upload/download endpoints,
  server-sent events, large payloads.
- **Routing with typed path parameters.** `/users/{id:int64}`
  binds `id` as `int64` in the handler signature; mismatched
  types fail at compile time when the route is declared at a
  module scope, runtime when registered dynamically.
- **Middleware that composes.** Logging, auth, CORS, rate
  limiting, compression as standalone middleware that wraps a
  handler. Compose order matters and is explicit at registration.

## Non-goals (v1)

- **HTTP/3 / QUIC.** UDP-based QUIC transport is non-trivial; lands
  as a follow-up once `cajeta.net` grows UDP socket support and the
  QUIC state machine work happens. The HTTP layer is designed to
  accommodate it (transport abstraction over HTTP semantics) but v1
  ships HTTP/1.1 + HTTP/2 only.
- **gRPC.** Higher-level RPC sits on HTTP/2 but has its own service
  description (.proto), code generation, and streaming semantics.
  Belongs in a separate library (`cajeta.grpc`) that depends on
  cajeta.net.http for transport.
- **Server-side templating.** `cajeta.net.http` does HTTP, not HTML
  rendering. A separate `cajeta.template` (or third-party) handles
  templates and feeds the output into a response body.
- **Built-in session / cookie management beyond parsing + setting.**
  Cookie attributes are parsed correctly; what to PUT in cookies
  (signed sessions, server-side stores, JWTs) is application logic
  not framework concern.
- **OAuth / OIDC client.** Auth protocols built on HTTP belong in
  their own library. cajeta.net.http exposes the primitives (Bearer
  token headers, redirect handling) those libraries need.
- **Custom TLS implementation.** v1 wraps the system TLS stack
  (OpenSSL on Linux, SChannel on Windows, Network.framework on
  macOS) via cajeta.net. A pure-cajeta TLS implementation is a
  separate, much larger effort.

## Package layout

```
cajeta.net.http               — HTTP types: Method, Status, Headers, Url,
                            Version, MediaType, parsing helpers
cajeta.net.http.body          — Body abstraction: in-memory + streaming
                            input / output; chunked encoding;
                            multipart parsing
cajeta.net.http.client        — HttpClient with connection pooling,
                            request builder, redirect / retry policy,
                            timeout configuration
cajeta.net.http.server        — HttpServer with fiber-per-request
                            handling, request lifecycle, response
                            building, graceful shutdown
cajeta.net.http.routing       — Route patterns, typed path parameter
                            extraction, route trees, dispatch
cajeta.net.http.middleware    — Middleware trait + bundled middleware
                            (logging, request ID, auth, CORS,
                            compression, rate limit, recover)
cajeta.net.http.tls           — TLS configuration, cert / key loading,
                            ALPN, SNI, system trust store integration
cajeta.net.ws     — Frame protocol, message types, control
                            frame handling, close codes
cajeta.net.ws.client — WebSocket client (handshake + frame
                            loop)
cajeta.net.ws.server — WebSocket server-side upgrade
                            integration with cajeta.net.http.server
cajeta.net.http.h1            — HTTP/1.1 protocol implementation
                            (internal — users go through .client /
                            .server)
cajeta.net.http.h2            — HTTP/2 protocol implementation: HPACK,
                            frame framing, stream multiplexing,
                            flow control (internal)
cajeta.net.http.compression   — gzip / deflate / brotli body encoding +
                            decoding
cajeta.net.http.sse           — Server-Sent Events (text/event-stream)
                            server + client
```

Deferred to follow-up libraries:
```
cajeta.net.http.h3            — HTTP/3 over QUIC
cajeta.grpc               — gRPC client + server
```

---

## cajeta.net.http — core types

```cajeta
public enum Method {
    GET, HEAD, POST, PUT, DELETE, PATCH, OPTIONS, CONNECT, TRACE;

    public boolean isSafe();          // GET, HEAD, OPTIONS, TRACE
    public boolean isIdempotent();    // safe methods + PUT, DELETE
    public boolean allowsBody();
}

public enum Version {
    HTTP_1_0, HTTP_1_1, HTTP_2, HTTP_3;
    public String wireString();
    public boolean supportsKeepAlive();
}

public final class Status {
    public int16  code;             // 100-599
    public String reason;           // canonical reason phrase

    public static Status OK;                          // 200
    public static Status CREATED;                     // 201
    public static Status NO_CONTENT;                  // 204
    public static Status MOVED_PERMANENTLY;           // 301
    public static Status FOUND;                       // 302
    public static Status NOT_MODIFIED;                // 304
    public static Status BAD_REQUEST;                 // 400
    public static Status UNAUTHORIZED;                // 401
    public static Status FORBIDDEN;                   // 403
    public static Status NOT_FOUND;                   // 404
    public static Status METHOD_NOT_ALLOWED;          // 405
    public static Status CONFLICT;                    // 409
    public static Status GONE;                        // 410
    public static Status PAYLOAD_TOO_LARGE;           // 413
    public static Status TOO_MANY_REQUESTS;           // 429
    public static Status INTERNAL_SERVER_ERROR;       // 500
    public static Status BAD_GATEWAY;                 // 502
    public static Status SERVICE_UNAVAILABLE;         // 503
    public static Status GATEWAY_TIMEOUT;             // 504
    // ... full RFC 9110 status registry

    public static Status of(int16 code);
    public static Status of(int16 code, String reason);

    public boolean isInformational();   // 1xx
    public boolean isSuccess();         // 2xx
    public boolean isRedirection();     // 3xx
    public boolean isClientError();     // 4xx
    public boolean isServerError();     // 5xx
}

public final class Headers {
    // Case-insensitive multi-map (HTTP header names are
    // case-insensitive per RFC; values can repeat).
    public Headers();

    public Headers put(String name, String value);          // sets, overwriting
    public Headers add(String name, String value);          // appends
    public String  get(String name);                        // first value, or null
    public String[] getAll(String name);                    // all values
    public boolean contains(String name);
    public Headers remove(String name);
    public Iterable<Pair<String, String>> entries();

    // Common typed accessors with parsing.
    public int64       contentLength();             // -1 if absent / chunked
    public MediaType   contentType();
    public String      authorization();
    public String      userAgent();
    public Cookie[]    cookies();                   // parses Cookie header
    public Date        date();                      // RFC 7231 IMF-fixdate
    public CacheControl cacheControl();             // parses Cache-Control
}

public final class Url {
    public String scheme;            // "http", "https", "ws", "wss"
    public String host;
    public int16  port;
    public String path;
    public Map<String, String[]> query;
    public String fragment;          // not sent on the wire; client-side only

    public static Url parse(String text);
    public String     toString();
    public Url        resolve(String relative);     // RFC 3986 reference resolution
    public Url        withPath(String newPath);
    public Url        withQuery(String key, String value);
}

public final class MediaType {
    public String type;              // "text"
    public String subtype;           // "html"
    public Map<String, String> parameters;   // {"charset": "utf-8"}

    public static MediaType parse(String text);
    public String           toString();

    public static MediaType TEXT_PLAIN;
    public static MediaType TEXT_HTML;
    public static MediaType APPLICATION_JSON;
    public static MediaType APPLICATION_OCTET_STREAM;
    public static MediaType APPLICATION_FORM_URLENCODED;
    public static MediaType MULTIPART_FORM_DATA;
    // ...
}
```

---

## cajeta.net.http.body — streaming bodies

```cajeta
public abstract class Body {
    public abstract int64 contentLength();          // -1 for unknown / chunked
    public abstract MediaType contentType();
    public abstract InputStream stream();
}

public final class BytesBody extends Body {
    public BytesBody(int8[] data, MediaType type = MediaType.APPLICATION_OCTET_STREAM);
}

public final class StringBody extends Body {
    public StringBody(String text, MediaType type = MediaType.TEXT_PLAIN);
}

public final class StreamBody extends Body {
    // Wraps an InputStream — used for upload / download where
    // the body shouldn't materialize in memory.
    public StreamBody(InputStream src, MediaType type, int64 contentLength = -1);
}

public final class FormBody extends Body {
    // application/x-www-form-urlencoded.
    public FormBody();
    public FormBody add(String name, String value);
}

public final class MultipartBody extends Body {
    // multipart/form-data. Fields can be plain values or file
    // uploads (a Body in turn).
    public MultipartBody();
    public MultipartBody addField(String name, String value);
    public MultipartBody addFile(String name, String fileName, Body content);
}

// Reading multipart from incoming requests.
public final class MultipartParser {
    public MultipartParser(InputStream src, String boundary);
    public Iterator<MultipartPart> parts();
}
```

`Body` is an abstraction over both in-memory and streaming
shapes. Handlers reading large uploads pull from `body.stream()`
without materializing the whole payload; small JSON bodies use
`BytesBody.of(...).asString()` and the convenience accessors
short-circuit.

---

## cajeta.net.http.client

```cajeta
public final class HttpClient {
    // Construction via builder for the many configuration knobs.
    public static HttpClient.Builder builder();

    // Issue a request synchronously (returns when the response
    // headers + body are both available, body materialized to
    // bytes by default).
    public Response send(Request req);

    // Issue a request asynchronously — returns a Task<Response>
    // hooked into the cajeta.concurrent fiber scheduler.
    public Task<Response> sendAsync(Request req);

    // Streaming send: response body stays as an InputStream,
    // caller drives the read.
    public StreamingResponse sendStreaming(Request req);

    // Connection pool management.
    public void shutdown(Duration drainTimeout = Duration.ofSeconds(10));
}

public final class HttpClient.Builder {
    public Builder version(Version v);                    // HTTP_1_1 default
    public Builder followRedirects(RedirectPolicy p);     // NORMAL default
    public Builder connectTimeout(Duration d);            // 10s default
    public Builder requestTimeout(Duration d);            // unbounded default
    public Builder maxConnectionsPerOrigin(int8 n);       // 8 default
    public Builder maxIdleConnections(int16 n);           // 32 default
    public Builder idleTimeout(Duration d);               // 90s default
    public Builder retryPolicy(RetryPolicy p);
    public Builder cookieJar(CookieJar jar);
    public Builder proxy(ProxyConfig proxy);
    public Builder tls(TlsConfig tls);
    public Builder userAgent(String ua);
    public Builder defaultHeaders(Headers h);
    public HttpClient build();
}

public final class Request {
    public Method  method;
    public Url     url;
    public Version version;
    public Headers headers;
    public Body    body;             // null for GET / HEAD / DELETE without body
    public Duration timeout;         // overrides client-level timeout

    public static Request.Builder builder();
    public static Request get(Url url);
    public static Request post(Url url, Body body);
    public static Request put(Url url, Body body);
    public static Request delete(Url url);
    public static Request patch(Url url, Body body);
}

public final class Response {
    public Status   status;
    public Version  version;
    public Headers  headers;
    public int8[]   body;            // materialized; for streaming see StreamingResponse

    public String   bodyAsString(Encoding enc = Encoding.UTF_8);
    public boolean  successful();    // status.isSuccess()
    public String   header(String name);
}

public final class StreamingResponse {
    public Status      status;
    public Version     version;
    public Headers     headers;
    public InputStream bodyStream;

    // The connection returns to the pool when bodyStream is
    // closed — explicitly close to free the connection slot.
    public void close();
}

public enum RedirectPolicy {
    NEVER,           // never follow; return the 3xx response
    SAME_ORIGIN,     // follow only if scheme + host + port match
    NORMAL,          // follow up to maxRedirects, but switch
                     // POST -> GET on 301 / 302 (browser shape)
    ALWAYS,          // follow everything, preserving method
}

public final class RetryPolicy {
    public int8       maxAttempts;            // 1 = no retry
    public Duration   initialBackoff;
    public float64    backoffMultiplier;      // exponential
    public Duration   maxBackoff;
    public Predicate<Response> retryOn;       // 503 / 429 by default
    public Predicate<Throwable> retryOnError; // connect / read timeouts by default
}
```

Connection pooling keeps idle connections per-origin (scheme +
host + port). HTTP/2 uses a single multiplexed connection per
origin by default (one connection serves many concurrent
requests); HTTP/1.1 uses up to `maxConnectionsPerOrigin`
parallel sockets.

---

## cajeta.net.http.server

### Connectivity model

Two execution shapes ship side-by-side, picked per-server:

**Fiber-per-connection** (default). Every accepted connection
gets its own fiber; the handler runs to completion in that
fiber, reading the request body and writing the response with
ordinary blocking-looking calls. Under the hood cajeta.concurrent's
scheduler multiplexes thousands of fibers across a small thread
pool via non-blocking I/O — the runtime turns "blocking" calls
into reactor-driven yield-and-resume, so user code stays simple
without giving up high-concurrency I/O.

This is the right choice for the vast majority of services.
Sequential, easy to read, debuggers see one stack per request,
errors propagate as exceptions. The ceiling is the fiber
itself: each fiber carries a stack (typically 8KB-64KB
configurable), context-switch costs are real, and at extreme
connection counts (millions of concurrent, mostly-idle
connections — long-poll farms, IoT fan-out, financial market
data feeds) the per-fiber overhead becomes the dominant cost.

**Event-driven** (opt-in). A small reactor thread pool
multiplexes all connections; handlers return `Task<Response>`
and never block. Per-connection memory is just the
connection's protocol state (no fiber stack), so a single
process can hold millions of idle connections within a few
GB of RAM. The tradeoff is the programming model: handlers
that need to compose multiple async operations write
continuation chains or `await` sequences, exception paths
travel through Task results, and debugger stacks reflect the
reactor loop rather than the logical request.

**Hybrid** (opt-in). The reactor accepts connections and
runs HTTP framing; once a request is fully parsed, the
request gets dispatched to a fiber pool where the handler
runs. Good for "many connections, real handler work per
request" — keeps the fiber count proportional to active
requests rather than open connections, while letting handler
code stay synchronous.

```cajeta
public enum ServerMode {
    FIBER_PER_CONNECTION,        // default; sync handlers, fiber per conn
    EVENT_DRIVEN,                // reactor pool; async handlers only
    HYBRID,                      // reactor framing + fiber-pool handlers
}
```

**Mode is per-server, not per-program.** Each `HttpServer`
instance picks its own mode at build time; multiple servers
with different modes coexist in the same process without
running multiple binaries or coordinating thread pools
externally. The reactor runtime is shared (one set of OS
threads doing non-blocking I/O behind the scenes); each
server owns only its own listener socket, fiber-pool / reactor-
worker counts, and handler chain.

The typical mixed-mode shape — a REST API on one port,
WebSocket fan-out on another — looks like:

```cajeta
public static int32 main() {
    // REST API: fiber-per-connection. Handlers do real work
    // (DB lookups, template renders), connection count is
    // moderate, ergonomics matter.
    var api = HttpServer.builder()
        .mode(ServerMode.FIBER_PER_CONNECTION)
        .router(buildApiRouter())
        .tls(serverTls)
        .build();
    api.bind(InetAddress.ANY, 8443);

    // WebSocket fan-out: event-driven. Connection count is
    // high (millions of long-lived clients), per-message work
    // is small, per-connection overhead dominates.
    var wsServer = HttpServer.builder()
        .mode(ServerMode.EVENT_DRIVEN)
        .reactorThreads(4)
        .route(Method.GET, "/feed", WebSocket.asyncHandler(heap FanoutHandler()))
        .tls(serverTls)
        .build();
    wsServer.bind(InetAddress.ANY, 9443);

    api.start();
    wsServer.start();
    cajeta.concurrent.awaitShutdown();   // block main fiber until SIGTERM
    return 0;
}
```

Both servers share the same TLS config, the same routing
machinery, the same Request / Response types. What differs is
strictly the execution model under the listener — and the
choice is local to each `HttpServer`, not lifted to the
process or the library.

### Handler shapes

Two handler types covering the modes. Most code writes the
sync one; the async one is for `EVENT_DRIVEN` and for handlers
that genuinely benefit from async composition.

```cajeta
public abstract class Handler {
    public abstract Response handle(Request request);
}

public abstract class AsyncHandler {
    public abstract Task<Response> handleAsync(Request request);
}
```

Compatibility across modes:

| Mode                  | `Handler` (sync)            | `AsyncHandler` (async)      |
|-----------------------|-----------------------------|-----------------------------|
| FIBER_PER_CONNECTION  | runs natively in the fiber  | awaited inside the fiber    |
| EVENT_DRIVEN          | rejected at registration    | runs natively on reactor    |
| HYBRID                | runs in dispatched fiber    | awaited on reactor          |

The `Handler` -> `AsyncHandler` adaption is a one-liner
(`AsyncHandler.of(handler)` wraps a sync handler into an
async one that returns an already-completed Task), so any sync
handler works in any mode at the cost of carrying its sync
nature. Going the other way — sync wrapper around an async
handler — is rejected: `EVENT_DRIVEN` mode can't host a sync
handler safely, and the type system reflects that.

```cajeta
public final class HttpServer {
    public static HttpServer.Builder builder();

    public void bind(InetAddress address, int16 port);
    public void start();             // returns once listening
    public void shutdown(Duration drainTimeout = Duration.ofSeconds(30));

    public InetAddress[] localAddresses();
    public ServerMetrics metrics();  // active conns, fiber/reactor stats
}

public final class HttpServer.Builder {
    public Builder mode(ServerMode m);              // FIBER_PER_CONNECTION default

    // Route registration accepts either handler shape; valid
    // combinations are checked against `mode` at build().
    public Builder route(Method m, String pattern, Handler h);
    public Builder route(Method m, String pattern, AsyncHandler h);
    public Builder router(Router r);
    public Builder middleware(Middleware m);

    public Builder tls(TlsConfig tls);
    public Builder maxHeaderBytes(int32 n);         // 16KB default
    public Builder maxBodyBytes(int64 n);           // 16MB default; -1 = unbounded
    public Builder readTimeout(Duration d);         // header-receive timeout
    public Builder writeTimeout(Duration d);
    public Builder idleTimeout(Duration d);         // keep-alive idle
    public Builder backlog(int16 n);                // accept backlog (TCP listen)
    public Builder errorHandler(ErrorHandler h);    // panic / exception path

    // Mode-specific knobs. Builder rejects mismatched config
    // (e.g. .reactorThreads on FIBER_PER_CONNECTION) at build()
    // with a clear error.
    public Builder fiberStackBytes(int32 bytes);    // FIBER + HYBRID
                                                    //   8KB default for HTTP-only
                                                    //   handlers; bump for handlers
                                                    //   that do deep work
    public Builder maxFibers(int32 n);              // FIBER + HYBRID
                                                    //   bounded queue past this;
                                                    //   default unbounded (the
                                                    //   ceiling is OS thread count
                                                    //   * scheduler arity)
    public Builder reactorThreads(int8 n);          // EVENT + HYBRID
                                                    //   defaults to cpu count
    public Builder handlerFibers(int32 n);          // HYBRID — handler pool size

    public HttpServer build();
}

public final class Request {       // server-side variant — same shape as client Request
    public Method   method;
    public Url      url;            // path + query; no scheme/host on server side
    public Headers  headers;
    public Body     body;           // streaming by default; .asBytes() to materialize
    public InetAddress remoteAddress;
    public TlsInfo  tls;            // null on plain HTTP

    // Path-parameter extraction set by the router.
    public <T> T pathParam(String name);
    public String queryParam(String name);
    public String[] queryParams(String name);
}
```

Regardless of mode, handlers see the same request-in /
response-out shape, and HTTP/2-specific features (server push,
trailers) are accessible through the same opt-in API surface.
The choice between modes is a deployment-tier decision: pick
fiber for ergonomics + the common case, hybrid when handler
work dwarfs per-connection cost, event-driven when connection
count is the binding constraint.

### When to pick which

| Workload                                                 | Mode         |
|----------------------------------------------------------|--------------|
| Typical REST API, internal service, admin tool           | Fiber        |
| API gateway, request router proxying upstream            | Hybrid       |
| Long-poll / SSE / WebSocket fan-out, IoT bus, chat       | Event-driven |
| Heavy-handler CRUD with moderate concurrency             | Fiber        |
| Low-latency RPC with many short-lived connections        | Hybrid       |
| Trading-tier feed redistribution, gaming presence server | Event-driven |

A rough rule: if simultaneous-connection count is below ~100K
and per-request work is non-trivial, fiber is the right
default. Past 100K-1M concurrent connections — especially when
most are idle most of the time — the fiber per-connection
overhead starts to dominate and event-driven pays for its
ergonomic cost.

### Routing

```cajeta
public final class Router implements Handler {
    public static Router builder();

    public Router GET(String pattern, Handler h);
    public Router POST(String pattern, Handler h);
    public Router PUT(String pattern, Handler h);
    public Router DELETE(String pattern, Handler h);
    public Router PATCH(String pattern, Handler h);
    public Router method(Method m, String pattern, Handler h);
    public Router any(String pattern, Handler h);          // any method

    public Router mount(String prefix, Router sub);        // nested routing
    public Router middleware(Middleware m);                // route-scoped

    public Response handle(Request req);                   // dispatches
}

// Path patterns:
//   /users                       -> exact match
//   /users/{id}                  -> string path parameter
//   /users/{id:int64}            -> typed path parameter
//   /files/{path:*}              -> wildcard (matches segments)
//   /static/{path:**}            -> recursive wildcard (matches everything)
```

Typed parameters (`{id:int64}`) parse-and-bind in the router;
mismatch (`/users/abc` against `{id:int64}`) returns 404 from
the router (no match) rather than reaching the handler. Handlers
read parameters via `req.pathParam("id")` with the same type
the route declared.

### Middleware

```cajeta
public abstract class Middleware {
    public abstract Response wrap(Request req, Handler next);
}

// Bundled middleware (cajeta.net.http.middleware):
//   RequestId            — generates / propagates X-Request-Id header
//   Logging              — structured access log (json-lines)
//   Recover              — catches handler exceptions, returns 500
//   Timeout              — per-request deadline
//   Cors                 — CORS preflight + headers
//   Compression          — gzip / deflate / brotli response encoding
//                          (Accept-Encoding negotiated)
//   Decompression        — request body Content-Encoding handling
//   RateLimit            — token-bucket per IP / per identity
//   BasicAuth            — Authorization: Basic
//   BearerAuth           — Authorization: Bearer (validates via callback)
//   StaticFile           — serves files from a directory
//   ETag                 — auto-set ETag, handle If-None-Match
//   ProxyHeaders         — reads X-Forwarded-* into req
```

Middleware composition order is registration order; each `wrap`
sees the request, calls `next.handle(req)` to delegate, and can
modify the request before / response after. Handlers and
middleware are both `Handler` shape so middleware composition is
just function composition.

---

## cajeta.net.ws

WebSocket frame protocol per RFC 6455, plus the negotiated
`permessage-deflate` extension per RFC 7692.

```cajeta
public enum FrameType {
    CONTINUATION,      // 0x0
    TEXT,              // 0x1
    BINARY,            // 0x2
    CLOSE,             // 0x8
    PING,              // 0x9
    PONG,              // 0xA
}

public final class Frame {
    public FrameType type;
    public boolean   fin;            // last fragment?
    public int8[]    payload;
}

// User-facing message abstraction (frames assembled).
public abstract class Message {
    public abstract MessageType type();
}

public final class TextMessage extends Message {
    public String text;
    public TextMessage(String text);
}

public final class BinaryMessage extends Message {
    public int8[] data;
    public BinaryMessage(int8[] data);
}

public enum CloseCode {
    NORMAL(1000),
    GOING_AWAY(1001),
    PROTOCOL_ERROR(1002),
    UNSUPPORTED_DATA(1003),
    NO_STATUS(1005),                 // reserved — never sent
    ABNORMAL_CLOSE(1006),            // reserved — never sent
    INVALID_PAYLOAD(1007),
    POLICY_VIOLATION(1008),
    MESSAGE_TOO_BIG(1009),
    MISSING_EXTENSION(1010),
    INTERNAL_ERROR(1011),
    TLS_FAILURE(1015);               // reserved — never sent

    public int16 code();
    public static CloseCode of(int16 code);
}
```

### Server-side

```cajeta
public abstract class WebSocketHandler {
    // Called when the upgrade handshake completes and the
    // connection has been promoted to WebSocket. Runs in the
    // connection's fiber.
    public abstract void onConnect(WebSocketConnection conn);

    // Called per inbound message (assembled from one or more
    // frames). Default: no-op; override to handle.
    public void onMessage(WebSocketConnection conn, Message msg) { }

    // Lifecycle hooks.
    public void onPing(WebSocketConnection conn, int8[] payload) {
        conn.sendPong(payload);     // default: echo
    }
    public void onPong(WebSocketConnection conn, int8[] payload) { }
    public void onClose(WebSocketConnection conn, CloseCode code, String reason) { }
    public void onError(WebSocketConnection conn, Throwable t) { }
}

public interface WebSocketConnection {
    public void send(TextMessage msg);
    public void send(BinaryMessage msg);
    public void sendPing(int8[] payload = null);
    public void sendPong(int8[] payload);
    public void close(CloseCode code = CloseCode.NORMAL, String reason = "");

    public boolean isOpen();
    public InetAddress remoteAddress();
    public Map<String, String> handshakeHeaders();
}
```

Wiring into the HTTP server:

```cajeta
Router router = Router.builder()
    .GET("/api/users", heap UserListHandler())
    .GET("/ws/chat", WebSocket.handler(heap ChatHandler()));   // one endpoint, dual mode

HttpServer server = HttpServer.builder()
    .router(router)
    .build();
```

`WebSocket.handler(...)` returns a `Handler` that responds to
non-WebSocket requests with `426 Upgrade Required` and to valid
WebSocket upgrade requests by promoting the connection and
running the supplied `WebSocketHandler`.

### Client-side

```cajeta
public final class WebSocketClient {
    public static WebSocketClient.Builder builder();

    public WebSocketConnection connect(Url url);
    public Task<WebSocketConnection> connectAsync(Url url);

    public void shutdown();
}

public final class WebSocketClient.Builder {
    public Builder tls(TlsConfig tls);
    public Builder connectTimeout(Duration d);
    public Builder maxMessageBytes(int64 n);          // 1MB default
    public Builder pingInterval(Duration d);          // unset default; auto-keepalive
    public Builder permessageDeflate(boolean enable); // true default
    public Builder header(String name, String value); // sent during handshake
    public Builder subprotocols(String... protos);    // negotiated
    public Builder onMessage(Function<Message, Void> handler);
    public Builder onClose(BiFunction<CloseCode, String, Void> handler);
    public WebSocketClient build();
}
```

Both client and server enforce maximum message size limits to
prevent unbounded memory growth on hostile peers; default 1MB,
configurable. `permessage-deflate` extension is offered by
default but only used when both peers negotiate it.

---

## cajeta.net.http.tls

```cajeta
public final class TlsConfig {
    public static TlsConfig.Builder server();        // requires cert + key
    public static TlsConfig.Builder client();        // verifies against trust store
    public static TlsConfig.Builder mutual();        // both directions verify
}

public final class TlsConfig.Builder {
    // Server / mutual: load cert chain + private key.
    public Builder certificate(Path certChainPath, Path privateKeyPath);
    public Builder certificate(int8[] certChainPem, int8[] privateKeyPem);

    // Client: trust-store setup. Defaults to the system trust
    // store; override for testing / pinning.
    public Builder trustStore(Path caBundlePath);
    public Builder trustSystemDefault();
    public Builder trustNothing();           // explicit "no verification" — testing only

    // Protocol + cipher settings.
    public Builder minVersion(TlsVersion v);         // TLS 1.2 default
    public Builder maxVersion(TlsVersion v);
    public Builder cipherSuites(String... suites);   // null = backend default
    public Builder alpn(String... protocols);        // ["h2", "http/1.1"] default

    // Server-only: SNI dispatch (multi-tenant).
    public Builder sni(Function<String, TlsConfig> hostnameToConfig);

    public TlsConfig build();
}

public enum TlsVersion {
    TLS_1_2, TLS_1_3;
}
```

v1 wraps the platform TLS library through `cajeta.net.tls`
(OpenSSL on Linux, SChannel on Windows, Network.framework on
macOS). A pure-cajeta TLS implementation is a separate effort
(`cajeta.tls`) tracked independently.

---

## cajeta.net.http.sse — Server-Sent Events

A minor surface but useful enough to ship in v1, since SSE
solves the same problem WebSockets often get used for (server-
to-client push) without WebSocket's bidirectional complexity.

```cajeta
public final class SseEvent {
    public String id;            // optional; client uses Last-Event-ID for resume
    public String eventName;     // optional; defaults to "message"
    public String data;          // can be multiline
    public Duration retry;       // optional; tells client reconnect interval
}

// Server-side: response body is a stream of events.
public final class SseResponse {
    public static SseResponse stream(Iterable<SseEvent> events);
    public static SseResponse channel(Channel<SseEvent> channel);   // fiber-pushed
}

// Client-side: subscribes to an SSE endpoint, receives events.
public final class SseClient {
    public SseClient(HttpClient http);
    public Iterable<SseEvent> subscribe(Url url);
}
```

---

## Implementation sequence

A reasonable order, given dependencies:

1. **cajeta.net.http core types.** `Method`, `Status`, `Headers`,
   `Url`, `MediaType`, `Version`. Pure data types, no IO. Used by
   every other layer.
2. **cajeta.net.http.body.** `Body`, `BytesBody`, `StringBody`,
   `StreamBody`. Multipart parser. Standalone, no protocol code.
3. **cajeta.net.http.h1.** HTTP/1.1 wire protocol — request / response
   parsing, chunked transfer encoding, framing. Internal to .client
   and .server.
4. **cajeta.net.http.client (HTTP/1.1).** HttpClient with connection
   pooling, request building, redirect / retry policy. The
   single-protocol path lands first; HTTP/2 plugs in behind the
   same surface.
5. **cajeta.net.http.server (HTTP/1.1).** HttpServer + fiber-per-
   request handling. Routing via cajeta.net.http.routing. Lets users
   stand up a real HTTP/1.1 server.
6. **cajeta.net.http.middleware.** The bundled middleware set —
   logging, request ID, recover, timeout, CORS, compression, basic
   / bearer auth, static file. Each middleware is an independent
   commit; ship as you go.
7. **cajeta.net.http.tls.** TLS wrapper around the platform stack.
   HTTPS reachable; client cert verification works against system
   trust store. Wraps cajeta.net.tls primitives (also v1 work
   under cajeta.net).
8. **cajeta.net.http.h2.** HTTP/2 — HPACK, framing, stream
   multiplexing, flow control. Negotiated via ALPN. Plugs in
   behind the .client and .server surfaces from steps (4) and (5);
   user code unchanged. Frame parsing is the canonical
   `view` use case (see `Views.md`): the 9-byte frame header has a
   fixed big-endian layout and decodes zero-copy via `H2FrameHeader(buf)`.
   Frame payloads (DATA, HEADERS, PRIORITY, etc.) get their own view
   types, all sharing the same buffer pool, no per-frame allocation.
9. **cajeta.net.http.compression.** gzip / deflate / brotli encoders
   + decoders. Used by the Compression middleware and by the
   client / server for Content-Encoding handling.
10. **cajeta.net.ws.** Frame protocol (RFC 6455). Just
    the codec + state machine, no transport. Frame headers (the
    2–14 byte prefix with FIN, opcode, mask bit, payload-length
    field) decode via a `view` (see `Views.md`); the variable
    payload-length encoding fits the length-prefixed-field
    pattern documented there.
11. **cajeta.net.ws.server.** Upgrade integration with
    cajeta.net.http.server. WebSocket endpoints reachable.
12. **cajeta.net.ws.client.** WebSocketClient. Standalone,
    independent of server work.
13. **cajeta.net.ws: permessage-deflate (RFC 7692).**
    Negotiated extension. Optional but expected by most modern
    WS deployments.
14. **cajeta.net.http.sse.** Server-Sent Events server + client.
    Small layer over the streaming response support already
    landed in (5).

The gating step is (3) — once HTTP/1.1 wire protocol is correct,
client and server are straightforward applications. (8) lifts the
wire protocol to HTTP/2 without changing the surface above it.

Deferred (separate libraries):
- cajeta.net.http.h3 / QUIC transport — needs UDP socket + QUIC state
  machine in cajeta.net first
- cajeta.grpc — RPC framework on HTTP/2
- cajeta.tls — pure-cajeta TLS (replaces the platform-library
  wrapper in cajeta.net.http.tls)

---

## Open questions

- **TLS implementation choice.** Wrap platform libraries (smaller
  binary, ecosystem-matched cert handling) vs. ship a pure-cajeta
  TLS stack (no platform variance, build-once-run-anywhere). v1
  wraps; the pure-cajeta path is its own multi-quarter effort.
  Worth tracking as a follow-up so eventual transition is
  designed-in, not retrofitted.
- **HTTP/2 server push.** The HTTP/2 spec includes server push
  but most browsers have removed support; Chrome dropped it in
  103. Include the API anyway (it's still useful for non-browser
  clients), or skip it and document the omission? Lean: include
  with a "deprecated upstream" doc note; trivial to support and
  some clients still use it.
- **Streaming body lifecycle.** Server-side handlers reading
  large bodies need a clear contract for "consume the body or
  the connection stalls." Options: implicit drain on handler
  return (safer, hides bugs), explicit drain required (faster
  to debug, easier to break). Lean: implicit drain with an
  opt-out flag for handlers that explicitly own the body
  lifetime (e.g. proxies that pipe to upstream).
- **Request body size enforcement timing.** Reject early
  (413 Payload Too Large at parse time, before handler runs)
  vs. let handlers see oversized bodies if they want? Modern
  servers reject early with a configurable threshold per
  route. Lean: same — global default + per-route override.
- **Routing trie vs regex.** Path matching can be a compiled
  trie (fast, deterministic, less expressive) or regex-based
  (slower, hard to reason about overlap, very expressive).
  Lean: trie with limited wildcard support (`*` for one
  segment, `**` for everything-rest), which covers ~95% of
  real routing needs without regex complexity.
- **Cookie jar in client.** Should `HttpClient` carry an
  in-memory cookie jar by default (browser-shape) or require
  explicit jar configuration (library-shape)? Lean: no jar by
  default; explicit `CookieJar` opt-in. Ergonomics for
  scripting (one-line client) competes against surprise
  ("why is my client sending cookies I didn't set?"); the
  surprise side wins.
- **WebSocket compression default.** `permessage-deflate` adds
  CPU cost and minor latency; usually a net win for text
  payloads. Negotiate-by-default on (clients that don't want
  it can disable explicitly), or off by default? Lean: on,
  matching browser defaults.
