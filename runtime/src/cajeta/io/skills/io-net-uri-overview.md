---
id: io-net-uri-overview
applies-to: [cajeta/io/net/uri]
title: URI package — parse, build, percent-encode, query-params (RFC 3986)
description: Map of cajeta.io.net.uri — immutable Uri.parse/builder/resolve, per-component PercentCodec, ordered QueryParams multi-map; pure String logic.
---

# URI package (`cajeta.io.net.uri`)

The **addressing layer** of `cajeta.io.net`: turn URI *strings* into structured
components and back. Everything here is **pure `cajeta.lang.String` logic** — no sockets,
no I/O, no native intrinsics. If you need to *connect* to a URL you are in the wrong
package (see TCP/TLS/HTTP); this package only decomposes, validates, encodes, and
recomposes the text.

## Task → entry point

| Want to… | Use |
| --- | --- |
| Split a URL into scheme/host/port/path/query/fragment | `Uri.parse(str)` |
| Build a URL from parts | `Uri.builder()….build()` |
| Resolve a relative reference against a base (e.g. a redirect `Location`) | `Uri.resolve(base, ref)` |
| Decode/iterate `?k=v&k=v2` pairs | `u.queryParams()` or `QueryParams.parse(rawQuery)` |
| Percent-encode one untrusted value for a component | `Uri.percentEncode(raw, UriComponent.X)` |
| Percent-decode `%XX` escapes | `Uri.percentDecode(s)` |
| Recompose a URI / query string to text | `Uri.toString()` / `QueryParams.toString()` |

Negative — **not** here: DNS resolution, opening connections, IPv4/IPv6 numeric parsing
(host is kept as text), and `+`-for-space handling *in the generic codec* (`PercentCodec`
leaves `+` literal — only `QueryParams` form-mode swaps it).

## Inventory

**Entry-point types** (you call/instantiate these):
- `Uri` — immutable parsed URI; `static parse`, `static resolve`, `static builder`,
  `static percentEncode/percentDecode`, plus `getScheme/getHost/getPort/getPath/getQuery/
  getFragment`, `queryParams()`, `toString()`.
- `UriBuilder` — fluent builder (`scheme/userinfo/host/port/path/query/fragment` →
  `build()`); obtain via `Uri.builder()`, never `heap UriBuilder()` directly.
- `PercentCodec` — `static encode(input, comp)` / `static decode(input)`. `Uri.percent*`
  are thin facades over it; prefer the `Uri` facades from caller code.
- `QueryParams` — ordered, duplicate-preserving `(key,value)` multi-map.

**Support types** (values/enums/exceptions — do not instantiate as an access point):
- `UriComponent` — enum selecting the per-component safe set: `PATH, SEGMENT, QUERY,
  QUERY_PARAM, FRAGMENT, USERINFO, HOST`. Ordinals are stable (PercentCodec switches on
  them).
- `MalformedUriException extends cajeta.io.net.NetException` — carries `position` (0-based
  byte offset, `-1` if not positional) and inherited `kind = 12` (KIND_INVALID).

## Collaboration

`Uri.parse` keeps components **raw / still-percent-encoded** and does **not** decode —
`getQuery()` returns the raw slice. `u.queryParams()` is the bridge: it feeds that raw
slice to `QueryParams.parse`, which calls `PercentCodec.decode` per key/value.
`QueryParams.toString` and `PercentCodec.encode` pick their safe set from `UriComponent`
(query params always use `QUERY_PARAM`). Layering: `UriComponent`/`PercentCodec` (codec)
← `QueryParams` (multi-map) and `Uri` (parse/build/resolve) on top.

## Cross-cutting invariants (package-specific)

- **Immutability.** A parsed/built `Uri` is read-only; mutate by building a new one. The
  seven components are each backed by a `String` plus a `has*` presence flag, because RFC
  3986 distinguishes *absent* from *present-but-empty* (`http:///x` vs `http:/x`).
- **No decode at parse time.** Components come back exactly as written (scheme is the one
  exception — lowercased per §3.1). Decoding is the caller's explicit step.
- **Default port.** `getPort()` returns the explicit port, else the scheme default
  (`http`→80, `https`→443, `ws`→80, `wss`→443, `ftp`→21), else `-1`. Check
  `u.hasExplicitPort` to tell a real `:80` from a default; `toString()` serializes a port
  only when explicit (so `http://h/` round-trips, not `http://h:80/`).
- **Errors are exceptions.** Bad input throws `MalformedUriException` (a `NetException`,
  itself `RecoverableException`): empty/null input, illegal scheme char, unterminated
  IPv6 `[`, non-numeric/out-of-range port, truncated/non-hex `%XX`. Catch it (or the
  `NetException` root) or declare it in `throws`. `getFirst` on a missing key returns
  `null`; `getAll` on a missing key returns a **zero-length** array (not null).
- **Ownership.** Static factories return `#`-owned values the caller owns and must drop:
  `parse`, `resolve`, `build`, `queryParams`, `percentEncode/Decode`, `QueryParams.parse/
  parseStrict/toString`. `QueryParams.keyAt/valueAt/getFirst/getAll` hand back **fresh
  copies** (view-mode Strings over the stored bytes), never aliases of internal storage —
  safe to keep after the map drops. `UriBuilder.build()` deep-copies, so the builder can
  be discarded or reused independently of the result.
- **`+`/space.** `QueryParams.parse` is **form-urlencoded** (`+`↔space); use
  `QueryParams.parseStrict` to keep `+` literal for byte-faithful round-trips.

## Example

```cajeta
import cajeta.lang.String;
import cajeta.io.net.uri.Uri;
import cajeta.io.net.uri.QueryParams;
import cajeta.io.net.uri.UriComponent;
import cajeta.io.net.uri.MalformedUriException;

// Parse, read components, decode the query (raw until you ask).
Uri u = Uri.parse("https://h.test/a/b?x=1&y=2&x=3");
String host = u.getHost();          // "h.test"
int32 port = u.getPort();           // 443 (scheme default; hasExplicitPort == false)
QueryParams q = u.queryParams();    // form-decoded multi-map
String first = q.getFirst("x");     // "1" (first wins); null if absent
String[] xs = q.getAll("x");        // ["1","3"] in order; length 0 if absent

// Build, encoding one untrusted value for its component.
String safe = Uri.percentEncode("a&b=c", UriComponent.QUERY_PARAM);  // "a%26b%3Dc"
Uri built = Uri.builder()
    .scheme("https").host("h.test").port(8443)
    .path("/a/b").query("x=1").fragment("frag")
    .build();
String s = built.toString();        // "https://h.test:8443/a/b?x=1#frag"
```

## Pointers

Component/method depth lives in the class/method skills: `QueryParams`,
`PercentCodec`/`UriComponent` safe-set details, and `Uri.parse`/`resolve` edge cases (IPv6
brackets, `remove_dot_segments`, the §5.4 golden table). See the `cajeta.io.net` library
overview for where addressing sits relative to sockets, HTTP, and WebSocket.
