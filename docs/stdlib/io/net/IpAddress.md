# IpAddress

`cajeta.io.net.IpAddress` — an immutable IP address, IPv4 or IPv6, with text
`parse` and canonical `toString`. The address is held as a 16-byte octet array
in network byte order regardless of family (IPv4 uses the first four bytes).
`parse` auto-detects the family and is strict — a malformed octet or group, an
out-of-range value, a doubled `::`, or trailing junk raises a citing
`MalformedAddressException`. `toString` reproduces the canonical form: IPv4
dotted-quad, IPv6 per RFC 5952, so `parse(s).toString()` round-trips a
canonical input byte-identically.

```cajeta
IpAddress a = IpAddress.parse("192.168.1.10");
IpAddress b = IpAddress.fromV4(192, 168, 1, 10);
boolean same = a.equals(b);          // true
IpAddress lo = IpAddress.loopbackV4();
String text = lo.toString();         // "127.0.0.1"
```

## Methods

| Signature | |
|---|---|
| `IpAddress()` | The unspecified IPv4 `0.0.0.0` (all-zero octets, family `V4`) |
| `AddressFamily getFamily()` | This address's family (`V4` or `V6`) |
| `boolean isV4()` | True iff IPv4 |
| `boolean isV6()` | True iff IPv6 |
| `int8[] getOctets()` | The raw 16-byte octet buffer (network byte order) |
| `static #IpAddress loopbackV4()` | The IPv4 loopback `127.0.0.1` |
| `static #IpAddress loopbackV6()` | The IPv6 loopback `::1` |
| `static #IpAddress anyV4()` | The IPv4 wildcard `0.0.0.0` |
| `static #IpAddress anyV6()` | The IPv6 wildcard `::` |
| `static #IpAddress fromV4(int32 a0, int32 a1, int32 a2, int32 a3)` ⚑ | Build directly from four octet values (`a.b.c.d`) |
| `static #IpAddress fromOctets(int8[] src, int32 len)` | Build from a 4-byte (v4) or 16-byte (v6) octet buffer — the inverse of `getOctets` |
| `boolean equals(IpAddress other)` | Equal iff same family and the family-relevant octets match |
| `static #IpAddress parse(String input)` ⚑ | Parse dotted-quad or IPv6 text (incl. `::` compression and the embedded-IPv4 tail form) |
| `#String toString()` | Canonical text: IPv4 dotted-quad, IPv6 per RFC 5952 |

⚑ = `@EntryPoint`

## See also

- Tour: [NetDemo](../../../../samples/tour/src/main/cajeta/tour/net/NetDemo.cajeta)
- Source: [`runtime/src/cajeta/io/net/IpAddress.cajeta`](../../../../runtime/src/cajeta/io/net/IpAddress.cajeta)
- [SocketAddress](SocketAddress.md) — an `IpAddress` paired with a port
