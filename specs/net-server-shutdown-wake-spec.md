# net Server — shutdown wake + localAddress read-through — spec (draft)

Origin: docs-refactor 15.6 (unit-11 ServerDemo, 2026-07-03).

## 1. Definition

Two `cajeta.io.net` server gaps discovered while writing the tour's
ServerDemo:

1. **Shutdown never wakes the acceptor.** `Server.shutdown()` closes the
   listener, but a fiber parked in `TcpListener.acceptAsync()` never wakes,
   so `serve()`'s scope join hangs forever. The docs claim the close wakes
   the acceptor with `NetException` — the reactor must actually deliver it.
2. **`localAddress()` is a placeholder.** `TcpListener.localAddress()`
   returns `0.0.0.0:0` and `Server.localAddress()` silently reads through
   it; `boundPort()` is the only working query.

ServerDemo currently drives one accept iteration by hand and documents the
production shape until this lands.

## 2. Features

### 2.1 Close-wakes-acceptor
Closing the listener (via `shutdown()` or drop) completes every parked
`acceptAsync()` with `NetException` (the documented contract).
Use cases:
1. As a server author, when I call `server.shutdown()` from another fiber,
   then `serve()` returns after in-flight handlers finish — no hang.
2. As the tour's ServerDemo, when the demo ends, then the process exits 0
   without hand-rolled accept loops.

### 2.2 Real localAddress
`TcpListener.localAddress()` reports the bound address:port from the
socket (getsockname), and `Server.localAddress()` reads it through.
Use cases:
1. As a test, when I bind port 0 (ephemeral), then `localAddress()` tells
   me where to connect — today only `boundPort()` does.
2. As an operator, when the server logs its startup line, then the address
   is the real bind, not `0.0.0.0:0`.

## 3. Non-goals
Graceful-drain policy changes; multi-listener servers; TLS.

## 4. Systems
Reactor (`cajeta.io.net` runtime + native poller), `TcpListener`,
`Server`/`ServerBuilder`; ServerDemo un-hand-rolled as acceptance.
