# SocketAddress

`cajeta.io.net.SocketAddress` — an immutable socket address: an
[IpAddress](IpAddress.md) paired with a TCP/UDP port (`0..65535`). `parse`
accepts the two canonical `host:port` spellings — `"127.0.0.1:8080"` for IPv4
and `"[::1]:8080"` for IPv6 (bracketed literal) — and `toString` reproduces
exactly that form, so `parse(s).toString()` round-trips byte-identically. A
bare host with no `:port` is not accepted by `parse`; use `of` to build one
programmatically.

```cajeta
SocketAddress addr = SocketAddress.parse("127.0.0.1:8080");
int32 port = addr.getPort();         // 8080
IpAddress lo = IpAddress.loopbackV6();
SocketAddress v6 = SocketAddress.of(#lo, 443);
String text = v6.toString();         // "[::1]:443"
```

## Methods

| Signature | |
|---|---|
| `SocketAddress()` | The IPv4 wildcard `0.0.0.0:0` |
| `static #SocketAddress of(#IpAddress addr, int32 port)` ⚑ | Build from an existing `IpAddress` (ownership moves in) and a port |
| `IpAddress getIp()` | The IP endpoint |
| `int32 getPort()` | The port (`0..65535`) |
| `AddressFamily getFamily()` | The address family of the contained IP (`V4` or `V6`) |
| `boolean equals(SocketAddress other)` | Equal iff IPs and ports match |
| `static #SocketAddress parse(String input)` ⚑ | Parse `"ip:port"` / `"[v6]:port"` text |
| `#String toString()` | Canonical text: `ip:port` for IPv4, `[ip]:port` for IPv6 — the exact inverse of `parse` |

⚑ = `@EntryPoint`

## See also

- Tour: [NetDemo](../../../../samples/tour/src/main/cajeta/tour/net/NetDemo.cajeta)
- Source: [`runtime/src/cajeta/io/net/SocketAddress.cajeta`](../../../../runtime/src/cajeta/io/net/SocketAddress.cajeta)
- [IpAddress](IpAddress.md) — the address half; [TcpListener](TcpListener.md) / [TcpStream](TcpStream.md) / [UdpSocket](UdpSocket.md) — where addresses are used
