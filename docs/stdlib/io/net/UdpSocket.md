# UdpSocket

`cajeta.io.net.UdpSocket` — a UDP datagram socket, the connectionless
transport peer of [TcpStream](TcpStream.md) / [TcpListener](TcpListener.md).
`bind` creates the socket (bind to `0.0.0.0:0` for a kernel-assigned ephemeral
port); `sendTo` / `recvFrom` are the unconnected datagram path, with
`recvFrom` returning a `RecvResult` carrying both the byte count and the
sender's address; `connect` sets a default peer (no handshake) enabling the
address-less `send` / `recv` forms. The destructor closes the socket if
`close()` wasn't called.

```cajeta
SocketAddress local = SocketAddress.parse("0.0.0.0:0");
UdpSocket sock = UdpSocket.bind(local);
SocketAddress dest = SocketAddress.parse("127.0.0.1:9999");
int8[] payload = heap int8[3];
sock.sendTo(payload, 0, 3, dest);
RecvResult in = sock.recvFrom(payload, 0, 3);
sock.close();
```

## Methods

| Signature | |
|---|---|
| `static #UdpSocket bind(SocketAddress local)` ⚑ | Open a UDP socket and bind it to `local` |
| `int32 sendTo(int8[] data, int32 offset, int32 length, SocketAddress dest)` | Send one datagram to `dest`; returns the byte count sent |
| `#RecvResult recvFrom(int8[] dst, int32 offset, int32 capacity)` | Receive one datagram into `dst[offset..)`; the result reports count and sender |
| `void connect(SocketAddress peer)` | Set a default peer (UDP `connect` performs no handshake) |
| `int32 send(int8[] data, int32 offset, int32 length)` | Send to the `connect`-stored peer |
| `int32 recv(int8[] dst, int32 offset, int32 capacity)` | Receive one datagram from the `connect`-stored peer |
| `#SocketAddress localAddress()` | The bound local endpoint (resolves the kernel-assigned ephemeral port after a `:0` bind) |
| `void setBroadcast(boolean on)` / `boolean getBroadcast()` | `SO_BROADCAST` |
| `void setTtl(int32 ttl)` / `int32 getTtl()` | Outbound TTL / hop limit |
| `void setRecvBufferSize(int32 bytes)` / `int32 getRecvBufferSize()` | Kernel receive buffer size (`SO_RCVBUF`) |
| `void setSendBufferSize(int32 bytes)` / `int32 getSendBufferSize()` | Kernel send buffer size (`SO_SNDBUF`) |
| `void close()` | Close the underlying socket; idempotent |
| `~UdpSocket()` | Destructor: closes the socket if `close` wasn't called |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/io/net/UdpSocket.cajeta`](../../../../runtime/src/cajeta/io/net/UdpSocket.cajeta)
- [SocketAddress](SocketAddress.md) — endpoints; [TcpStream](TcpStream.md) — the connection-oriented sibling
