# Dns

`cajeta.io.net.dns.Dns` — blocking host name resolution: turn a hostname (or a
numeric literal) into the list of [SocketAddress](../SocketAddress.md)
endpoints it maps to. This is the synchronous resolver — it drives libc
`getaddrinfo` through native intrinsics and parks the carrier thread in the
syscall while the lookup runs. A failed lookup (NULL host, failed resolution,
or zero addresses) raises a `NetException` carrying the resolve ordinal in its
`kind`.

```cajeta
SocketAddress[] addrs = Dns.resolve("localhost", 443);
SocketAddress[] v6 = Dns.resolve("localhost", 443, ResolveFamily.V6_ONLY);
int64 n = addrs.count();
```

## Methods

| Signature | |
|---|---|
| `static #SocketAddress[] resolve(String host)` ⚑ | Resolve `host` with port `0` and no family restriction |
| `static #SocketAddress[] resolve(String host, int32 port)` ⚑ | Resolve `host`, baking `port` into each address |
| `static #SocketAddress[] resolve(String host, int32 port, ResolveFamily family)` ⚑ | Resolve restricted to `family` (`V4_ONLY` / `V6_ONLY` / `BOTH`) |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/io/net/dns/Dns.cajeta`](../../../../../runtime/src/cajeta/io/net/dns/Dns.cajeta)
- [SocketAddress](../SocketAddress.md) — the resolved endpoints; [IpAddress](../IpAddress.md)
