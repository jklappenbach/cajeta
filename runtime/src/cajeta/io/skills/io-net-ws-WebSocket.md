---
id: io-net-ws-WebSocket
applies-to: [cajeta/io/net/ws/WebSocket]
title: WebSocket — RFC 6455 connection façade (send/sendBinary/receive/close, write-lock serialized)
description: Application-facing WebSocket; forServer/forClient factories over a borrowed AsyncReader/AsyncWriter, send/sendBinary/receive(next WsMessage)/close, write lock makes concurrent reader+writer fibers safe.
---

# WebSocket

The **application-facing façade** for a live RFC 6455 connection in `cajeta.io.net.ws`.
Use it to `send` text, `sendBinary` bytes, `receive` the next reassembled `WsMessage`,
and `close(code, reason)`. It binds the pure protocol layers (frame codec, fragmentation,
control frames, the `WsProtocol` read engine) to a fiber-parking `AsyncReader`/`AsyncWriter`
transport — you talk to *this* type, not to those layers.

**Access point:** yes — but you do **not** call the constructor. Obtain an instance from
`WebSocket.forServer(...)` or `WebSocket.forClient(...)`. The no-arg `WebSocket()` ctor is
an internal field-pinning shim, not a user entry point.

This type does **not** perform the HTTP upgrade handshake and does **not** open or close
the underlying socket — it assumes an already-upgraded connection and **borrows** the
reader/writer. The upgrade (`WsServerHandshake`/`WsClientHandshake`) and socket ownership
belong to the entry point (NET-10.7). There is no `ping()`/`pong()` here: pongs are
automatic (see Lifecycle/auto-pong), and an outbound ping is not exposed in v1.

## Construction & ownership

```cajeta
import cajeta.io.net.ws.WebSocket;
import cajeta.io.net.ws.WsMessage;
import cajeta.io.net.ws.WsCloseCode;
import cajeta.io.net.ws.ConnectionClosedException;
import cajeta.io.net.AsyncReader;
import cajeta.io.net.AsyncWriter;

// reader/writer are already wired over the accepted, upgraded socket by the
// entry point; the WebSocket BORROWS them and never closes them.
#WebSocket ws = WebSocket.forServer(reader, writer);   // owned; you drop it
ws.withMaxMessageLength(1 << 20);                       // optional, chains; 1009 if exceeded

// Server echo loop — one reader fiber owns receive():
try {
    while (true) {
        WsMessage m = ws.receive();                    // owned; see below
        if (m.isText()) {
            ws.sendBinary(m.getPayload());             // payload borrowed by send
        }
    }
} catch (ConnectionClosedException e) {
    int32 code = e.closeCode;                          // peer's close code (RFC §7.4)
}
```

- `static #WebSocket forServer(AsyncReader reader, AsyncWriter writer)` — server end:
  inbound frames MUST be masked, outbound frames are sent **unmasked**. **Returns an
  owned `WebSocket`** (the `#`); you free it / let scope drop it. `reader`/`writer` are
  **borrowed** — not consumed, not closed here.
- `static #WebSocket forClient(AsyncReader reader, AsyncWriter writer)` — client end:
  inbound unmasked, outbound masked per frame. **Caveat:** the default masking key is a
  per-frame placeholder **flagged not-for-production** (no CSPRNG in-tree yet); the server
  path never masks and is unaffected. Same borrow semantics.
- `WebSocket withMaxMessageLength(int64 maxBytes)` / `withAutoPong(boolean)` — config,
  return `this` for chaining.

## The methods that matter

- `void send(String text)` — one unfragmented TEXT frame (the string's UTF-8 bytes,
  copied; you keep `text`). Parks the fiber until the bytes are accepted.
- `void sendBinary(int8[] data)` — one unfragmented BINARY frame. `data`'s bytes are
  **copied**; you keep ownership of `data`.
- `#WsMessage receive()` — the next whole text/binary message. **Returns an owned
  `WsMessage`** (the `#`) — its `payload` is yours to keep/free; do not assume the engine
  retains it. See `cajeta/io/net/ws/WsMessage` for `isText()/isBinary()/getPayload()`.
  Parks the fiber across as many socket reads as the message takes. Control frames are
  handled transparently mid-loop (auto-pong; reciprocal close).
- `void close(int32 code, String reason)` — start the close handshake (sends a CLOSE
  frame, marks local closed). **Idempotent** — a second `close` is a no-op. `code` MUST be
  sendable (`WsCloseCode.isSendable`) and `reason` ≤ 123 UTF-8 bytes.
- `boolean isCloseSent()` / `boolean isPeerClosed()` — handshake state.
- `WsCloseReason peerCloseReason()` — the peer's `{code, reason}` once observed, else
  `null`. **Borrowed** — owned by the engine, valid for the connection's lifetime; copy
  the reason text to keep it past the connection.

## State & concurrency — the write lock

A WebSocket is full-duplex and the **standard pattern is two fibers on one connection**:
a reader fiber (`while (true) ws.receive()`) and a writer fiber (`ws.send(...)`). This is
safe because every frame emission — your `send`/`sendBinary`/`close` **and** the reader's
auto-pong/reciprocal-close — goes through an internal write `Lock` held for one frame's
whole `encode + writeAll + flush`, so two frames never interleave on the wire. The lock is
fiber-aware (a contending writer parks, never blocks the carrier thread).

Reads are **not** locked: exactly **one** reader fiber may own `receive()`. Two concurrent
readers is a misuse (interleaved `receive` splits messages) and is out of scope.

## Lifecycle

- The `WebSocket` is owned by whoever called `forServer`/`forClient`; dropping it destroys
  the write lock, decoder, and engine. It does **not** close the borrowed reader/writer or
  the socket — the entry point closes those **after** the close handshake.
- After `close()`, `send`/`sendBinary` raise `ConnectionClosedException` (no data may
  follow a CLOSE, RFC §5.5.1).
- **Auto-pong** is on by default: an inbound ping is answered inside `receive()` without
  surfacing to you and without disturbing a fragmented message in progress.

## Errors raised — all via `receive()` / `send*` / `close`

`ConnectionClosedException` (extends `WebSocketException` → `NetException`) is the one you
catch around the reader loop. It is raised by `receive()` when:
- the peer sends a CLOSE — carries the peer's `closeCode` (RFC §7.4); a no-status close
  surfaces `WsCloseCode.NO_STATUS` (1005);
- the transport hits EOF with no CLOSE frame — `WsCloseCode.ABNORMAL` (1006).

`receive()` also propagates `ProtocolViolationException` and `MessageTooLargeException`
(1009, from `withMaxMessageLength`) from the codec/engine unchanged. `send*` after a local
close raises `ConnectionClosedException`; `close` with a bad code/reason raises
`ProtocolViolationException` (via `WsControlFrames.close`). Read `closeCode` from the
caught exception as a public field (`e.closeCode`).
