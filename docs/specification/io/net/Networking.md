# Networking — `cajeta.io.net` (transport)

`cajeta.io.net` is the stdlib transport layer. It holds sockets (TCP / UDP /
multicast), name resolution, a cross-platform async I/O reactor wired into the
fiber scheduler, TLS, URI, and the framing + buffer/view substrate that
application protocols build on. There is one import root, one async model
(fibers rather than callbacks), and consistent error semantics.

> **Scope change (2026-06).** HTTP/1.1·2·3, WebSocket, and SSE are an
> application protocol layer, and they have moved out of the stdlib into the
> [cajeta-http](https://github.com/jklappenbach/cajeta-http) library. HTTP is
> transport-independent (HTTP/1.1+2 over TCP, HTTP/3 over QUIC/UDP), so it sits
> above `cajeta.io.net`. This document is the transport spec. It supersedes the
> old top-level `docs/Net.md` (retired) and the prior HTTP-focused version of
> this file. The HTTP/WS content is now in cajeta-http's `docs/http-spec.md`.

## Design principles

- **Fibers, not callbacks.** Every I/O op has a blocking-looking async form
  (`readAsync`, `acceptAsync`, …) that parks the calling fiber rather than the
  OS thread, and returns the result directly once the reactor wakes it. There
  is no callback hell, no explicit state machine and no function coloring, so
  your code reads linearly from top to bottom. It reuses the exact park/wake
  machinery that `spawn`/`await`, `Channel`, and the timer wheel use.
- **One model across OSes.** Linux epoll, macOS/BSD kqueue, and Windows IOCP
  are hidden behind one native reactor engine. The readiness-vs-completion
  divergence is contained in the C runtime, and the Cajeta surface is
  identical everywhere.
- **`cajeta.io.net` is a peer of `cajeta.io`, not a child.** File and network
  I/O share the fiber model and nothing else. Net has its own error hierarchy,
  its own event engine, and a far larger surface. This matches Rust
  (`std::net` vs `std::io`) and Go.
- **Composes with the concurrency primitives.** Timeouts are
  `Tasks.withTimeout`, cancellation is the fiber `cancel_with` path, fan-out
  is `spawn` + `Channel`, and multiplexing is `Tasks.selectReceive`.
  Networking adds no new concurrency vocabulary.
- **Streaming + zero-copy by default.** Data moves through `AsyncReader`/
  `AsyncWriter` over pooled buffers, and structured views decode framing in
  place with no per-message allocation. See [`../Io.md`](../Io.md) for the
  buffer / view / `Stream<T>` substrate.

## Package layout

| Package | Contents |
|---|---|
| `cajeta.io.net` | `TcpStream`, `TcpListener`, `UdpSocket`, `IpAddress`, `SocketAddress`, socket options, framing strategies, `NetException` + subtypes |
| `cajeta.io.net.dns` | `Dns`, `AddressFamily`, DNS cache |
| `cajeta.io.net.reactor` | internal — the cross-platform async engine |
| `cajeta.io.net.tls` | `TlsStream`, `TlsListener`, cert validation, SNI/ALPN (bundled BoringSSL) |
| `cajeta.io.net.uri` | `Uri`, percent-encoding, query params |

`cajeta.io.net` is a built-in stdlib root, alongside codec, collection,
concurrent, error, gpu, hash, io, lang, math, reflect, time and wire, and it
is DCE-linked. Supporting primitives live in their own roots. `Sha256`/`Sha1`
are in `cajeta.hash`, `Base64` is in `cajeta.codec`, and `Cache<K,V>` is in
`cajeta.collection`.

> **Application protocols are libraries, not stdlib.** HTTP/WS/SSE live in
> [cajeta-http](https://github.com/jklappenbach/cajeta-http), and SWIM gossip
> lives in [cajeta-gossip](https://github.com/jklappenbach/cajeta-gossip). The
> stdlib owns the transport primitives, and opinionated protocols ride on top.

## The async model

Every socket type offers a blocking surface and an async surface. The
blocking surface (`connect`/`read`/`write`/`accept`) parks the thread, and it
is meant for scripts and bring-up. The async surface
(`connectAsync`/`readAsync`/`acceptAsync`) parks the fiber, the carrier runs
other fibers, and the reactor wakes the fiber on readiness or completion.

```cajeta
void handle(TcpStream conn) {
    int8[] buf = heap int8[4096];
    while (true) {
        int64 n = conn.readAsync(buf, 0, 4096);   // parks the fiber; returns count
        if (n == 0) { break; }                     // EOF
        conn.writeAllAsync(buf, 0, n);
    }
}
// Deadlines compose through Tasks — no networking-specific timeout machinery:
Optional<int64> r = Tasks.withTimeout(Duration.ofSeconds(5), spawn conn.readAsync(buf, 0, 4096));
```

## Sockets

```cajeta
TcpStream s #= TcpStream.connectAsync(SocketAddress.parse("93.184.216.34:443"));
s.setNoDelay(true); s.writeAllAsync(b, 0, b.count()); int64 n = s.readAsync(buf, 0, buf.count());

TcpListener l #= TcpListener.bind(SocketAddress.parse("0.0.0.0:8080"));
TcpStream conn #= l.acceptAsync();

UdpSocket u #= UdpSocket.bind(SocketAddress.parse("0.0.0.0:0"));
u.sendTo(dg, 0, dg.count(), SocketAddress.parse("239.0.0.1:5000"));
RecvResult rr #= u.recvFrom(buf, 0, buf.count());   // rr.count, rr.from
```

Socket options are typed methods (`setNoDelay`, `setKeepAlive`,
`setReuseAddress`, `setReusePort`, `setRecvBufferSize`, `setSendBufferSize`,
`setLinger`, `setBroadcast`, `setTtl`, `setOnlyV6`), each with a getter.
Non-blocking mode returns a `WouldBlock` value rather than an exception. App
code normally uses the `*Async` forms and never sees it.

### UDP multicast

`UdpSocket` joins and leaves groups and controls the multicast TX path, as a
thin extension of the datagram socket. IPv4 ASM (`224.0.0.0/4`) and IPv6
(`ff00::/8`) use the same methods. Source-specific multicast (SSM) is a v1.x
add-on.

```cajeta
UdpSocket u #= UdpSocket.bind(SocketAddress.parse("0.0.0.0:5000"));
u.joinGroup(IpAddress.parse("239.0.0.1"));
u.setMulticastTtl(1); u.setMulticastLoopback(false);
u.sendTo(msg, 0, msg.count(), SocketAddress.parse("239.0.0.1:5000"));
RecvResult rr #= u.recvFrom(buf, 0, buf.count());
u.leaveGroup(IpAddress.parse("239.0.0.1"));
```

| Method | Effect |
|---|---|
| `joinGroup(group)` / `joinGroup(group, iface)` | `IP_ADD_MEMBERSHIP` / `IPV6_JOIN_GROUP` |
| `leaveGroup(group)` / `leaveGroup(group, iface)` | drop membership |
| `setMulticastTtl(int32)` | TX hop limit |
| `setMulticastLoopback(boolean)` | receive own sends? |
| `setMulticastInterface(iface)` | TX interface |
| `joinSource(group, source)` / `leaveSource(...)` | SSM — v1.x |

Multicast is gated by the `network` capability. Cluster membership (SWIM gossip)
is a separate library ([cajeta-gossip](https://github.com/jklappenbach/cajeta-gossip)).

### Raw TCP services + framing

A TCP connection is a boundary-less byte stream. Turning it into a message
stream needs a framing strategy, which is the layer between transport and an
application codec. UDP datagrams carry their own boundaries, and HTTP and
WebSocket bring their own framing, so a raw TCP protocol picks one.

| Strategy | Boundary |
|---|---|
| `Framing.lengthPrefixed(width, endian)` | an N-byte length header |
| `Framing.delimiter(bytes)` | a delimiter (e.g. `"\r\n"`) |
| `Framing.fixed(size)` | fixed-size records |
| `Framing.raw` | none — hand the handler the raw byte duplex |

A framer reads from a pooled buffer and emits `View<Frame>`, a zero-copy
borrow over the buffer. The borrow checker guarantees the frame view cannot
outlive the buffer, which gives you safe high-throughput binary protocols
without `ByteBuf.release()` footguns. Framing feeds an application codec, a
typed object or another `View<T>`, over the same data plane HTTP/WS frame
parsing uses (`Views.md`).

## Addresses & name resolution

```cajeta
SocketAddress a #= SocketAddress.parse("[::1]:8080");   // bracketed v6; round-trips
SocketAddress[] addrs #= Dns.resolve("example.test", 443);     // synchronous (built)
Task<SocketAddress[]> t = Dns.resolveAsync("example.test");    // planned — runs getaddrinfo
                                                               // on a carrier-pool worker
```

`Dns.resolveAsync` parks the fiber while `getaddrinfo` runs on a pool worker,
so no carrier is blocked. Results cache in a bounded LRU keyed on
`(host, family)` with negative caching. `connectAsync(host, port)` resolves and
then tries addresses sequentially. Happy-eyeballs (RFC 8305) is deferred.

## The reactor

One reactor thread per process (v1) owns the OS event handle:

| OS | Engine | Model |
|---|---|---|
| Linux | epoll (edge-triggered, `EPOLLONESHOT`) | readiness |
| macOS / BSD | kqueue (`EV_ONESHOT`) | readiness |
| Windows | IOCP (`WSARecv`/`AcceptEx`/`ConnectEx` + `GetQueuedCompletionStatus`) | completion |

A fiber issuing an awaitable op registers `(fd, interest, fiber-handle)` and
parks. The reactor blocks in `epoll_wait`/`kevent`/`GetQueuedCompletionStatus`,
and on an event it moves the fiber onto the carrier's ready deque, which is the
same wake path `Channel` and the timer wheel use. The IOCP proactor presents
"operation complete" to the Cajeta layer as "you may now read", so socket types
code against one readiness-style API even on Windows. io_uring is a future
Linux backend swap behind the same engine interface, with no surface change.
No carrier ever blocks on socket I/O. That is the invariant the whole design
protects.

> Note on the completion model: a completion-based backend (IOCP/io_uring)
> borrows the I/O buffer for the duration of the in-flight op. The buffer must
> outlive the operation and must not be touched until completion. The reactor
> expresses this as an ownership constraint on the pooled buffer (see
> [`../Io.md`](../Io.md)). The readiness backends (epoll/kqueue) do not have it.

## Server accept models

Both models ship, and you pick per workload via `Server.builder().model(...)`.
Handlers have the same un-colored shape under either model, which is
`(conn) -> handle(conn)`.

### Model A — fiber-per-connection (default)

The accept loop `spawn`s one fiber per accepted socket. It is the simplest
model, has the lowest latency, and is structured-concurrency friendly. The
cost is about 64KB of stack per connection.

```cajeta
Server.builder().bind("0.0.0.0:8080")
    .model(ServerModel.fiberPerConnection())   // default; may be omitted
    .handler((conn) -> handle(conn)).serve();
```

### Model B — event-driven shared-pool

A bounded pool of N worker fibers drains a readiness/completion queue. The
accept loop registers sockets and pushes "ready" events onto a bounded
`Channel`. Memory is bounded at about N live fibers rather than one per
connection, and the model is IOCP-native on Windows.

```cajeta
Server.builder().bind("0.0.0.0:8080")
    .model(ServerModel.sharedPool(8)).handler((conn) -> handle(conn)).serve();
```

| Dimension | A — fiber-per-conn | B — shared-pool(N) |
|---|---|---|
| Memory under load | ~64KB/live conn | ~N×stack — bounded |
| Per-request overhead | lowest | one Channel hop |
| Peak concurrent handlers | = live conns | ≈ N |
| Backpressure | accept admits until a cap sheds | full queue parks accept → kernel backlog |
| Best fit | moderate conns, latency-sensitive | C10K fan-in, mostly-idle conns |

The rule of thumb is to start with A. Switch to `sharedPool(n)` when one stack
per connection is the memory bottleneck or most connections are idle. The
common backpressure levers are a max-concurrent cap (semaphore-gated accept),
per-connection buffer caps, a configurable `listen` backlog, and a load-shed
policy.

## TLS

This section was amended 2026-10-18 with cajeta-crypto spec 6.3.
`cajeta.io.net.tls` left the stdlib for the `dev.cajeta.crypto` library as
`dev.cajeta.crypto.tls`, with the same types and APIs over the stdlib's
sockets. The design below holds, and the code lives there.

v1 bundles BoringSSL, statically linked. That gives one code path on all OSes,
TLS 1.3 + ALPN, and a memory-BIO interface that composes with the async
reactor. There is no fd ownership, and the Cajeta layer pumps ciphertext. An
internal `TlsBackend` keeps an mbedTLS swap mechanical.

```cajeta
TlsStream tls #= TlsStream.clientSystemTrust(#sock, host.bytes, host.byteLength);
tls.offerAlpn(alpn, alpnLen); tls.handshake();   // parks the fiber; validates chain
tls.write(plaintext, len); int32 n = tls.read(buf, buf.count());
int32 pl = tls.negotiatedAlpn(out, max);
```

Cert validation covers hostname SAN/CN with wildcards, the chain against the
OS trust store with a bundled CA fallback, and expiry. SNI comes from the
connect host, and ALPN is offered. The server side
(`TlsListener` / `TlsStream.server(...)`) takes a PEM cert+key and handles
termination and ALPN select. The handshake parks on the reactor, so a slow
handshake never blocks a carrier.

## URI

The URI module gives RFC 3986 parse and build (`Uri.parse`/`percentEncode`/
`resolve`), component-aware percent-encoding, an order-preserving query
multi-map, IPv6-literal authorities, and §5.4 reference resolution, which
cajeta-http consumes for relative redirect `Location` headers. It is a small,
broadly useful util, and it is kept in the stdlib.

## Error model

`cajeta.io.net.NetException extends cajeta.error.RecoverableException`. A
`cajeta_net_errno` shim maps POSIX `errno` / `WSAGetLastError` to a stable
cross-platform enum. The transport/TLS/DNS/URI subtypes are `ConnectionRefused`,
`ConnectionReset`, `ConnectionAborted`, `AddressInUse`, `AddressNotAvailable`,
`HostUnreachable`/`NetworkUnreachable`, `BrokenPipe`, `TimedOut`, `WouldBlock`
(a value, and not thrown), `UnknownHost`/`ResolutionFailed`,
`TlsHandshakeFailed`/`CertificateInvalid`/`TlsProtocolError`, and
`MalformedUri`. HTTP/WS parse errors live in cajeta-http, extending
`NetException`. A companion `io/net/Errors.md` errno→exception chart lands
with the hierarchy.

## Native intrinsics

The intrinsics are `__cajeta_net_*` in `runtime/native/cajeta_runtime.c`, via
the `Cajeta.*` intrinsic bridge:

- **Sockets:** `_socket`, `_bind`, `_listen`, `_accept`, `_connect`, `_send`,
  `_recv`, `_sendto`, `_recvfrom`, `_close`, `_shutdown`, `_setsockopt`,
  `_getsockopt`, `_set_nonblocking`, `_sockaddr_pack`/`_unpack`.
- **DNS:** `_getaddrinfo`, `_freeaddrinfo`.
- **Reactor:** `_reactor_init`, `_register`, `_deregister`, `_await_readable`,
  `_await_writable` (park + wake), reusing `__cajeta_parked_head` + the carrier deque.
- **TLS:** `__cajeta_tls_*` (memory-BIO: `_feed_ciphertext`/`_read_plaintext`/
  `_write_plaintext`/`_pull_ciphertext`/`_handshake_step`, with no fd ownership).

Windows uses Winsock (`WSAStartup` at init, IOCP reactor), and the errno shim
normalizes `WSAGetLastError`.

## Comparative analysis

| Choice | Source | Why |
|---|---|---|
| Fibers + blocking-looking async I/O | Go goroutines, our fiber scheduler | No callback hell; reuses park/wake |
| `cajeta.io.net` peer of `cajeta.io` | Rust `std::net`, Go `net` | Net is its own subsystem |
| HTTP a library, not stdlib | Rust (hyper crate) | App protocol over transport; multi-target footprint |
| Bundled BoringSSL | rustls/BoringSSL | One code path; clean async memory-BIO |
| Reactor over epoll/kqueue/IOCP | libuv, tokio | Hide readiness-vs-completion in the engine |
| Both accept models | nginx + Go | Different workloads, different shapes |
| Pluggable framing for raw TCP | Netty codecs (but zero-copy + borrow-checked) | Byte streams need message boundaries; views make it allocation-free |
| Timeouts via `Tasks.withTimeout` | our concurrency primitives | No net-specific timeout vocabulary |

## Implementation sequence

The transport order is sockets + reactor → addresses + DNS → TLS →
UDP/multicast → framing strategies → URI. The HTTP layer is now cajeta-http's
plan. The phased build is in
`../../../agents/cajeta/net/cajeta-net-plan.md`.
