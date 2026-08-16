# TcpStream

`cajeta.io.net.TcpStream` — a connected TCP stream: the blocking client/peer
socket, obtained from `connect` (client side) or
[`TcpListener.accept`](TcpListener.md) (server side). The async forms
(`connectAsync`, `readAsync`, `writeAsync`, `writeAllAsync`, `readWithin`) are
fiber-parking readiness loops — they never block the carrier thread. The
static `WOULD_BLOCK` sentinel (`-1`) is returned by the non-blocking I/O forms
when an op could not complete immediately. Socket options (`TCP_NODELAY`,
`SO_KEEPALIVE`, buffer sizes, `SO_LINGER`, TTL) are exposed as set/get pairs,
and the destructor closes the socket if `close()` wasn't called, so a dropped
`TcpStream` never leaks a descriptor.

```cajeta
SocketAddress addr #= SocketAddress.parse("127.0.0.1:7000");
TcpStream s #= TcpStream.connect(#addr);
int8[] buf = heap int8[4];
s.write(buf, (int64) 0, (int64) 4);
int64 got = s.read(buf, (int64) 0, (int64) 4);
s.close();
```

## Methods

| Signature | |
|---|---|
| `static #TcpStream connect(#SocketAddress addr)` ⚑ | Open a blocking TCP connection to `addr` and return the connected stream |
| `static #TcpStream connectAsync(#SocketAddress addr)` ⚑ | Connect without blocking the carrier (the fiber parks until the connect completes) |
| `int64 read(int8[] dst, int64 offset, int64 length)` | Read up to `length` bytes into `dst[offset..offset+length)`; returns the count, `0` at EOF |
| `int64 write(int8[] data, int64 offset, int64 length)` | Write up to `length` bytes from `data[offset..)`; returns the count written |
| `void writeAll(int8[] data, int64 offset, int64 length)` | Write all of `data[offset..offset+length)`, looping past short writes |
| `int64 readAsync(int8[] buf, int64 offset, int64 length)` | Async read — parks the fiber on readability instead of blocking |
| `int64 writeAsync(int8[] data, int64 offset, int64 length)` | Async write — parks the fiber on writability |
| `void writeAllAsync(int8[] data, int64 offset, int64 length)` | Async write-all: every byte lands, fiber-parking between short writes |
| `int64 readWithin(int8[] buf, int64 offset, int64 length, int32 timeoutMs)` | Deadline-bounded async read: like `readAsync` but bounds the total wait by `timeoutMs` |
| `void setNoDelay(boolean on)` / `boolean getNoDelay()` | `TCP_NODELAY` — disable Nagle's algorithm so small writes send immediately |
| `void setKeepAlive(boolean on)` / `boolean getKeepAlive()` | `SO_KEEPALIVE` |
| `void setRecvBufferSize(int32 bytes)` / `int32 getRecvBufferSize()` | Kernel receive buffer size (`SO_RCVBUF`) |
| `void setSendBufferSize(int32 bytes)` / `int32 getSendBufferSize()` | Kernel send buffer size (`SO_SNDBUF`) |
| `void setLinger(boolean on, int32 seconds)` / `boolean getLinger()` | `SO_LINGER` |
| `void setTtl(int32 ttl)` / `int32 getTtl()` | Outbound unicast TTL / hop limit (`0..255`) |
| `void shutdown(int32 how)` | Shut down part of the full-duplex connection without closing the descriptor |
| `void close()` | Close the underlying socket; idempotent |
| `~TcpStream()` | Destructor: closes the socket if `close` wasn't called |

⚑ = `@EntryPoint`

## See also

- Tour: [NetDemo](../../../../samples/tour/src/main/cajeta/tour/net/NetDemo.cajeta)
- Source: [`runtime/src/cajeta/io/net/TcpStream.cajeta`](../../../../runtime/src/cajeta/io/net/TcpStream.cajeta)
- [TcpListener](TcpListener.md) — where server-side streams come from; [UdpSocket](UdpSocket.md) — the datagram sibling
