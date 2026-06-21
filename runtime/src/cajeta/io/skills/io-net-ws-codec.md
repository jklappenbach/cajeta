---
id: io-net-ws-codec
applies-to: [cajeta/io/net/ws/WsFrameDecoder, cajeta/io/net/ws/WsFrameEncoder, cajeta/io/net/ws/WsMessageAssembler, cajeta/io/net/ws/WsControlFrames, cajeta/io/net/ws/WsProtocol]
title: WebSocket protocol pipeline — bytes → frames → messages → read actions
description: Wire the pure WS codec/assembler/protocol layers (RFC 6455) that turn socket bytes into messages and reply frames, with no I/O.
---

# WS protocol pipeline (RFC 6455, no I/O)

These five classes are the **pure logic** behind a WebSocket connection. They do
**no socket I/O** — they transform `int8[]` ↔ frames ↔ messages ↔ decisions. The
transport (`AsyncReader`/`AsyncWriter`) and the `WebSocket` façade do the actual
reads/writes; this pipeline tells the façade *what* to do.

The pipeline flows in one direction on the read side:

```
socket bytes ──► WsFrameDecoder.feed ──► WsFrame (per fragment)
                                            │
                                            ▼
                                       WsProtocol.onFrame ──► WsReadAction
                                            │  (delegates data frames to WsMessageAssembler,
                                            │   control frames to WsControlFrames)
                                            ▼
                          deliver WsMessage / write reply WsFrame / report CLOSE / read on
```

For the write side you build a frame with `WsControlFrames` (or your own `WsFrame.of`)
and serialize it with `WsFrameEncoder.encode`.

## Members and roles

- **`WsFrameDecoder`** — incremental, resumable byte→frame state machine. `feed()` any
  chunk size; complete frames queue up, drain with `hasFrame()`/`nextFrame()`. Built per
  direction: `forServer()` requires masked client frames, `forClient()` requires unmasked
  server frames. Enforces per-frame size cap.
- **`WsFrameEncoder`** — stateless frame→bytes serializer (`static encode`). The presence
  of a `maskKey` argument (not `frame.masked`) decides whether the wire form is masked.
- **`WsMessageAssembler`** — stitches a data frame + its continuation frames into one
  `WsMessage`, enforcing a per-message byte ceiling. Passes control frames through.
- **`WsControlFrames`** — stateless builders/parsers for ping/pong/close frames and the
  close-handshake / auto-pong policies.
- **`WsProtocol`** — the read-side **engine**: feed it one `WsFrame`, it drives the
  assembler + control logic and returns a `WsReadAction`. This is what the façade reads.

`WsFrame`, `WsMessage`, `WsReadAction`, `WsOpcode`, `WsCloseReason` are the value/enum
types these pass around (covered by their own class/package skills — not repeated here).

## Cross-class call sequence (read side)

1. `WsFrameDecoder.feed(bytes, n)` — append bytes; decode as many whole frames as buffered.
2. drain `while (dec.hasFrame()) { WsFrame f = dec.nextFrame(); ... }`.
3. `WsProtocol.onFrame(f)` for each frame → a `WsReadAction`. Internally:
   - data frame (text/binary/continuation) → `WsMessageAssembler.accept` → `MESSAGE` when
     a whole message completes, else `NONE`;
   - `PING` → auto-pong `SEND_FRAME` (default) or surfaced `PING` (auto-pong off);
   - `PONG` → `NONE` (swallowed);
   - `CLOSE` → `CLOSED` (parsed reason + the reciprocal close to write, unless the local
     side already closed).
4. Act on the action's `kind`: deliver `takeMessage()`, write `takeFrame()`, or tear down.

You normally drive `WsProtocol`, not the assembler/control classes directly — it owns a
`WsMessageAssembler` and calls `WsControlFrames` for you. Use the assembler/control
classes alone only when you want reassembly or frame-building without the full engine.

## Ownership / lifecycle (the part that bites)

Everything here is heap-allocated with explicit transfer (`#`). Cross-boundary rules:

- `WsFrameDecoder.forServer()/forClient()` return `#WsFrameDecoder` — you own it.
- `WsFrameDecoder.nextFrame()` returns `#WsFrame`: ownership **transfers to you**, the
  queue slot is nulled. You then pass that frame **borrowed** into `WsProtocol.onFrame`
  (the engine reads but does not take it) — so you still own `f` and must let it drop, or
  the frame leaks. `onFrame` makes its own copies of any frame it needs to keep.
- `WsProtocol.onFrame` returns `#WsReadAction` — you own it. Pull payloads out with
  `takeMessage()` / `takeFrame()`, which **detach** (null the field) and hand you the
  `#WsMessage` / `#WsFrame`; the action no longer aliases them. (`getMessage()`/`getFrame()`
  borrow without detaching — use those only to inspect.)
- `WsControlFrames.ping(#int8[])` / `pong(#int8[])` **take** their payload array (no copy).
  `pongFor(ping)`, `close(code, reason)`, `parseClose(frame)` **copy** / read-only their
  inputs — the caller keeps the source frame/String.
- `WsFrameEncoder.encode(frame, maskKey)` borrows `frame` and `maskKey`, returns a fresh
  `#int8[]` you own. `maskKey` is `null` (server, unmasked) or exactly 4 bytes (client).

`WsProtocol` after a peer `CLOSE` is **terminal**: every further `onFrame` returns a
reply-less `CLOSED` carrying the retained reason. There is no `reset()` — make a new engine
for a new connection. No drop-on-scope `close()` method exists; instances are reclaimed when
their owner drops.

## What this does NOT do

- **No socket I/O.** Nothing here reads or writes a socket; `feed`/`encode` move bytes you
  supply. The `WebSocket` façade does the actual transport.
- **No handshake.** The HTTP upgrade is `WsServerHandshake`/`WsClientHandshake`, not here.
- **No masking policy.** `WsFrameEncoder` masks iff you pass a key; it does not generate
  keys or decide direction. `frame.masked` is ignored on encode.
- **No UTF-8 validation** of text payloads or close reasons (length-only) — that is the
  façade's job (maps to a `1007` close).
- **No outbound fragmentation.** The assembler reassembles inbound fragments; it does not
  split outbound messages.
- `WsProtocol` does **not** track outstanding pings, so inbound `PONG` is always swallowed.

## Worked example — server read loop core (decode → act)

```cajeta
import cajeta.io.net.ws.WsFrameDecoder;
import cajeta.io.net.ws.WsFrame;
import cajeta.io.net.ws.WsProtocol;
import cajeta.io.net.ws.WsReadAction;
import cajeta.io.net.ws.WsMessage;
import cajeta.io.net.ws.WsFrameEncoder;

// One decoder + one engine per connection (server side: client frames are masked).
WsFrameDecoder dec = WsFrameDecoder.forServer();
WsProtocol proto = WsProtocol.create();   // auto-pong on, default 64 MiB message cap

// `chunk`/`n` are bytes the transport just read off the socket.
dec.feed(chunk, n);
while (dec.hasFrame()) {
    WsFrame f = dec.nextFrame();           // we now own `f`
    WsReadAction act = proto.onFrame(f);   // `f` is borrowed; engine copies what it keeps

    int32 kind = act.getKind();
    if (kind == WsReadAction.MESSAGE) {
        WsMessage m = act.takeMessage();   // detach: we own the message
        // ... deliver m.getPayload() to the application (m.isText() / m.isBinary()) ...
    } else if (kind == WsReadAction.SEND_FRAME) {
        WsFrame reply = act.takeFrame();   // auto-pong or reciprocal close: we own it
        int8[] noKey = null;               // server MUST NOT mask
        int8[] wire = WsFrameEncoder.encode(reply, noKey);
        // ... write `wire` to the socket ...
    } else if (kind == WsReadAction.CLOSED) {
        WsFrame reply = act.takeFrame();   // null if local side already closed
        if (reply != null) {
            int8[] noKey = null;
            int8[] wire = WsFrameEncoder.encode(reply, noKey);
            // ... write the reciprocal close, then tear down ...
        }
        // act.getCloseReason() carries the peer's { code, reason }
    }
    // WsReadAction.NONE: buffered fragment or swallowed pong — just read on.
}
```

A client read loop is identical but uses `WsFrameDecoder.forClient()` and passes a fresh
4-byte `maskKey` to `encode` (clients MUST mask). To send a heartbeat, build the frame
with `WsControlFrames.ping(#payload)` and encode it the same way.

## Errors

All faults are exceptions propagated unchanged through the pipeline:

- `ProtocolViolationException` — masking-direction mismatch, reserved opcode, set RSV bit,
  fragmented/oversize control frame, bad 64-bit length, over-cap frame (decoder); an
  out-of-sequence fragment or malformed close payload (assembler / `parseClose`). It cites
  the stream byte offset where the decoder raised it.
- `MessageTooLargeException` — a reassembled message exceeds the assembler's cap (RFC 6455
  §7.4.1 close code `1009`). Set the cap with `WsProtocol.withMaxMessageLength(...)` (which
  delegates to the assembler) or `WsFrameDecoder.withMaxFrameLength(...)` for the per-frame
  floor.

The façade maps these to the right "Fail the Connection" close code; this layer just raises.
