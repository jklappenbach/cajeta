---
id: io-net-overview
applies-to: [cajeta/io/net]
title: cajeta.io.net — sockets, the ByteChannel seam, buffering, and servers
description: Neighborhood map for cajeta.io.net — pick TCP/UDP socket, buffered async reader/writer, Server accept model, or address parsing, and route to http/ws/tls/dns/uri/reactor.
---

# cajeta.io.net

The transport layer of `cajeta.io`: TCP/UDP **sockets**, the **`ByteChannel`** seam that
lets plaintext and TLS share one read/write contract, **buffer** primitives, **buffered
async reader/writer** codecs build on, the **`Server`** accept core + two models, and the
**address value** types. The higher-level protocols (`http`, `ws`, `tls`, `dns`, `uri`)
are subpackages reached from here; this package is the byte-level plumbing under them.

All async I/O **parks the fiber, never the carrier** (the reactor wakes it when the fd is
ready), so a connection blocked on a slow peer yields its carrier to other fibers. This
is the library-wide invariant — see the `cajeta.io` library skill.

## Task → entry point

| You want to… | Start with |
| --- | --- |
| Connect a TCP client | `TcpStream.connect(#SocketAddress)` (or `connect(host, port)` to resolve via DNS) |
| Connect without blocking the carrier | `TcpStream.connectAsync(#SocketAddress)` |
| Listen / accept TCP | `TcpListener.bind(addr)` → `accept()` / `acceptAsync()` |
| Run a real TCP server (lifecycle + drain) | `Server.bind(addr, handler)` / `Server.builder()` |
| Read tokens (lines, exact N) off a connection | `AsyncReader` over the stream |
| Buffer + flush writes | `AsyncWriter` over the stream |
| UDP datagrams | `UdpSocket.bind(local)` → `sendTo` / `recvFrom` |
| Parse / build an address | `SocketAddress.parse("h:p")`, `IpAddress.parse`, `SocketAddress.of(#ip, port)` |
| Pool/reuse byte buffers | `BufferPool.acquire()` / `release(#buf)` |
| Map an OS error to a typed exception | `NetErrors.fromErrno(ordinal, detail)` |
| HTTP / WebSocket / TLS / DNS / URI | the `http` / `ws` / `tls` / `dns` / `uri` subpackages |

**Not here:** there is no event-loop you write callbacks against (use fibers + the
buffered reader/writer); no built-in connection *pool* for clients; no HTTP/2 (`http` is
HTTP/1.1); `Reactor` (the `reactor` subpackage) is the fd-readiness park primitive the
async ops drive — you rarely call it directly.

## Inventory

**Entry-point types** (instantiate / call statically):
- `TcpStream` — connected TCP socket; the plaintext `ByteChannel` implementor. Sync
  `read`/`write`/`writeAll`, async `readAsync`/`writeAsync`/`writeAllAsync`/`readWithin`,
  socket options (`setNoDelay`, `setKeepAlive`, …), `shutdown`, `close`.
- `TcpListener` — bound listening socket; `accept`/`acceptAsync`/`acceptFd`,
  `boundPort`, `localAddress`, `close`.
- `UdpSocket` — datagram socket; `sendTo`/`recvFrom` (connectionless) or
  `connect`+`send`/`recv`; returns a `RecvResult`.
- `Server` — TCP server core: bind, accept loop, per-connection dispatch, graceful
  `shutdown(Duration)`. Built directly via `bind` or fluently via `ServerBuilder`.
- `SharedPoolServer` — Model B dispatch (bounded worker-fiber pool over a readiness
  `Channel`); selected through `ServerModel`, not constructed by hand.
- `AsyncReader` / `AsyncWriter` — buffered token reader / write-coalescer over any
  `ByteChannel` (this is what HTTP/WS codecs sit on).
- `BufferPool` — slab pool of reusable `ByteBuffer`s.

**Seam / buffer types:**
- `ByteChannel` — the interface (`readAsync`, `writeAllAsync`, `readWithin`, `close`);
  implementors are `TcpStream` (plaintext) and `tls/TlsStream` (encrypted). Code written
  against `ByteChannel` runs unchanged over cleartext or TLS.
- `ByteBuffer` — growable cursor buffer (`put`/`write`/`read`/`compact`/`reserve`).
- `RingBuffer` — fixed-capacity ring with low/high watermarks (backpressure signalling).

**Support / value types** (do not "start here"): `IpAddress`, `SocketAddress`,
`RecvResult`, `AddressFamily`, `SocketOption`, `ServerModel`, `ServerState`,
`ConnectionLimits`, `ConnectionLimiter`, `LoadShedPolicy`, `Headers` (HTTP header map,
used by the `http` subpackage), and the `NetException` family + `NetErrors` factory.

## How they collaborate

`TcpListener.bind` → `accept()` yields an **owned** `TcpStream`. A `TcpStream` *is* a
`ByteChannel`, so you wrap it in an `AsyncReader` / `AsyncWriter` to read lines / exact
counts and to coalesce writes; those buffered types **borrow** the channel (they do not
close it). `Server` automates the accept loop: it hands each accepted `TcpStream` to your
`(TcpStream) -> void` handler — which **owns and closes** that connection — and tracks
in-flight handlers so `shutdown` can stop accepting and drain before settling `STOPPED`.
Model selection (fiber-per-connection vs shared pool) and connection caps
(`ConnectionLimits` / `ConnectionLimiter`) layer on the same core.

Errors are **typed exceptions**: socket ops raise a `NetException` subtype
(`ConnectionRefusedException`, `ConnectionResetException`, `TimedOutException`,
`BrokenPipeException`, `AddressInUseException`, …), mapped from the normalized OS ordinal
by `NetErrors.fromErrno`. Two non-exception conventions to know: a `readAsync`/`read` of
**`0` is an orderly peer close (EOF), not an error**, and **`WouldBlock` is surfaced as a
value** on the non-blocking hot path (the reactor retries) rather than thrown. Address
parsing raises `MalformedAddressException`.

## Ownership (the `#` rules in this package)

- `SocketAddress.of(#ip, port)`, `TcpStream.connect(#addr)`, `connectAsync(#addr)` **take
  ownership** of the passed address — do not reuse your reference after.
- `accept()` / `acceptAsync()` / `connect*` return an **owned** `#TcpStream`; the caller
  (or, under `Server`, the handler) must `close()` it. `Server` hands off ownership and
  does not double-close.
- `AsyncReader` / `AsyncWriter` **borrow** the `ByteChannel` — whoever opened the socket
  closes it. `AsyncReader.readUntil` returns an **owned** `#int8[]`.
- `BufferPool.acquire()` returns an **owned** `#ByteBuffer`; `release(#buf)` **takes
  ownership** back (the pool retains it for reuse — skipping the `#` leaves a dangling
  free-list slot → use-after-free). Oversized/grown buffers are dropped, not pooled.
- Reader/writer/buffers are **single-fiber owned** (one drives one connection, no
  locking). `close()` on a stream/listener/socket is idempotent.

## Idiomatic example — async loopback echo (from NetAsyncEchoTest)

```cajeta
import cajeta.io.net.IpAddress;
import cajeta.io.net.SocketAddress;
import cajeta.io.net.TcpStream;
import cajeta.io.net.TcpListener;

IpAddress la = IpAddress.loopbackV4();
SocketAddress bindAddr = SocketAddress.of(#la, 0);     // # transfers the address
TcpListener listener = TcpListener.bind(bindAddr);
int32 port = listener.boundPort();                     // 0 → kernel-assigned

IpAddress ca = IpAddress.loopbackV4();
SocketAddress connAddr = SocketAddress.of(#ca, port);
TcpStream client = TcpStream.connect(#connAddr);       // owned; caller closes
TcpStream server = listener.accept();                  // owned; caller closes

int8[] ping = heap int8[4]; /* fill 'p','i','n','g' */
client.writeAsync(ping, (int64) 0, (int64) 4);

int8[] rbuf = heap int8[4];
int64 got = server.readAsync(rbuf, (int64) 0, (int64) 4);   // 0 would mean EOF
server.writeAllAsync(rbuf, (int64) 0, got);

client.close(); server.close(); listener.close();
```

Addresses can also come from text: `SocketAddress.parse("127.0.0.1:8080")` /
`SocketAddress.parse("[::1]:8080")` (bracketed IPv6 required); a bare IP without a port,
or unbracketed IPv6, raises `MalformedAddressException`.

## Downward pointers

- Buffers (cursors, watermarks, pooling): `ByteBuffer`, `RingBuffer`, `BufferPool` class skills.
- Servers (models, builder, drain, caps): `Server`, `ServerBuilder`, `ServerModel`, `SharedPoolServer`.
- Protocols: `cajeta/io/net/http` (HTTP/1.1 client+server), `.../ws` (WebSocket),
  `.../tls` (TLS termination via `TlsStream`/`TlsListener`), `.../dns` (resolution),
  `.../uri` (URI parse/encode).
- Reactor primitive: `cajeta/io/net/reactor/Reactor`.
- The leaf `cajeta.io.Buffer` (SWAR loads) is a *different* type in the parent package —
  not the networking buffers above.
