---
id: io-net-UdpSocket
applies-to: [cajeta/io/net/UdpSocket]
title: UdpSocket — connectionless datagram socket (bind, sendTo/recvFrom, optional connect)
description: UDP socket access point; bind() factory, sendTo/recvFrom (recvFrom returns RecvResult with count+sender), optional connect for address-less send/recv, recvFromAsync (fiber-parking receive).
---

# UdpSocket

The **connectionless datagram** transport in `cajeta.io.net` — the UDP peer of the
connection-oriented `TcpStream`/`TcpListener`. Use it when you send/receive independent
datagrams (each addressed on its own) rather than a byte stream. One socket can talk to
many peers.

**Access point:** yes — but you do **not** call the constructor. Obtain an instance from
the static factory `UdpSocket.bind(...)`. The public `UdpSocket(int32 fd)` ctor is an
internal field-pinning shim used by the intrinsic codegen, not a user entry point.

## Construction & ownership

```cajeta
import cajeta.io.net.UdpSocket;
import cajeta.io.net.SocketAddress;
import cajeta.io.net.RecvResult;

// Bind to an ephemeral port on the IPv4 wildcard ("0.0.0.0:0" → kernel picks).
UdpSocket sock #= UdpSocket.bind(SocketAddress.parse("0.0.0.0:0"));

// Learn the kernel-assigned local endpoint.
SocketAddress local #= sock.localAddress();        // owned, you free it

// --- unconnected path: each datagram names its destination ---
int8[] msg = ...;                                   // your payload
SocketAddress dest #= SocketAddress.parse("127.0.0.1:9000");
int32 sent = sock.sendTo(msg, 0, msg.length, dest); // bytes handed to kernel

int8[] buf = heap int8[2048];
RecvResult r #= sock.recvFrom(buf, 0, buf.length);  // owned result
int32 n = r.getCount();                             // payload bytes in buf[0..n)
SocketAddress who = r.getFrom();                    // sender — reply target

sock.close();                                       // or just let it drop
```

- `static #UdpSocket bind(SocketAddress local)` — opens a `SOCK_DGRAM` socket and binds
  it. The family (`V4`/`V6`) of `local` selects the socket family. **Returns an owned
  `UdpSocket`** (the `#`); you free it (or let scope drop it — see Lifecycle). `local` is
  borrowed (not consumed) — keep using it. Use `:0` for an ephemeral port and
  `0.0.0.0`/`[::]` for the wildcard address.

## The methods that matter

Unconnected path (talk to any peer):
- `int32 sendTo(int8[] data, int32 offset, int32 length, SocketAddress dest)` — send
  `data[offset .. offset+length)` as one datagram to `dest`. `data`/`dest` borrowed.
  Returns the byte count handed to the kernel.
- `#RecvResult recvFrom(int8[] dst, int32 offset, int32 capacity)` — receive one datagram
  into `dst[offset .. offset+capacity)`. **Returns an owned `RecvResult`** carrying the
  byte `count` and the sender's `from` address. `dst` is borrowed and written in place —
  the result does **not** own or copy it. See `cajeta/io/net/RecvResult`.

Connected path (set a default peer, then send/recv without an address):
- `void connect(SocketAddress peer)` — record a default peer. UDP `connect` does **no
  handshake**; it makes the kernel filter inbound datagrams to `peer` and enables the
  address-less forms. `peer` borrowed.
- `int32 send(int8[] data, int32 offset, int32 length)` — send to the connected peer.
- `int32 recv(int8[] dst, int32 offset, int32 capacity)` — receive from the connected
  peer; returns the byte count only (no sender — it's always `peer`).

Introspection / options:
- `#SocketAddress localAddress()` — the bound local endpoint, resolving the ephemeral
  port after `bind(..,:0)`. **Owned** return.
- `setBroadcast/getBroadcast`, `setTtl/getTtl`, `setRecvBufferSize/getRecvBufferSize`,
  `setSendBufferSize/getSendBufferSize` — typed socket-option accessors (`SO_BROADCAST`,
  TTL/hop-limit, `SO_RCVBUF`, `SO_SNDBUF`).

## Lifecycle

- `void close()` — closes the descriptor; **idempotent** (sets `fd = -1`).
- The destructor `~UdpSocket()` calls `close()`, so a dropped `UdpSocket` never leaks a
  descriptor. Explicit `close()` is therefore optional — call it to release the fd before
  scope end.

## Errors

Failures throw subclasses of `cajeta.io.net.NetException` (which `extends
RecoverableException`) — e.g. `AddressInUseException` from `bind`, `WouldBlockException`
from a non-blocking recv with no datagram ready. `SocketAddress.parse` throws
`MalformedAddressException` on a bad `host:port`. There are no `-1`/sentinel returns on the
socket ops; `sendTo`/`recvFrom`/etc. return real counts and signal failure by throwing.

## RecvResult and 0-length datagrams

`recvFrom` returns `count == 0` for a **legal empty datagram** — UDP has no stream and no
orderly close, so `0` is **not** end-of-stream (unlike a TCP `recv`). `RecvResult.from` is
borrowed-style data carried in the owned result; if you keep the sender address past the
result's lifetime, retain the `RecvResult` (it owns `from`). See `cajeta/io/net/RecvResult`.

## What v1 does NOT do (don't hunt for these)

- **Mostly sync.** `recvFromAsync(dst, offset, capacity)` is the one async form
  (NET-3.3): it parks the FIBER on the reactor until a datagram is ready, then
  receives it — the receive-loop primitive (single receive fiber per socket;
  a spurious wakeup re-parks). There is no `sendToAsync` yet (UDP sends rarely
  block) and no timed variant — bound a wait with `Tasks.withTimeout` around a
  channel hand-off instead.
- **No multicast join/leave** and no generic `setOption` — only the typed option accessors
  listed above.
- **Not a stream.** No connection, no backpressure, no `read`/`write` channel semantics —
  for that use `TcpStream`. `connect` here only pins a default peer; it does not establish
  a connection.
