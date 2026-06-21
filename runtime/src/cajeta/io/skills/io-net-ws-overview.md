---
id: io-net-ws-overview
applies-to: [cajeta/io/net/ws]
title: WebSocket (RFC 6455) package map — façade, protocol layers, handshakes, values, errors
description: Route WebSocket work — open with WsUpgrade, drive WebSocket.send/receive/close, or use the pure WsFrame/WsProtocol layers; ownership and the close-via-exception model.
---

# WebSocket (RFC 6455) — package map

`cajeta.io.net.ws` is the **RFC 6455 WebSocket** stack. For almost all application code there is exactly one entry point and one façade:

- **Open a connection → `WsUpgrade.acceptServer` / `WsUpgrade.connectClient`.** They run the opening handshake over an already-connected `AsyncReader`/`AsyncWriter` and hand back a live, role-correct `#WebSocket`. Do **not** construct `WebSocket` directly — `forServer`/`forClient` exist but assume the `101` handshake already happened; let `WsUpgrade` do it.
- **Talk over it → `WebSocket`** (`send`/`sendBinary`/`receive`/`close`).

Everything else in the package is either a **pure protocol layer** you only touch when building/testing framing without a socket, or a **value/enum/exception** carried across those APIs.

Routing:

- Need a live socket-backed connection → `WsUpgrade` then `WebSocket`.
- Need to decode/encode frame bytes with no I/O (tests, custom transport) → `WsFrameDecoder` / `WsFrameEncoder` → `WsMessageAssembler` → `WsProtocol`.
- Need to build/inspect a control frame (ping/pong/close) → `WsControlFrames`.
- Need just the handshake protocol (HttpRequest↔HttpResponse, no socket) → `WsServerHandshake` / `WsClientHandshake`.
- **Not here:** no socket/TCP/TLS connect (that's `cajeta.io.net` `TcpStream`/`tls`), no `permessage-deflate`/extensions (RSV bits are a violation today), no production-grade randomness — client mask keys and `Sec-WebSocket-Key` use *placeholder* generators flagged not-for-production (a CSPRNG lands later).

## Inventory map

**Entry points (you call / instantiate these):**

- **`WsUpgrade`** — the socket-facing glue. `acceptServer(reader, writer)` (server), `connectClient(reader, writer, uri, keySeed)` (client). Returns `#WebSocket`.
- **`WebSocket`** — the live connection façade (NET-10.6). The only stateful socket-bound object an app holds.

**Pure protocol layers (no I/O; pin against golden vectors):**

- **`WsFrameDecoder`** — incremental byte→`WsFrame` decoder. `forServer()`/`forClient()` fix the masking direction; `feed(data,len)` then drain `hasFrame()`/`nextFrame()`.
- **`WsFrameEncoder`** — `WsFrame`→wire bytes. `encode(frame, maskKey)` (client; pass a 4-byte key) / `encodeUnmasked(frame)` (server).
- **`WsMessageAssembler`** — stitches data + continuation frames into one `WsMessage` (NET-10.4). `accept(frame)` returns the message on `FIN`, else `null`.
- **`WsProtocol`** — the read-side engine (NET-10.6) the façade drives: `onFrame(frame)` → a `WsReadAction`. Wraps the assembler + control + close logic; pure decision, no socket writes.
- **`WsControlFrames`** — static builders/parsers for ping/pong/close frames (`pongFor`, `close`, `reciprocalClose`, `parseClose`).
- **`WsServerHandshake` / `WsClientHandshake`** — the opening-handshake protocol as pure HTTP-message logic (`accept(request)`→`HttpResponse`; `buildRequest`/`requireAccept`).

**Support types (values / enums / exceptions — never instantiate as an access point):**

- **Values:** `WsMessage` (reassembled text/binary message: `opcode` + owned `payload`), `WsFrame` (one decoded frame: `fin`/`opcode`/`masked`/owned `payload`), `WsReadAction` (the engine's decision: `NONE`/`MESSAGE`/`SEND_FRAME`/`PING`/`CLOSED`), `WsCloseReason` (`{code, hasCode, reason}`).
- **Enums/constant holders (no instances):** `WsOpcode` (`TEXT`/`BINARY`/`CLOSE`/`PING`/`PONG` + `isControl`/`isData`/`isReserved`), `WsCloseCode` (`NORMAL` 1000 … `INTERNAL_ERROR` 1011, plus never-on-the-wire `NO_STATUS` 1005 / `ABNORMAL` 1006; `isSendable`).
- **Exceptions:** see the family below.

## Intra-package collaboration

The read path is a pipeline the façade hides: `WebSocket.receive()` → `AsyncReader.read` into a scratch buffer → `WsFrameDecoder.feed`/`nextFrame` → `WsProtocol.onFrame` → a `WsReadAction`, which the façade acts on (deliver the `WsMessage`, write an auto-pong/reciprocal close under the write lock, or raise on close). The write path: `send`/`sendBinary`/`close` build a `WsFrame` (via `WsControlFrames` for close) and funnel through one `writeFrame` that holds a single write `Lock` for the whole `encode + writeAll + flush`. `WsProtocol` itself delegates fragmentation to `WsMessageAssembler` and control/close to `WsControlFrames`.

## Package invariants (apply across the package)

- **Ownership of payloads.** `WsMessage`/`WsFrame` own their `payload` (`int8[]`). Factories `WsMessage.of` / `WsFrame.of` and the close builders take `#` ownership of the payload you pass. `receive()` returns `#WsMessage` — **owned by the caller**; `WsReadAction.takeMessage()`/`takeFrame()` detach (null the field) so the action no longer aliases what it hands off. `send(text)`/`sendBinary(data)` **copy** the bytes — the caller keeps ownership of its argument.
- **Transport is borrowed.** `WebSocket` (and `WsUpgrade`) **borrow** the `AsyncReader`/`AsyncWriter`; the caller that opened the socket owns them and must keep them alive for the connection's lifetime and `close()` the underlying `TcpStream` after the close handshake. The `WebSocket` does not close the transport.
- **Masking is by role, not by call.** Server: inbound MUST be masked, outbound MUST NOT be — pick `forServer`. Client: the reverse, outbound masked with a fresh per-frame key. `WsUpgrade` wires the right role; never mix directions.
- **Close is signalled by an exception, not a return.** A clean reader loop is `while (true) ws.receive();` — when the peer closes, `receive()` raises `ConnectionClosedException` carrying the peer's `closeCode`; an abnormal transport EOF raises it with `WsCloseCode.ABNORMAL` (1006). After local `close()`, `send`/`sendBinary` raise `ConnectionClosedException`. `receive()` never returns `null`.
- **Concurrency.** One reader fiber + one writer fiber on the same `WebSocket` is the supported pattern; the write lock serializes frame emission across them. Two concurrent readers is misuse.
- **Errors all descend from `WebSocketException`** (itself a `cajeta.io.net.NetException`). Codec/engine faults propagate unchanged through `receive()`.

## Error family

`NetException` → `WebSocketException` (root; `kind == KIND_INVALID` 12) →

- **`ProtocolViolationException`** — malformed framing (reserved opcode, RSV set, mask-direction mismatch, fragmented/over-125 control frame, bad close payload). Carries `position` (byte offset, `-1` if N/A). Maps to close code 1002.
- **`MessageTooLargeException`** — reassembled message past the configured cap. Carries `limit`. Maps to close code 1009. Cap it via `WebSocket.withMaxMessageLength(maxBytes)`.
- **`HandshakeRejectedException`** — opening handshake failed (bad method/headers/version on the server side; non-`101`/bad `Sec-WebSocket-Accept` on the client). Carries `httpStatus` (400, or 426 for version mismatch).
- **`ConnectionClosedException`** — the connection closed. **`kind == KIND_OTHER` (99)**, not `KIND_INVALID` — it is the stream's end, not a parse fault. Carries `closeCode` (a `WsCloseCode`), nullable `reason`, and `isClean` (true when a CLOSE frame was exchanged, even `NO_STATUS`; false for `ABNORMAL`).

A single `catch (WebSocketException e)` nets every WS fault; `catch (NetException e)` also nets transport faults.

## Idiomatic example — a server echo (mirrors `WsEntryPointTests`)

```cajeta
import cajeta.io.net.TcpStream;
import cajeta.io.net.AsyncReader;
import cajeta.io.net.AsyncWriter;
import cajeta.io.net.ws.WebSocket;
import cajeta.io.net.ws.WsMessage;
import cajeta.io.net.ws.WsUpgrade;

public static async int32 wsServer(#TcpListener listener) {
    TcpStream sock = listener.acceptAsync();          // caller owns the socket
    AsyncReader reader = heap AsyncReader(sock);       // borrowed by the WebSocket
    AsyncWriter writer = heap AsyncWriter(sock);
    WebSocket ws = WsUpgrade.acceptServer(reader, writer);  // #WebSocket, owned
    WsMessage m = ws.receive();                        // #WsMessage, owned here
    int8[] pay = m.getPayload();                        // borrowed view into m
    ws.sendBinary(pay);                                 // bytes are COPIED into the frame
    sock.close();                                       // the owner closes the transport
    return m.length();
}
```

A long-lived reader loops `while (true) { WsMessage m = ws.receive(); ... }` and lets the `ConnectionClosedException` unwind it on close.

## Downward pointers

For per-type detail go to the class/component skills under `cajeta/io/net/ws/*` (the protocol-layer codec/assembler/engine, and `WebSocket` itself). For the transport these borrow, see `cajeta/io/net` (`TcpStream`, `AsyncReader`/`AsyncWriter`, `tls`); for the handshake's HTTP messages see `cajeta/io/net/http`.
