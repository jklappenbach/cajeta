---
id: io-overview
applies-to: [cajeta.io]
title: cajeta.io library map — Buffer, files, and the net stack (sockets/HTTP/WS/TLS/DNS/URI)
description: Routing table and cross-cutting rules (ownership, close, errors, sync-vs-async) for cajeta.io; pick the entry point and learn the invariants once.
---

# cajeta.io — orientation & routing

`cajeta.io` is the runtime's I/O surface: an in-memory byte `Buffer`, filesystem
access (`cajeta.io.file`), and a full networking stack (`cajeta.io.net` and its
sub-packages). It is built bottom-up around one transport seam — the
`ByteChannel` async byte interface — so plaintext and TLS (and application
protocols built above, like `dev.cajeta.http`'s HTTP and WebSocket) all ride
the same buffered reader/writer.

## Task → entry point

| Want to… | Start with |
| --- | --- |
| Read/write a whole small file in one call | `File.readAllBytes(path)` / `File.writeAllBytes(path, data, len)` (`cajeta.io.file`) |
| Stream a file incrementally | `File.openRead(path)` → `FileReader`; `File.openWrite(path, OpenMode)` → `FileWriter` |
| Seek / random-access / lock a file | `File.open(path, OpenMode)` → seekable `File` handle |
| Reinterpret bytes / unaligned loads (SWAR, serialization) | `Buffer<T>` (`loadU64`, `byteAt`) |
| Make a blocking TCP client | `TcpStream.connect(#SocketAddress)` |
| Make a fiber-friendly TCP client (no carrier block) | `TcpStream.connectAsync(#SocketAddress)` |
| Listen/accept TCP | `TcpListener.bind(SocketAddress)` → `accept()` / `acceptAsync()` |
| Run a concurrency-managed TCP server | `Server.bind(addr, handler)` (NET-4 accept loop, graceful drain) |
| Speak HTTP or WebSocket | the external `dev.cajeta.http` library (HTTP is application-layer, not stdlib) |
| Wrap a socket in TLS | `TlsStream.client/clientSystemTrust/server(#TcpStream, …)` |
| Resolve a hostname | `Dns.resolve(host[, port[, ResolveFamily]])` |
| Parse/build a URL | `Uri.parse(String)` / `Uri.builder()` |
| Datagram (UDP) | `UdpSocket.bind(SocketAddress)` |
| Parse a `host:port` | `SocketAddress.parse(...)` / `SocketAddress.of(#IpAddress, port)` |

Negative rows (capabilities **not** here — don't hunt for them):

- **No directory/path mutation** in `File` — there is no `mkdir`, and
  `writeAllBytes` does **not** create parent directories.
- **No HTTP or WebSocket** — both moved to the external `dev.cajeta.http`
  library; the stdlib ships the transport layer (TCP/UDP/TLS/DNS/URI) only.
- **No drop-on-scope close for file streams yet** (see invariants) — always
  call `close()`.

## Cross-cutting invariants (learn once, apply everywhere)

- **Ownership marker `#`.** A `#` on a return type means *you receive
  ownership*; a `#` on a parameter means *the call takes ownership* (e.g.
  `TcpStream.connect(#SocketAddress addr)` consumes `addr`;
  `File.openRead` returns a `#FileReader` you own). Plain (non-`#`) returns are
  borrowed views — copy if you need to outlive the source.
- **Explicit `close()` is the contract.** `File`, `FileReader`, `FileWriter`
  have **no auto-close-on-drop destructor yet** — forgetting `close()` leaks the
  fd. (Sockets are the exception: `TcpStream` and `UdpSocket` carry a
  `~`-destructor safety net that closes a dropped fd, but still call `close()`
  explicitly.) All `close()` are idempotent and set the fd to `-1`.
- **Errors are exceptions, not Optionals.** File faults descend from
  `cajeta.io.file.IoException`; every network fault descends from
  `cajeta.io.net.NetException` (DNS/TLS/URI included — `dev.cajeta.http`'s
  HTTP/WS exceptions extend it too), each carrying a
  `kind` ordinal. Both extend `RecoverableException`: catch the specific subtype
  or declare it in `throws`. A request-boundary `catch (NetException e)` is the
  universal net-out.
- **Sync vs async I/O model.** Sync ops (`read`/`write`/`close`, socket options)
  are intrinsic-lowered and **block the carrier thread**. Async ops
  (`readAsync`/`writeAllAsync`/`connectAsync`/`acceptAsync`, and everything
  `Server` drives) **park the fiber** on the reactor
  and resume when the fd is ready — the carrier never blocks. Use the async
  forms inside server handlers and fibers.
- **EOF / sentinels.** `ByteChannel.readAsync` returns `0` on orderly peer close
  (EOF); `FileReader.read` returns a short count (`< max`, incl. `0`) at EOF;
  non-blocking forms return the `TcpStream.WOULD_BLOCK` (`-1`) value.
- **Null.** Numeric IP literals and names both go through `Dns.resolve`; a null
  host raises rather than returning null.

## End-to-end example (TCP echo round-trip)

```cajeta
import cajeta.io.net.SocketAddress;
import cajeta.io.net.TcpStream;

TcpStream conn = TcpStream.connect(SocketAddress.parse("127.0.0.1:7000"));  // resolves, connects (fiber-parking)
int8[] msg = heap int8[5];
conn.write(msg, (int64) 0, (int64) 5);
int64 n = conn.read(msg, (int64) 0, (int64) 5);
conn.close();
```

(HTTP/WebSocket live in the external `dev.cajeta.http` library — the stdlib
ships the transport layer only.)

Streaming file round-trip (note the explicit `close()`):

```cajeta
import cajeta.io.file.File;
import cajeta.io.file.FileWriter;
import cajeta.io.file.FileReader;
import cajeta.io.file.OpenMode;

FileWriter w = File.openWrite("/tmp/out.txt", OpenMode.WRITE);
w.writeString("hello\n");
w.flush();
w.close();

FileReader r = File.openRead("/tmp/out.txt");
int8[] buf = heap int8[64];
int32 n = r.read(buf, 64);   // loop until read returns < max (EOF)
r.close();
```

## Disambiguation

- **File access:** `readAllBytes`/`writeAllBytes` (whole-file one-shot) vs
  `openRead`/`openWrite` → reader/writer (streaming, fixed-memory) vs
  `open` → seekable handle (`seek`/`size`/`truncate`/`lock`).
- **TCP connect:** `connect` blocks the carrier (simple/CLI); `connectAsync`
  parks the fiber (servers, concurrency).
- **Server run:** `serve()` blocks the caller; `runAsync()` returns an async
  task you `spawn` then `shutdown(Duration)`.
- **TLS trust:** `clientSystemTrust` (OS CA store, public PKI) vs `client`
  (pinned PEM anchor, private/self-signed).

## Hazards

- `Buffer.loadU64(off)` does **no bounds check** — keep `off` in
  `[0, byteCount-8]`.
- `SocketAddress.parse` requires `host:port` (IPv6 bracketed: `"[::1]:8080"`);
  a bare host is rejected — use `SocketAddress.of` to build programmatically.
- `File.read`/`write` return a count; `0` is EOF, a negative is a hard error
  (the IoException-throwing wrapper is still landing on some paths).
- Calling sync `read`/`write` from inside a fiber handler blocks the whole
  carrier — reach for the `*Async` forms there.

## Setup / preconditions

- Part of the cajeta runtime (`runtime/src/cajeta/io`); built against the
  cajeta-llvm fork. Sockets are POSIX-based; the async path requires the fiber
  runtime + reactor (`cajeta.io.net.reactor.Reactor`).

## Going deeper

Packages: `cajeta.io.file` (filesystem), `cajeta.io.net` (sockets, `Server`,
`ByteChannel`), `cajeta.io.net.dns`, `cajeta.io.net.tls`,
`cajeta.io.net.uri`. See the per-package/class skills for
signatures, return-ownership detail, and protocol sequences.
