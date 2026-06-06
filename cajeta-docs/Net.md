# Net.md

The cajeta standard library ships a networking subsystem rooted at
**`cajeta.net`**: sockets, name resolution, an async I/O reactor
wired into the fiber scheduler, TLS, URI parsing, an HTTP/1.1
client + server, and a WebSocket library. One import root, one
async model (fibers, not callbacks), consistent error semantics
across the stack.

This document is the **spec** (API surface + wire semantics); the
phased build order lives in
[`plan/cajeta-net-plan.md`](../plan/cajeta-net-plan.md). Where the
two disagree, the plan's checkbox state is the source of truth for
*what is built*; this doc is the source of truth for *what the
shape should be*.

## Table of contents

1. [Design principles](#design-principles)
2. [Package layout](#package-layout)
3. [The async model](#the-async-model)
4. [Sockets](#sockets)
5. [Addresses](#addresses)
6. [Name resolution](#name-resolution)
7. [The reactor](#the-reactor)
8. [Server accept models](#server-accept-models)
9. [TLS](#tls)
10. [URI](#uri)
11. [HTTP/1.1 message model](#http11-message-model)
12. [HTTP client](#http-client)
13. [HTTP server](#http-server)
14. [WebSocket](#websocket)
15. [Supporting primitives](#supporting-primitives)
16. [Error model](#error-model)
17. [Native intrinsics](#native-intrinsics)
18. [Comparative analysis](#comparative-analysis)

---

## Design principles

- **Fibers, not callbacks.** Every I/O operation has a blocking-
  looking async form (`readAsync`, `acceptAsync`, …) that returns
  a `Task`-shaped awaitable. Calling `await` on it **parks the
  fiber**, not the OS thread — the carrier runs other fibers while
  the I/O is in flight. There is no callback hell, no explicit
  state machine; the linear code reads top-to-bottom. This reuses
  the *exact* park/wake machinery `await`, `Channel`, and the
  timer wheel already use.
- **One model across OSes.** Linux epoll, macOS/BSD kqueue, and
  Windows IOCP are hidden behind one native reactor engine. The
  Cajeta surface is identical on every platform; the
  readiness-vs-completion divergence is contained in the C runtime.
- **`cajeta.net` is a peer of `cajeta.io`, not a child.** File I/O
  and network I/O share the fiber model but nothing else — the
  network layer has its own error hierarchy, its own event engine,
  and a far larger surface (HTTP/WS/TLS/URI). Matches Rust
  (`std::net` vs `std::io`) and Go (`net` vs `io`).
- **Composes with the concurrency primitives.** Timeouts are
  `Tasks.withTimeout(d, op)`; cancellation is the existing fiber
  `cancel_with` path; fan-out is `spawn` + `Channel`;
  multiplexing is `Tasks.selectReceive`. Networking adds no new
  concurrency vocabulary.
- **Streaming by default.** Bodies (HTTP requests/responses, WS
  messages, downloads) stream through `AsyncReader`/`AsyncWriter`
  with bounded buffers; nothing forces a full payload into memory.

---

## Package layout

| Package | Contents |
|---|---|
| `cajeta.net` | `TcpStream`, `TcpListener`, `UdpSocket`, `IpAddress`, `SocketAddress`, socket options, `NetException` + subtypes |
| `cajeta.net.dns` | `Dns`, `AddressFamily`, DNS cache |
| `cajeta.net.reactor` | internal — the async engine; no public surface beyond the park/wake hooks the socket types call |
| `cajeta.net.tls` | `TlsClient`, `TlsListener`, cert validation, SNI/ALPN |
| `cajeta.net.uri` | `Uri`, percent-encoding, query params |
| `cajeta.net.http` | `HttpRequest`/`HttpResponse`, `Headers`, parser/serializer, `HttpClient`, `HttpServer`, router |
| `cajeta.net.ws` | `WebSocket`, frame codec, handshake |

Supporting primitives live in their existing roots (they are
networking *blockers* but not networking *types*):

| Symbol | Home |
|---|---|
| `Sha256` | `cajeta.hash` (new) |
| `Sha1` | `cajeta.hash` (new — WS handshake only) |
| `Base64` | `cajeta.codec` (new) |
| `Cache<K,V>` | `cajeta.collection` (exists — reused for the DNS + connection caches) |

`cajeta.net` is a **new built-in stdlib root** alongside codec,
collection, error, hash, io, lang, threading, time, wire, xpu —
it compiles into the toolchain and is DCE-linked like the others.

---

## The async model

Every socket type offers two surfaces:

- **Blocking** (`connect`, `read`, `write`, `accept`, …) — parks
  the *thread* in the syscall. Useful for simple scripts + the
  initial Phase-1 bring-up; not for servers.
- **Async** (`connectAsync`, `readAsync`, `acceptAsync`, …) —
  returns a `Task<…>`-shaped awaitable. `await` parks the
  **fiber**; the carrier runs other fibers; the reactor wakes the
  fiber on readiness/completion.

```cajeta
// Async TCP echo handler — reads block the fiber, never the carrier.
async void handle(TcpStream conn) {
    byte[] buf = new byte[4096];
    while (true) {
        int32 n = await conn.readAsync(buf);
        if (n == 0) { break; }            // peer closed
        await conn.writeAllAsync(buf, n);
    }
}
```

Timeouts and cancellation compose through the existing `Tasks`
API — no networking-specific timeout machinery:

```cajeta
Optional<int32> r = Tasks.withTimeout(Duration.ofSeconds(5),
                                      spawn conn.readAsync(buf));
if (r.isEmpty()) { /* read timed out; the reactor op was deregistered */ }
```

---

## Sockets

```cajeta
// TCP client
TcpStream s = await TcpStream.connectAsync("example.test", 443);
s.setNoDelay(true);
await s.writeAllAsync(bytes);
int32 n = await s.readAsync(buf);
s.close();

// TCP server
TcpListener l = TcpListener.bind(SocketAddress.parse("0.0.0.0:8080"));
l.setReuseAddress(true);
TcpStream conn = await l.acceptAsync();

// UDP
UdpSocket u = UdpSocket.bind(SocketAddress.parse("0.0.0.0:0"));
await u.sendToAsync(datagram, SocketAddress.parse("239.0.0.1:5000"));
RecvResult rr = await u.recvFromAsync(buf);   // rr.bytes, rr.from
```

**Socket options** are typed methods, not raw `setsockopt` ints:
`setNoDelay`, `setKeepAlive`, `setReuseAddress`, `setReusePort`,
`setRecvBufferSize`, `setSendBufferSize`, `setLinger`,
`setBroadcast`, `setTtl`, `setOnlyV6`. Each has a getter.

**Non-blocking mode** — a socket can be put in non-blocking mode;
operations that would block return a `WouldBlock` result (a
distinct value, **not** an exception) so the reactor can drive
readiness loops. Application code normally uses the `*Async` forms
and never sees `WouldBlock`.

The `fd` field is pinned at the same field index `File` uses, so
the intrinsic codegen addresses the descriptor identically across
file + socket types.

---

## Addresses

```cajeta
IpAddress v4 = IpAddress.parse("127.0.0.1");
IpAddress v6 = IpAddress.parse("::1");
SocketAddress a = SocketAddress.parse("[::1]:8080");   // bracket form for v6
a.ip(); a.port(); a.family();                          // V4 | V6
a.toString();                                          // round-trips
```

`SocketAddress.parse` accepts `host:port` (v4) and `[v6]:port`
(bracketed v6); `toString` reproduces the canonical form
byte-identically.

---

## Name resolution

```cajeta
SocketAddress[] addrs = Dns.resolve("example.test", 443);
Task<SocketAddress[]> t = Dns.resolveAsync("example.test");   // parks the fiber
```

`getaddrinfo` is synchronous in libc, so `resolveAsync` runs it on
a **carrier-pool worker** and parks the calling fiber on the
resulting `Task` — no carrier is blocked. Results are cached in a
bounded LRU keyed on `(host, family)` with a configurable default
TTL (record TTL isn't exposed by `getaddrinfo`) and negative-
result caching.

`TcpStream.connectAsync(host, port)` resolves then tries the
returned addresses **sequentially** until one connects (v1).
Happy-eyeballs parallel v4/v6 racing (RFC 8305) is deferred.

---

## The reactor

One reactor thread per process (v1) owns the OS event handle:

| OS | Engine | Model |
|---|---|---|
| Linux | epoll (edge-triggered, `EPOLLONESHOT`) | readiness |
| macOS / BSD | kqueue (`EV_ONESHOT`) | readiness |
| Windows | IOCP (`WSARecv`/`WSASend`/`AcceptEx`/`ConnectEx` + `GetQueuedCompletionStatus`) | completion |

A fiber issuing an awaitable op (1) registers `(fd, interest,
fiber-handle)` with the reactor, (2) parks via the existing
`__cajeta_fiber` park path. The reactor blocks in
`epoll_wait`/`kevent`/`GetQueuedCompletionStatus`; on an event it
moves the registered fiber onto the carrier's ready deque (the
same wake path `Channel` + the timer wheel use). The carrier
resumes the fiber, which retries/finishes the syscall.

The IOCP proactor presents "operation complete" to the Cajeta
layer as "you may now read the buffer," so the socket types code
against one **readiness-style** API even on Windows. io_uring is a
future Linux backend swap behind the same engine interface (no
Cajeta-surface change).

**No carrier ever blocks on socket I/O** — the invariant the whole
design protects. Cancellation deregisters the in-flight op from
the reactor before raising the cancellation sentinel.

---

## Server accept models

Both models ship; pick per workload via `Server.builder()`. The
model is selected with a `ServerModel` value —
`ServerModel.fiberPerConnection()` (Model A, the default) or
`ServerModel.sharedPool(n)` (Model B with `n` workers) — passed to
the builder's `.model(...)` step. `build()` then materializes the
matching concrete server (a plain `Server` for Model A, a
`SharedPoolServer` for Model B); both are a `Server`, so the rest
of the program drives one `serve()` / `shutdown()` surface and the
model's accept-loop differences dispatch polymorphically.

### Model A — fiber-per-connection (default)

The accept loop `spawn`s one fiber per accepted socket. Simplest,
lowest latency, structured-concurrency friendly (a scope owns all
connection fibers; shutdown awaits them). Cost: ~64KB stack per
connection.

```cajeta
Server.builder()
    .bind("0.0.0.0:8080")
    .model(ServerModel.fiberPerConnection())   // the default — may be omitted
    .handler((conn) -> handle(conn))
    .serve();
```

**Use when:** connection counts are moderate, latency matters,
each connection is mostly active.

### Model B — event-driven shared-pool

A bounded pool of N worker fibers drains a readiness/completion
queue. The accept loop registers accepted sockets with the
reactor and pushes "this connection is ready" events onto a
bounded `Channel`; pool workers pull events, run a handler turn,
re-arm interest. IOCP-native on Windows (the completion queue *is*
the work queue).

```cajeta
Server.builder()
    .bind("0.0.0.0:8080")
    .model(ServerModel.sharedPool(8))
    .handler((conn) -> handle(conn))
    .serve();
```

**Use when:** C10K-style fan-in with many mostly-idle connections
— bounded memory (≈ pool size live fibers, not one per
connection) at a slightly higher per-turn overhead.

### Choosing a model — the tradeoff

| Dimension | Model A — fiber-per-conn | Model B — shared-pool(N) |
|---|---|---|
| Memory under load | ~64 KB stack **per live connection** | ~N × stack — **bounded**, independent of connection count |
| Per-request overhead | lowest — handler runs on its own fiber, no queue hop | slightly higher — one bounded-`Channel` hop per connection |
| Latency | lowest (no queueing delay) | a connection may wait for a free worker when all N are busy |
| Peak concurrent handlers | = live connection count (unbounded) | ≈ N (the pool size) — the hard bound |
| Backpressure shape | accept admits until a higher-level cap sheds | full work queue parks the accept loop → connections wait in the kernel `listen` backlog |
| Best fit | moderate connection counts, mostly-active conns, latency-sensitive | C10K-style fan-in, many mostly-idle conns, bounded-memory requirement |
| Complexity | simplest — the default | one extra moving part (the worker pool + bounded queue) |

Rule of thumb: **start with Model A** (the default). Switch to
`sharedPool(n)` only when connection counts grow large enough that
one stack per connection is the memory bottleneck, or when many
connections are idle most of the time — size `n` to the number of
CPU-bound handler turns you want in flight at once (often ≈ core
count for CPU-bound handlers, higher for handlers that park on I/O).

### Backpressure

Common to both: a max-concurrent-connections cap (semaphore-gated
accept), per-connection buffer caps, configurable `listen`
backlog, and a load-shed policy (refuse/close beyond the cap). A
slow client blocks its own writer fiber without stalling others.

---

## TLS

**v1 bundles BoringSSL** (statically linked), not platform-native
stacks — one code path on all OSes, modern TLS 1.3 + ALPN, and a
memory-BIO interface that composes with the async reactor (no fd
ownership: the Cajeta layer pumps ciphertext between the socket
and the TLS engine). An internal `TlsBackend` interface keeps an
mbedTLS swap mechanical.

```cajeta
TlsClient tls = await TlsClient.connectAsync("example.test", 443,
    TlsConfig.builder().alpn(["http/1.1"]).build());
// SNI is set from the host automatically; cert chain validated
await tls.writeAsync(plaintext);
int32 n = await tls.readAsync(buf);
String proto = tls.negotiatedProtocol();   // "http/1.1"
```

- **Cert validation:** hostname match (SAN + CN fallback, wildcard
  rules), chain verification against the OS trust store (with a
  bundled CA fallback), expiry/not-before checks. Uses **SHA-256**
  (`cajeta.hash`) for fingerprinting.
- **SNI:** sent from the connect host.
- **ALPN:** client offers a protocol list, reports the negotiated
  protocol (`http/1.1` for HTTPS; the surface is ready for `h2`).
- **Server-side** (`TlsListener`): load cert + key (PEM),
  terminate TLS on accepted connections, ALPN selection callback.
  Ships after the client.

The handshake parks on the reactor — a slow handshake never blocks
a carrier.

---

## URI

RFC 3986 parse + build:

```cajeta
Uri u = Uri.parse("https://u:p@h.test:8443/a/b?x=1&y=2#frag");
u.scheme();   // "https"
u.host();     // "h.test"
u.port();     // 8443  (or the scheme default)
u.path();     // "/a/b"
u.query();    // QueryParams multi-map, order-preserving
u.fragment(); // "frag"

String enc = Uri.percentEncode(raw, Component.Query);   // component-aware sets
Uri abs = Uri.resolve(base, "../other");                // RFC 3986 §5 reference resolution
```

Percent-encoding uses the correct reserved/unreserved set per
component (path vs query vs fragment differ), UTF-8 aware. Query
params parse into an ordered multi-map (duplicate keys preserved).
Reference resolution (`resolve`) follows RFC 3986 §5.4 — needed
for relative HTTP redirect `Location` headers. IPv6-literal
authorities (`http://[::1]:80/`) parse.

---

## HTTP/1.1 message model

Pure codec over byte buffers — no I/O, tests without sockets.

```cajeta
HttpRequest req = HttpRequest.builder()
    .method("GET").uri(u)
    .header("Accept", "application/json")
    .build();

Headers h = resp.headers();
h.get("content-type");      // case-insensitive
h.getAll("set-cookie");     // multi-value, order-preserving
```

- **`Headers`** — case-insensitive (lowercased keys), multi-value,
  insertion-order-preserving; correct comma-folded-vs-repeated
  handling and the `Set-Cookie` special case.
- **Incremental parser** — a resumable state machine fed arbitrary
  byte chunks (a header split across reads parses identically to a
  one-shot feed), with hard limits (max header size/count/line
  length) to resist abuse.
- **Body framing** — `Content-Length` reader, **chunked
  transfer-decoding** (size lines, ignored chunk-extensions,
  trailers), and `Connection: close`-delimited bodies. The
  `BodyReader` streams.
- **Serializer** — writes a head + `Content-Length` or **chunked**
  body to an `AsyncWriter`, with correct `Host`/`Connection`/
  `Date` defaults.
- **Keep-alive** — HTTP/1.1 default is keep-alive unless
  `Connection: close`; the reuse-vs-close decision matches the
  1.0/1.1 defaults.

---

## HTTP client

The HTTPS-capable HTTP/1.1 client — **cvm's dependency**.

```cajeta
HttpClient c = HttpClient.builder()
    .connectTimeout(Duration.ofSeconds(10))
    .maxRedirects(5)
    .build();

// JSON convenience
ReleaseManifest m = await c.getJson<ReleaseManifest>("https://api.github.test/releases/latest");

// Streaming download with running checksum — cvm's exact need
Sha256 sum = await c.downloadTo("https://.../cajeta-1.0.tar.zst", path);
if (sum.hex() != expected) { /* checksum mismatch — abort install */ }
```

- **Connection pool + keep-alive** keyed on `(scheme, host,
  port)`; idle reaping; max-conns-per-host.
- **Redirects** — 301/302/303/307/308 with a hop cap, correct
  method/body rewrite (303 → GET; 307/308 preserve), relative-
  `Location` resolution, and `Authorization` stripping on
  cross-origin hops.
- **Timeouts/cancellation** — per-request connect/read/total
  deadlines via `Tasks.withTimeout`/`withDeadline`; cancellation
  deregisters the in-flight reactor op.
- **Streaming bodies** — upload from an `AsyncReader`, download as
  a streaming `BodyReader`; `downloadTo(path)` streams to a
  `cajeta.io.file.File` with an optional running SHA-256.
- **Transparent decompression** — `gzip`/`deflate` response
  bodies, advertised via `Accept-Encoding`, decoded streaming.

---

## HTTP server

```cajeta
HttpServer.builder()
    .bind("0.0.0.0:8080")
    .model(SharedPool(16))                    // or FiberPerConnection
    .route("GET", "/users/{id}", (req) -> {
        String id = req.pathParam("id");
        return HttpResponse.json(200, lookup(id));
    })
    .serve();
```

- Runs on **both** accept models (NET-4a/4b), selected via the
  builder.
- **Minimal router** — method + path-pattern (`/users/{id}` path
  params), 404/405 defaults. Deliberately not a web framework.
- **Streaming** — handlers write chunked bodies incrementally and
  read streaming request bodies without buffering.
- **HTTPS** — TLS termination (`TlsListener`) in front; ALPN
  `http/1.1`.
- **Hardening** — request timeout, max body size, slowloris
  mitigation (header-read deadline), `100-continue`.

---

## WebSocket

RFC 6455 — client + server.

```cajeta
WebSocket ws = await WebSocket.connect("wss://echo.test/socket");
await ws.send("hello");                       // text
Message m = await ws.receive();               // m.isText() / m.isBinary()
await ws.send(payloadBytes);                  // binary
await ws.close(1000, "bye");
```

- **Handshake** — client sends `Upgrade: websocket` + a random
  `Sec-WebSocket-Key`; validates the server's
  `Sec-WebSocket-Accept` = `base64(SHA-1(key + GUID))`. Server
  side computes + returns it. **Needs SHA-1 + base64.**
- **Frame codec** — FIN/RSV/opcode/MASK/payload-length
  (7/16/64-bit) + masking key; client frames masked, server frames
  unmasked (RFC 6455 §5); incremental decoder (frame split across
  reads).
- **Fragmentation** — continuation frames reassemble into a
  logical message, with a max-message-size limit.
- **Control frames** — ping/pong (auto-pong by default), the
  bidirectional close handshake (close codes + reason), interleaved
  between data fragments.
- **Concurrency** — a reader fiber and a writer fiber may operate
  on one socket concurrently (a write mutex serializes frame
  emission). This is the standard WS usage pattern.
- **WSS** — over TLS (NET-5) via the `wss://` scheme.
- **Conformance** — gated on the Autobahn TestSuite core cases.

`permessage-deflate` compression is deferred (the handshake's
extension-negotiation slot is parsed so it slots in later).

---

## Supporting primitives

These are networking blockers that live in their existing roots:

- **`Sha256`** (`cajeta.hash`) — FIPS 180-4, one-shot +
  incremental (so it runs over a streaming download). The current
  `cajeta.hash` has MD5/SipHash/XXHash3/DefaultHasher but **no
  SHA-256** — this is a new addition. Needed by TLS cert
  validation + cvm's checksum.
- **`Sha1`** (`cajeta.hash`) — for the WebSocket
  `Sec-WebSocket-Accept` only; flagged "not for security use."
- **`Base64`** (`cajeta.codec`) — standard + URL-safe alphabets,
  padding handling. Needed by the WS handshake + HTTP auth.
- **Buffer pool** (`cajeta.net`, internal) — pooled byte buffers
  reused across connections, a ring/rope buffer for the
  incremental parsers, and high/low-watermark backpressure
  signalling.

---

## Error model

`cajeta.net.NetException extends cajeta.error.RecoverableException`
— the root, mirroring `cajeta.io.file.IoException`. Callers either
catch a specific subtype or declare it in their throws clause. A
`cajeta_net_errno` shim maps platform error codes (POSIX `errno` /
`WSAGetLastError`) to a stable cross-platform enum, then to the
exception subtype.

| Subtype | Raised when |
|---|---|
| `ConnectionRefused` | connect to a closed port |
| `ConnectionReset` | peer reset (RST) |
| `ConnectionAborted` | local abort |
| `AddressInUse` | bind to a taken address |
| `AddressNotAvailable` | bind to a non-local address |
| `HostUnreachable` / `NetworkUnreachable` | routing failure |
| `BrokenPipe` | write to a closed peer |
| `TimedOut` | deadline exceeded |
| `WouldBlock` | non-blocking op would block (surfaced as a value, not thrown, in the reactor path) |
| `UnknownHost` / `ResolutionFailed` | DNS failure |
| `TlsHandshakeFailed` / `CertificateInvalid` / `TlsProtocolError` | TLS failures (`CertificateInvalid` carries a reason: expired / hostname-mismatch / untrusted-root / self-signed) |
| `MalformedUri` | URI parse failure |
| `MalformedMessage` / `HeadersTooLarge` / `InvalidChunkEncoding` / `UnexpectedEof` | HTTP parse failures |
| `HandshakeRejected` / `ProtocolViolation` / `MessageTooLarge` / `ConnectionClosed` | WebSocket failures (`ConnectionClosed` carries the close code) |

A companion `cajeta-docs/stdlib/net/Errors.md` chart (errno →
exception) lands with the error hierarchy, matching the
`cajeta.io.file` `Errors.md` precedent.

---

## Native intrinsics

The native layer adds `__cajeta_net_*` to
`runtime/native/cajeta_runtime.c`, surfaced through the existing
`Cajeta.*` intrinsic-bridge convention (the same path `Channel`
reaches `Cajeta.lockNew` and `File` reaches the fd ops). Groups:

- **Sockets:** `__cajeta_net_socket`, `_bind`, `_listen`,
  `_accept`, `_connect`, `_send`, `_recv`, `_sendto`, `_recvfrom`,
  `_close`, `_shutdown`, `_setsockopt`, `_getsockopt`,
  `_set_nonblocking`, plus `__cajeta_net_sockaddr_pack`/`_unpack`.
- **DNS:** `__cajeta_net_getaddrinfo`, `_freeaddrinfo`.
- **Reactor:** `__cajeta_net_reactor_init`, `_register`,
  `_deregister`, `__cajeta_net_await_readable`,
  `__cajeta_net_await_writable` (each parks the current fiber and
  returns on wake), reusing `__cajeta_parked_head` + the carrier
  deque.
- **TLS:** `__cajeta_tls_ctx_new`, `_conn_new`, `_set_sni`,
  `_set_alpn`, `_feed_ciphertext`, `_read_plaintext`,
  `_write_plaintext`, `_pull_ciphertext`, `_handshake_step`,
  `_free` (memory-BIO design — no fd ownership).

Windows uses Winsock (`WSAStartup` once at runtime init, IOCP for
the reactor); the errno shim normalizes `WSAGetLastError` into the
cajeta error space.

---

## Comparative analysis

| Choice | Source | Why |
|---|---|---|
| Fibers + blocking-looking async I/O | Go goroutines, our own fiber scheduler | No callback hell; linear code; reuses the existing park/wake path |
| `cajeta.net` peer of `cajeta.io` | Rust `std::net`, Go `net` | Network I/O is its own subsystem, not "another stream" |
| Bundled BoringSSL over native TLS stacks | rustls/BoringSSL ecosystems | One code path; clean async memory-BIO integration; avoids SChannel's async pain |
| Reactor abstraction over epoll/kqueue/IOCP | libuv, tokio | Hide the readiness-vs-completion split in the native engine |
| Both accept models shipped | nginx (event pool) + Go (goroutine-per-conn) | Different workloads want different shapes; let the user pick |
| Streaming bodies by default | Go `io.Reader` bodies, Rust `hyper` | Bounded memory; large downloads (cvm) without buffering |
| Minimal router, not a framework | Go `net/http.ServeMux` | The stdlib provides the transport; frameworks layer on top |
| Timeouts via `Tasks.withTimeout` | our own concurrency primitives | No networking-specific timeout vocabulary |
