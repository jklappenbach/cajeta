# Networking — `cajeta.io.net` (transport)

The stdlib **transport** layer: sockets (TCP / UDP / multicast), name
resolution, a cross-platform async I/O reactor wired into the fiber scheduler,
TLS, URI, and the framing + buffer/view substrate that application protocols
build on. One import root, one async model (fibers, not callbacks), consistent
error semantics.

> **Scope change (2026-06).** HTTP/1.1·2·3, WebSocket, and SSE are an
> *application* protocol layer and have moved **out of stdlib** into the
> **[cajeta-http](https://github.com/jklappenbach/cajeta-http)** library —
> HTTP is transport-independent (HTTP/1.1+2 over TCP, HTTP/3 over QUIC/UDP), so
> it sits above `cajeta.io.net`. This document is the transport spec. It
> **supersedes** the old top-level `docs/Net.md` (retired) and the prior
> HTTP-focused version of this file; the HTTP/WS content is now in cajeta-http's
> `docs/http-spec.md`.

## Design principles

- **Fibers, not callbacks.** Every I/O op has a blocking-looking async form
  (`readAsync`, `acceptAsync`, …) that **parks the calling fiber**, not the OS
  thread, and returns the result directly once the reactor wakes it. No callback
  hell, no explicit state machine, no function coloring — linear top-to-bottom
  code. Reuses the exact park/wake machinery `spawn`/`await`, `Channel`, and the
  timer wheel use.
- **One model across OSes.** Linux epoll, macOS/BSD kqueue, and Windows IOCP are
  hidden behind one native reactor engine; the readiness-vs-completion divergence
  is contained in the C runtime. The Cajeta surface is identical everywhere.
- **`cajeta.io.net` is a peer of `cajeta.io`, not a child.** File and network I/O
  share the fiber model but nothing else — net has its own error hierarchy, event
  engine, and a far larger surface. Matches Rust (`std::net` vs `std::io`) and Go.
- **Composes with the concurrency primitives.** Timeouts are `Tasks.withTimeout`;
  cancellation is the fiber `cancel_with` path; fan-out is `spawn` + `Channel`;
  multiplexing is `Tasks.selectReceive`. Networking adds no new concurrency
  vocabulary.
- **Streaming + zero-copy by default.** Data moves through `AsyncReader`/
  `AsyncWriter` over **pooled buffers**; structured **views** decode framing
  in place (no per-message allocation). See [`../Io.md`](../Io.md) for the
  buffer / view / `Stream<T>` substrate.

## Package layout

| Package | Contents |
|---|---|
| `cajeta.io.net` | `TcpStream`, `TcpListener`, `UdpSocket`, `IpAddress`, `SocketAddress`, socket options, framing strategies, `NetException` + subtypes |
| `cajeta.io.net.dns` | `Dns`, `AddressFamily`, DNS cache |
| `cajeta.io.net.reactor` | internal — the cross-platform async engine |
| `cajeta.io.net.tls` | `TlsStream`, `TlsListener`, cert validation, SNI/ALPN (bundled BoringSSL) |
| `cajeta.io.net.uri` | `Uri`, percent-encoding, query params |

`cajeta.io.net` is a **built-in stdlib root** (alongside codec, collection,
concurrent, error, gpu, hash, io, lang, math, reflect, time, wire), DCE-linked.
Supporting primitives live in their roots: `Sha256`/`Sha1` (`cajeta.hash`),
`Base64` (`cajeta.codec`), `Cache<K,V>` (`cajeta.collection`).

> **Application protocols are libraries, not stdlib.** HTTP/WS/SSE →
> [cajeta-http](https://github.com/jklappenbach/cajeta-http); SWIM gossip →
> [cajeta-gossip](https://github.com/jklappenbach/cajeta-gossip). Stdlib owns the
> transport primitives; opinionated protocols ride on top.

## The async model

Every socket type offers a **blocking** surface (`connect`/`read`/`write`/`accept`
— parks the thread; for scripts + bring-up) and an **async** surface
(`connectAsync`/`readAsync`/`acceptAsync` — parks the fiber; the carrier runs
other fibers; the reactor wakes the fiber on readiness/completion).

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
#TcpStream s = TcpStream.connectAsync(SocketAddress.parse("93.184.216.34:443"));
s.setNoDelay(true); s.writeAllAsync(b, 0, b.count()); int64 n = s.readAsync(buf, 0, buf.count());

#TcpListener l = TcpListener.bind(SocketAddress.parse("0.0.0.0:8080"));
#TcpStream conn = l.acceptAsync();

#UdpSocket u = UdpSocket.bind(SocketAddress.parse("0.0.0.0:0"));
u.sendTo(dg, 0, dg.count(), SocketAddress.parse("239.0.0.1:5000"));
#RecvResult rr = u.recvFrom(buf, 0, buf.count());   // rr.count, rr.from
```

**Socket options** are typed methods (`setNoDelay`, `setKeepAlive`,
`setReuseAddress`, `setReusePort`, `setRecvBufferSize`, `setSendBufferSize`,
`setLinger`, `setBroadcast`, `setTtl`, `setOnlyV6`), each with a getter.
**Non-blocking mode** returns a `WouldBlock` *value* (not an exception); app code
normally uses the `*Async` forms and never sees it.

### UDP multicast

`UdpSocket` joins/leaves groups and controls the multicast TX path — a thin
extension of the datagram socket. IPv4 ASM (`224.0.0.0/4`) and IPv6 (`ff00::/8`)
use the same methods; source-specific multicast (SSM) is a v1.x add-on.

```cajeta
#UdpSocket u = UdpSocket.bind(SocketAddress.parse("0.0.0.0:5000"));
u.joinGroup(IpAddress.parse("239.0.0.1"));
u.setMulticastTtl(1); u.setMulticastLoopback(false);
u.sendTo(msg, 0, msg.count(), SocketAddress.parse("239.0.0.1:5000"));
#RecvResult rr = u.recvFrom(buf, 0, buf.count());
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

A TCP connection is a boundary-less **byte stream**; turning it into a message
stream needs a **framing strategy** — the layer between transport and an
application codec. (UDP datagrams carry their own boundaries; HTTP/WebSocket bring
their own framing; a *raw* TCP protocol picks one.)

| Strategy | Boundary |
|---|---|
| `Framing.lengthPrefixed(width, endian)` | an N-byte length header |
| `Framing.delimiter(bytes)` | a delimiter (e.g. `"\r\n"`) |
| `Framing.fixed(size)` | fixed-size records |
| `Framing.raw` | none — hand the handler the raw byte duplex |

A framer reads from a pooled buffer and emits **`View<Frame>`** — a borrow over
the buffer, zero-copy; the borrow checker guarantees the frame-view cannot outlive
the buffer (safe high-throughput binary protocols without `ByteBuf.release()`
footguns). Framing feeds an application codec (a typed object or another `View<T>`),
the same data plane HTTP/WS frame parsing uses (`Views.md`).

## Addresses & name resolution

```cajeta
#SocketAddress a = SocketAddress.parse("[::1]:8080");   // bracketed v6; round-trips
#SocketAddress[] addrs = Dns.resolve("example.test", 443);     // synchronous (built)
Task<SocketAddress[]> t = Dns.resolveAsync("example.test");    // planned — runs getaddrinfo
                                                               // on a carrier-pool worker
```

`Dns.resolveAsync` parks the fiber while `getaddrinfo` runs on a pool worker (no
carrier blocked); results cache in a bounded LRU keyed on `(host, family)` with
negative caching. `connectAsync(host, port)` resolves then tries addresses
sequentially (happy-eyeballs RFC 8305 deferred).

## The reactor

One reactor thread per process (v1) owns the OS event handle:

| OS | Engine | Model |
|---|---|---|
| Linux | epoll (edge-triggered, `EPOLLONESHOT`) | readiness |
| macOS / BSD | kqueue (`EV_ONESHOT`) | readiness |
| Windows | IOCP (`WSARecv`/`AcceptEx`/`ConnectEx` + `GetQueuedCompletionStatus`) | completion |

A fiber issuing an awaitable op registers `(fd, interest, fiber-handle)` and parks;
the reactor blocks in `epoll_wait`/`kevent`/`GetQueuedCompletionStatus`; on an
event it moves the fiber onto the carrier's ready deque (the same wake path
`Channel` + the timer wheel use). **The IOCP proactor presents "operation complete"
to the Cajeta layer as "you may now read,"** so socket types code against one
readiness-style API even on Windows. io_uring is a future Linux backend swap behind
the same engine interface (no surface change). **No carrier ever blocks on socket
I/O** — the invariant the whole design protects.

> Note on the completion model: a completion-based backend (IOCP/io_uring) borrows
> the I/O buffer *for the duration of the in-flight op* — the buffer must outlive
> the operation and not be touched until completion. The reactor expresses this as
> an ownership constraint on the pooled buffer (see [`../Io.md`](../Io.md)); the
> readiness backends (epoll/kqueue) do not have it.

## Server accept models

Both ship; pick per workload via `Server.builder().model(...)`. **Handlers are the
same un-colored shape under either model** — `(conn) -> handle(conn)`.

### Model A — fiber-per-connection (default)

The accept loop `spawn`s one fiber per accepted socket. Simplest, lowest latency,
structured-concurrency friendly. Cost: ~64KB stack/connection.

```cajeta
Server.builder().bind("0.0.0.0:8080")
    .model(ServerModel.fiberPerConnection())   // default; may be omitted
    .handler((conn) -> handle(conn)).serve();
```

### Model B — event-driven shared-pool

A bounded pool of N worker fibers drains a readiness/completion queue; the accept
loop registers sockets and pushes "ready" events onto a bounded `Channel`. Bounded
memory (≈ N live fibers, not one per connection); IOCP-native on Windows.

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

Rule of thumb: **start with A**; switch to `sharedPool(n)` when one stack per
connection is the memory bottleneck or most connections are idle. Common
backpressure: max-concurrent cap (semaphore-gated accept), per-connection buffer
caps, configurable `listen` backlog, load-shed policy.

## TLS

**v1 bundles BoringSSL** (statically linked) — one code path on all OSes, TLS 1.3
+ ALPN, a memory-BIO interface that composes with the async reactor (no fd
ownership; the Cajeta layer pumps ciphertext). An internal `TlsBackend` keeps an
mbedTLS swap mechanical.

```cajeta
#TlsStream tls = TlsStream.clientSystemTrust(#sock, host.bytes, host.byteLength);
tls.offerAlpn(alpn, alpnLen); tls.handshake();   // parks the fiber; validates chain
tls.write(plaintext, len); int32 n = tls.read(buf, buf.count());
int32 pl = tls.negotiatedAlpn(out, max);
```

Cert validation (hostname SAN/CN + wildcard, chain vs OS trust store with bundled
CA fallback, expiry), SNI from the connect host, ALPN. Server side
(`TlsListener` / `TlsStream.server(...)`): PEM cert+key, termination, ALPN select.
The handshake parks on the reactor — a slow handshake never blocks a carrier.

## URI

RFC 3986 parse + build (`Uri.parse`/`percentEncode`/`resolve`), component-aware
percent-encoding, order-preserving query multi-map, IPv6-literal authorities,
§5.4 reference resolution (for relative redirect `Location` headers — consumed by
cajeta-http). A small, broadly-useful util, kept in stdlib.

## Error model

`cajeta.io.net.NetException extends cajeta.error.RecoverableException`. A
`cajeta_net_errno` shim maps POSIX `errno` / `WSAGetLastError` to a stable
cross-platform enum. Transport/TLS/DNS/URI subtypes: `ConnectionRefused`,
`ConnectionReset`, `ConnectionAborted`, `AddressInUse`, `AddressNotAvailable`,
`HostUnreachable`/`NetworkUnreachable`, `BrokenPipe`, `TimedOut`, `WouldBlock`
(value, not thrown), `UnknownHost`/`ResolutionFailed`, `TlsHandshakeFailed`/
`CertificateInvalid`/`TlsProtocolError`, `MalformedUri`. (HTTP/WS parse errors
live in cajeta-http, extending `NetException`.) A companion
`io/net/Errors.md` errno→exception chart lands with the hierarchy.

## Native intrinsics

`__cajeta_net_*` in `runtime/native/cajeta_runtime.c`, via the `Cajeta.*`
intrinsic bridge:

- **Sockets:** `_socket`, `_bind`, `_listen`, `_accept`, `_connect`, `_send`,
  `_recv`, `_sendto`, `_recvfrom`, `_close`, `_shutdown`, `_setsockopt`,
  `_getsockopt`, `_set_nonblocking`, `_sockaddr_pack`/`_unpack`.
- **DNS:** `_getaddrinfo`, `_freeaddrinfo`.
- **Reactor:** `_reactor_init`, `_register`, `_deregister`, `_await_readable`,
  `_await_writable` (park + wake), reusing `__cajeta_parked_head` + the carrier deque.
- **TLS:** `__cajeta_tls_*` (memory-BIO: `_feed_ciphertext`/`_read_plaintext`/
  `_write_plaintext`/`_pull_ciphertext`/`_handshake_step` — no fd ownership).

Windows uses Winsock (`WSAStartup` at init, IOCP reactor); the errno shim
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

Transport order (the HTTP layer is now cajeta-http's plan): sockets + reactor →
addresses + DNS → TLS → UDP/multicast → framing strategies → URI. Phased build:
[`../../../agents/cajeta/net/cajeta-net-plan.md`](../../../agents/cajeta/net/cajeta-net-plan.md).
