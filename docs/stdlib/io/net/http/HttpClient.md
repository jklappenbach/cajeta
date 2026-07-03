# HttpClient

`cajeta.io.net.http.HttpClient` — the HTTP/1.1 client. It drives one
request/response exchange over an async, TLS-or-plaintext byte channel:
resolve the host ([Dns](../dns/Dns.md)), connect a non-blocking
[TcpStream](../TcpStream.md), for `https://` wrap it in a verifying TLS stream
offering ALPN `http/1.1`, serialize the request, parse the response. By
default `https://` verifies the peer against the operating-system CA store; a
private CA or self-signed test server is supported by pinning an anchor with
`trustAnchor` (PEM bytes). v1 is the core single exchange: it stamps
`Connection: close` (one exchange per socket) and buffers the response body in
memory; `get` follows up to `MAX_REDIRECTS` (10) redirects.

```cajeta
HttpClient client = heap HttpClient();
HttpResponse resp = client.get("http://127.0.0.1:8080/health");
int32 status = resp.statusCode();
```

## Methods

| Signature | |
|---|---|
| `HttpClient()` ⚑ | A client trusting the OS CA store (the default public-PKI path) |
| `HttpClient trustAnchor(int8[] pem, int32 len)` | Pin an explicit trust anchor (PEM bytes) for `https://` verification, replacing the OS trust store |
| `#HttpResponse send(Uri uri, HttpRequest req)` ⚑ | Send `req` to `uri`'s authority and return the parsed response |
| `#HttpResponse get(String url)` ⚑ | `GET url` — parse, build the origin-form request (with `Host`), send, following up to `MAX_REDIRECTS` redirects |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/io/net/http/HttpClient.cajeta`](../../../../../runtime/src/cajeta/io/net/http/HttpClient.cajeta)
- [Uri](../uri/Uri.md) — request targets; [HttpServer](HttpServer.md) — the server side; [Dns](../dns/Dns.md) — name resolution underneath
