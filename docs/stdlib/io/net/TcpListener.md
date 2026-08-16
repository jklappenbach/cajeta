# TcpListener

`cajeta.io.net.TcpListener` — a passive TCP listening socket: `bind` + `listen`
plus a blocking `accept` that returns one accepted connection as a
[TcpStream](TcpStream.md). `acceptAsync` is the fiber-parking twin: it parks
the calling fiber (not the carrier thread) on the listening socket until a
connection is pending. The listener exposes its descriptor as the public `fd`
field (`-1` once closed).

```cajeta
IpAddress lo #= IpAddress.loopbackV4();
SocketAddress bindAddr #= SocketAddress.of(#lo, 0);
TcpListener listener #= TcpListener.bind(bindAddr);
int32 port = listener.boundPort();   // kernel-assigned ephemeral port
TcpStream conn #= listener.accept();  // blocks until a client connects
conn.close();
listener.close();
```

## Methods

| Signature | |
|---|---|
| `static #TcpListener bind(SocketAddress addr)` ⚑ | Bind a fresh TCP listening socket to `addr` and start listening with the default backlog (128) |
| `#TcpStream accept()` | Block until a client connects, then return the accepted connection |
| `int32 acceptFd()` | The fd-level half of `accept`: block for one connection and return its raw descriptor (`>= 0`), or throw on failure |
| `#TcpStream acceptAsync()` | Async accept — park the fiber (not the carrier) until a connection is pending; errors raise the mapped `NetException` subtype |
| `int32 boundPort()` | The kernel-assigned local port (resolves the ephemeral port for a `:0` bind) |
| `#SocketAddress localAddress()` | The bound local endpoint; currently a placeholder that returns the IPv4 wildcard `0.0.0.0:0` — use `boundPort` for the real port |
| `void close()` | Close the listening socket; idempotent |

⚑ = `@EntryPoint`

## See also

- Tour: [NetDemo](../../../../samples/tour/src/main/cajeta/tour/net/NetDemo.cajeta)
- Source: [`runtime/src/cajeta/io/net/TcpListener.cajeta`](../../../../runtime/src/cajeta/io/net/TcpListener.cajeta)
- [TcpStream](TcpStream.md) — the accepted connection; [Server](Server.md) — the accept-loop core built on this; [TlsListener](tls/TlsListener.md) — the TLS-terminating counterpart
