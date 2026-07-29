---
id: io-net-tcp
applies-to: [cajeta/io/net/TcpStream, cajeta/io/net/TcpListener, cajeta/io/net/ByteChannel]
title: TCP sockets + the ByteChannel transport seam
description: TcpListener accepts/TcpStream connects; sync ops block, *Async ops park the fiber over the reactor; ByteChannel abstracts plaintext vs TLS; read/readAsync return 0 at EOF.
---

# TCP sockets + the `ByteChannel` seam

Three cooperating types for stream TCP:

- **`TcpListener`** — passive listening socket. `bind` it, then `accept()` (blocking)
  or `acceptAsync()` (fiber-parking) to get one connected **`TcpStream`** per client.
- **`TcpStream`** — a connected peer socket (client or server side). The read/write
  surface. Implements **`ByteChannel`**.
- **`ByteChannel`** — the async transport *interface* (`readAsync`, `writeAllAsync`,
  `readWithin`, `close`) that buffered I/O and the HTTP/WS codecs read/write *through*,
  so they don't care whether the transport is plaintext (`TcpStream`) or encrypted
  (`tls/TlsStream`).

## Sync vs async — the one decision that matters

| Op kind | Methods | Behavior |
|---|---|---|
| **sync** | `read`, `write`, `writeAll`, `accept`, `connect`, options, `close` | intrinsic-lowered to a `__cajeta_net_*` native call; **blocks the carrier thread**. Fine for tests/single-threaded/loopback. |
| **async** | `readAsync`, `writeAsync`, `writeAllAsync`, `readWithin`, `acceptAsync`, `connectAsync` | pure-Cajeta readiness loops; a would-block **parks the fiber** on `reactor/Reactor` and retries on readiness. **The carrier never blocks** (the no-carrier-blocks invariant). Use these inside server/client fibers. |

The async forms call `ensureAsyncReady()` once (idempotent: sets the fd non-blocking +
`Reactor.init()`), then loop the lowered non-blocking op, classifying `-1` via the
native `isWouldBlockNative()` into "park & retry" vs "real error → throw".

`ByteChannel` exposes **only** the async four. The sync `read`/`write`, socket options,
and the `WOULD_BLOCK = -1` sentinel are `TcpStream`-concrete, not on the interface.

## Object graph & ownership across boundaries

```
SocketAddress ──#──> TcpListener.bind  ──> #TcpListener
                                            │ accept()/acceptAsync()
                                            ▼
SocketAddress ──#──> TcpStream.connect ──> #TcpStream  (impl ByteChannel)
                     /connectAsync                 ▲
                                                   │ also returned by accept*
AsyncReader/AsyncWriter ── read/write through ──> ByteChannel ──┬─ TcpStream (plaintext)
   (→ dev.cajeta.http HttpServer/HttpClient, WebSocket)                         └─ TlsStream (TLS)
```

- `TcpStream.connect(#SocketAddress)` / `connectAsync(#SocketAddress)` **take ownership**
  of the address (the `#`). Both **return an owned `#TcpStream`** — caller frees.
- `TcpListener.bind(SocketAddress)` **borrows** the address (no `#`; pass the value, not
  `#value`). It **returns an owned `#TcpListener`**, already listening (backlog 128).
- `accept()` / `acceptAsync()` **return a fresh owned `#TcpStream`** per connection.
- Buffers: `read*`/`write*` operate in place on a caller-owned `int8[]`; no transfer.

## Lifecycle — close() and EOF

- `close()` is **idempotent** on both types (lowers to `__cajeta_net_close(fd)`, then
  sets `fd = -1`; a second call is a no-op). `TcpStream` also has a **destructor that
  calls `close()`**, so a dropped stream never leaks a descriptor — but close eagerly to
  release the fd promptly. `TcpListener` has no destructor: **`close()` it explicitly.**
- **EOF = 0.** `read`/`readAsync`/`readWithin` return the count read (`>= 0`); a return
  of **`0` is an orderly peer close (EOF), not an error**. This is the terminate-on-`0`
  contract the buffered readers drive — loop until `0`.
- `writeAll`/`writeAsync`/`writeAllAsync` raise **`BrokenPipeException`** when the peer
  has stopped accepting data; other socket failures raise the mapped `NetException`
  subtype (`ConnectionRefusedException`, `TimedOutException`, …) via `NetErrors.fromErrno`.
- `readWithin(buf, off, len, timeoutMs)` bounds the total wait; raises
  `TimedOutException` on the deadline. A **non-positive `timeoutMs` degenerates to the
  unbounded `readAsync`**. The reactor registration is dropped on every exit path.

## When to use which

- **`TcpStream` directly** for raw plaintext byte streams. For TLS, don't construct a
  `TcpStream` and encrypt yourself — use `tls/TlsStream` (same `ByteChannel` API).
- **Program against `ByteChannel`**, not the concrete type, in any code that should run
  over both `http://`/`ws://` and `https://`/`wss://` — that is the whole point of the seam.
- **`acceptAsync` over `accept`** in a real server (one accept fiber shouldn't pin a
  carrier). `accept` is for tests/simple blocking flows.

## Worked example — loopback echo (sync, mirrors `NetLoopbackEchoTest`)

```cajeta
import cajeta.lang.String;
import cajeta.io.net.IpAddress;
import cajeta.io.net.SocketAddress;
import cajeta.io.net.TcpStream;
import cajeta.io.net.TcpListener;

IpAddress la = IpAddress.loopbackV4();
SocketAddress bindAddr = SocketAddress.of(#la, 0);   // :0 → ephemeral port
TcpListener listener = TcpListener.bind(bindAddr);    // bindAddr is borrowed
int32 port = listener.boundPort();                    // resolve the kernel-assigned port

IpAddress ca = IpAddress.loopbackV4();
SocketAddress connAddr = SocketAddress.of(#ca, port);
TcpStream client = TcpStream.connect(#connAddr);      // # transfers connAddr
TcpStream server = listener.accept();                 // owned #TcpStream

int8[] ping = heap int8[4];
ping[0] = (int8) 112; ping[1] = (int8) 105;
ping[2] = (int8) 110; ping[3] = (int8) 103;           // "ping"
client.write(ping, (int64) 0, (int64) 4);

int8[] rbuf = heap int8[4];
int64 got = server.read(rbuf, (int64) 0, (int64) 4);  // got == 0 would mean EOF
server.write(rbuf, (int64) 0, got);                   // echo back

int8[] echo = heap int8[4];
client.read(echo, (int64) 0, (int64) 4);

client.close();                                        // idempotent
server.close();
listener.close();                                      // listener: close explicitly
```

The async path is the same shape with `acceptAsync()` / `connectAsync(#addr)` /
`readAsync` / `writeAllAsync`, each parking the fiber instead of blocking.

## What this component does NOT do (don't hunt)

- **No UDP** — datagrams are `UdpSocket` (separate type, same package).
- **No full local `SocketAddress` yet** — `TcpListener.localAddress()` is a placeholder
  returning `0.0.0.0:0`; use **`boundPort()`** to learn the ephemeral port.
- **No `read`/`write` sync forms, options, or `shutdown` on `ByteChannel`** — those are
  `TcpStream`-only; the interface is the async four plus `close`.
- **No reconnect/retry, no framing/length-prefixing** — `read` may return a short count;
  loop yourself (or use `writeAll`/`AsyncReader`).
- Per-method codegen/native-lowering detail lives on the class skills; the reactor
  parking primitives live in `cajeta/io/net/reactor/Reactor`.
