# WebSocket

`cajeta.io.net.ws.WebSocket` — a live RFC 6455 WebSocket connection, the
application-facing façade: `send` text / `sendBinary` bytes, `receive` the
next reassembled `WsMessage`, and `close(code, reason)`. A connection is
opened for one role — `forServer` (inbound frames must be masked, outbound
unmasked) or `forClient` (the reverse) — over an already-upgraded connection's
`AsyncReader` / `AsyncWriter`, which the WebSocket borrows (the caller that
opened the socket owns and closes it after the close handshake). Concurrent
read + write from separate fibers is safe: every frame emission goes through a
fiber-aware write lock, while the read half is owned by a single reader fiber.
By default control `PING`s are auto-ponged by the read loop.

```cajeta
SocketAddress addr = SocketAddress.parse("127.0.0.1:9001");
TcpStream sock = TcpStream.connect(#addr);
AsyncReader reader = heap AsyncReader(sock);
AsyncWriter writer = heap AsyncWriter(sock);
WebSocket ws = WebSocket.forClient(reader, writer);
ws.send("hello");
WsMessage m = ws.receive();          // m.isText() / m.isBinary()
ws.close(WsCloseCode.NORMAL, "bye");
sock.close();
```

## Methods

| Signature | |
|---|---|
| `static #WebSocket forServer(AsyncReader reader, AsyncWriter writer)` ⚑ | A server-side WebSocket over an already-upgraded connection (expects masked client frames; sends unmasked) |
| `static #WebSocket forClient(AsyncReader reader, AsyncWriter writer)` ⚑ | A client-side WebSocket (expects unmasked server frames; masks outbound frames) |
| `WebSocket withMaxMessageLength(int64 maxBytes)` | Cap the per-message reassembled size in bytes |
| `WebSocket withAutoPong(boolean enabled)` | Enable (default) or disable auto-pong |
| `void send(String text)` | Send a text message — the string's UTF-8 bytes in a single unfragmented `TEXT` frame |
| `void sendBinary(int8[] data)` | Send a binary message — `data` in a single unfragmented `BINARY` frame |
| `#WsMessage receive()` | Receive the next whole text/binary message, parking the fiber across as many socket reads as its frames take |
| `void close(int32 code, String reason)` | Initiate the close handshake: send a `CLOSE` frame with `code` + UTF-8 `reason` |
| `boolean isCloseSent()` | `true` once the local side has sent a `CLOSE` |
| `boolean isPeerClosed()` | `true` once the peer's `CLOSE` has been observed by the read loop |
| `WsCloseReason peerCloseReason()` | The peer's parsed close `{ code, reason }` once observed, else `null` |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/io/net/ws/WebSocket.cajeta`](../../../../../runtime/src/cajeta/io/net/ws/WebSocket.cajeta)
- [TcpStream](../TcpStream.md) — the transport underneath; [HttpServer](../http/HttpServer.md) — where the upgrade handshake arrives
