# UriBuilder

`cajeta.io.net.uri.UriBuilder` — builder for [Uri](Uri.md), opened with
`Uri.builder()`. It accumulates components, setting the matching `has*`
presence flags as each is supplied, then `build()` materializes a fresh owned
`Uri` independent of the builder (which may be reused). Setting a host that
contains `:` is auto-flagged as an IPv6 literal so `toString` re-brackets it.

```cajeta
Uri u #= Uri.builder()
    .scheme("https")
    .host("example.test")
    .port(8443)
    .path("/index.html")
    .build();
String text #= u.toString();    // "https://example.test:8443/index.html"
```

## Methods

| Signature | |
|---|---|
| `UriBuilder()` ⚑ | A fresh builder with no components set |
| `UriBuilder scheme(String scheme)` | Set the (case-insensitive) scheme; stored lowercased, marks the scheme present |
| `UriBuilder userinfo(String userinfo)` | Set the userinfo (`"user:pass"`); marks userinfo present |
| `UriBuilder host(String host)` | Set the host (and thereby an authority); a host containing `:` is flagged IPv6 |
| `UriBuilder port(int32 port)` | Set an explicit port (serialized verbatim by `toString`) |
| `UriBuilder path(String path)` | Set the path component (always present; `""` is valid) |
| `UriBuilder query(String query)` | Set the raw (already-encoded) query; marks query present |
| `UriBuilder fragment(String fragment)` | Set the fragment; marks fragment present |
| `#Uri build()` | Materialize the assembled `Uri` as a fresh owned value |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/io/net/uri/Uri.cajeta`](../../../../../runtime/src/cajeta/io/net/uri/Uri.cajeta) (declared alongside `Uri`)
- [Uri](Uri.md) — the parsed counterpart (`Uri.parse`)
