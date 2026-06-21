---
id: io-net-http-client
applies-to: [cajeta/io/net/http/HttpClient]
title: HttpClient — one-shot HTTP/1.1 client (http:// and https://)
description: Single-exchange HTTP/1.1 client; get(url) follows redirects, send(uri,req) is one request; https verifies against OS CA store unless trustAnchor pins a PEM.
---

# HttpClient

The **entry point for making an HTTP/1.1 request**. Give it a URL (or a `Uri` +
`HttpRequest`) and it does the whole job: resolve the host, connect a
fiber-parking `TcpStream`, wrap it in a verifying `TlsStream` for `https://`,
serialize the request, and parse the full response (head + buffered body). The
`http://` vs `https://` choice is made *inside* `HttpClient` from the URI scheme —
you never pick a transport yourself. Lives in `cajeta/io/net/http`.

**Access point:** yes — construct it directly with `heap HttpClient()`. The layers
below (`Dns`, `TcpStream`, `TlsStream`, `HttpParser`) are composed for you; you do
not touch them for a normal request.

## Minimal usage

```cajeta
import cajeta.io.net.http.HttpClient;
import cajeta.io.net.http.HttpResponse;

HttpClient client = heap HttpClient();
HttpResponse resp = client.get("https://example.test/path");   // owned (#)
if (resp.statusCode() == 200) {
    int8[] body = resp.body;                 // borrowed view into resp
    int32 len  = resp.bodyLength();
    // ... use body[0..len) ...
}
```

For a non-GET method or a request body, build the request and use `send`:

```cajeta
import cajeta.io.net.http.HttpRequest;
import cajeta.io.net.uri.Uri;

Uri uri = Uri.parse("http://127.0.0.1:8080/submit");
HttpRequest req = HttpRequest.fromUri("POST", uri);
req.body(payload, payload.count());
HttpResponse resp = client.send(uri, req);   // exactly ONE exchange, no redirects
```

## Construction & ownership

- `HttpClient()` — trusts the **OS CA store** for `https://`
  (`TlsStream.clientSystemTrust`). The default, real-world public-PKI path.
- `HttpClient trustAnchor(int8[] pem, int32 len)` — pin an explicit CA/self-signed
  anchor (PEM bytes) instead of the OS store, switching `https://` to
  `TlsStream.client`. Keeps a **fresh owned copy** of `pem[0..len)` (your array is
  not consumed). **Fluent** — returns `this`, so chain it:
  `heap HttpClient().trustAnchor(pem, len)`.

## Methods that matter

- `#HttpResponse get(String url)` — parse `url`, build a `GET` (with `Host`), send,
  and **follow up to `MAX_REDIRECTS` (10) 3xx redirects** (301/302/303/307/308),
  rebasing each `Location` (absolute, or origin-relative `/path`). On hitting the
  hop cap, or a 3xx with no `Location`, returns the last response **as-is** — so a
  returned 3xx status is the signal you ran out of hops. Returns an **owned**
  `HttpResponse` (caller frees on drop).
- `#HttpResponse send(Uri uri, HttpRequest req)` — **one** request/response, **no
  redirect following**. Fills a missing `Host` from `uri`, stamps
  `Connection: close`, picks port 80/443 from the scheme when `uri` has none.
  Returns an **owned** `HttpResponse`.
- `MAX_REDIRECTS` — `public static final int32 = 10`.

The returned `HttpResponse` owns its `body` (`int8[]`) and `headers`; read via
`statusCode()`, `bodyLength()`, `getHeaders()` / the public `status`, `body`,
`headers` fields. `body` is a borrowed view valid for the response's lifetime —
copy it to outlive the response. See the `cajeta/io/net/http/HttpResponse` skill.

## Lifecycle & concurrency

- **No `close()` on the client.** `HttpClient` is a plain heap value; it drops with
  scope. Each call **opens and closes its own socket** internally (it stamps
  `Connection: close` — one exchange per socket), so there are no transport handles
  for you to manage or leak.
- **Reusable.** Call `get`/`send` as many times as you like on one instance; each
  call is independent and dials a fresh connection. `trustAnchor` settings persist
  across calls.
- **Fiber-parking.** Connect/read/write park the calling fiber (via `TcpStream`
  async ops), so this must run on a cajeta fiber/task scheduler (e.g. inside
  `Tasks.runBlocking` / a `spawn`ed task), not bare native threads.

## Errors raised

Failures propagate as exceptions (not sentinels): `NetException` and subtypes
(`cajeta/io/net`) for DNS resolution and connect failures; TLS handshake/trust
failures (`cajeta/io/net/tls`, e.g. `CertificateInvalidException`); and
`HttpException` subtypes (`cajeta/io/net/http`, e.g. `UnexpectedEofException`,
`MalformedMessageException`) when the peer's response is truncated or malformed.

## What it does NOT do (NET-8.1 scope — don't hunt for these)

- **No connection pooling / keep-alive reuse.** Every call dials a new socket and
  sends `Connection: close`. (Pooling is a separate later layer.)
- **No streaming bodies.** The response body is **fully buffered in memory** before
  `send`/`get` returns; there is no incremental body stream on the client.
- **No per-request deadline/timeout, no `getJson`/`downloadTo` convenience.**
- **No happy-eyeballs / dual-stack fallback.** v1 `connectAny` resolves **IPv4
  only** (`ResolveFamily.V4_ONLY`) and connects to the first endpoint — an
  IPv6-only host will not be reached. Redirect following is the one higher-level
  behavior `get` *does* provide; `send` does not redirect.
