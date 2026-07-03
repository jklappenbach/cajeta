# Uri

`cajeta.io.net.uri.Uri` — an immutable, parsed URI per RFC 3986. `Uri.parse`
strictly decomposes a string into the seven generic-syntax components (scheme,
userinfo, host, port, path, query, fragment), rejecting malformed input with a
`MalformedUriException` that cites the offending byte offset. IPv6 literal
hosts are parsed in bracket form (brackets stripped from `getHost`), the
default port per scheme (`http` 80, `https` 443, `ws` 80, `wss` 443, `ftp` 21)
applies when the authority omits one, and authority-less forms
(`mailto:a@b.test`, a bare relative path) are handled. Each optional component
carries a `has*` presence flag, since RFC 3986 distinguishes absent from
present-but-empty; the stored scheme is lowercased, and no percent-decoding
happens at parse time.

```cajeta
Uri u = Uri.parse("https://example.test/a/b?x=1#top");
String host = u.getHost();       // "example.test"
int32 port = u.getPort();        // 443 (scheme default)
String query = u.getQuery();     // "x=1"
String text = u.toString();
```

## Methods

| Signature | |
|---|---|
| `Uri()` | Field-pinning no-arg constructor (all components empty/absent) |
| `String getScheme()` | Lowercased scheme, or `""` when absent |
| `String getHost()` | Host (IPv6 brackets stripped), or `""` when no authority |
| `int32 getPort()` | Effective port: explicit if present, else the scheme default, else `-1` |
| `String getUserinfo()` | Userinfo, or `""` when absent |
| `String getPath()` | Path (possibly `""`) |
| `String getQuery()` | Raw (still-percent-encoded) query, or `""` when absent |
| `#QueryParams queryParams()` | Parse the raw query into a decoded, order-preserving, duplicate-keeping multi-map |
| `String getFragment()` | Fragment, or `""` when absent |
| `static #Uri parse(String input)` ⚑ | Parse `input` per RFC 3986; throws `MalformedUriException` citing the byte offset |
| `static #String percentEncode(String raw, UriComponent comp)` | Percent-encode `raw` using component `comp`'s RFC 3986 safe set |
| `static #String percentDecode(String s)` | Decode `%XX` escapes — the inverse of `percentEncode` |
| `#String toString()` | Recompose back into a string per RFC 3986 §5.3 |
| `static #Uri resolve(Uri base, String reference)` | Resolve a reference against `base` per RFC 3986 §5.2 (strict) |
| `static #UriBuilder builder()` ⚑ | Fluent [builder](UriBuilder.md) for constructing a `Uri` programmatically |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/io/net/uri/Uri.cajeta`](../../../../../runtime/src/cajeta/io/net/uri/Uri.cajeta)
- [UriBuilder](UriBuilder.md) — programmatic construction; [HttpClient](../http/HttpClient.md) — where URIs are consumed
