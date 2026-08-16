# TlsListener

`cajeta.io.net.tls.TlsListener` — server-side TLS termination over a
[TcpListener](../TcpListener.md). Binds a listening socket and presents a
fixed certificate + private key; each `accept` returns a `TlsStream` whose
handshake has already completed, so the caller works in plaintext from the
first byte. The cert/key (and the optional ALPN list) are borrowed — the
caller must keep them alive for the listener's lifetime. Dropping the
`TlsListener` closes the underlying listening socket.

```cajeta
int8[] cert #= File.readAllBytes("/etc/ssl/server-cert.pem");
int8[] key #= File.readAllBytes("/etc/ssl/server-key.pem");
SocketAddress addr #= SocketAddress.parse("127.0.0.1:0");
TlsListener listener #= TlsListener.bind(addr, cert, (int32) cert.count(),
                                        key, (int32) key.count());
TlsStream conn #= listener.accept();   // handshake already complete
listener.close();
```

## Methods

| Signature | |
|---|---|
| `static #TlsListener bind(SocketAddress addr, int8[] cert, int32 certLen, int8[] key, int32 keyLen)` ⚑ | Bind a TLS-terminating listener to `addr`, presenting `cert` (chain) + `key` (PEM bytes); a `0` port binds to an OS-assigned one |
| `void supportAlpn(int8[] protos, int32 len)` | Offer `protos` (ALPN wire format) on every accepted connection; optional |
| `int32 boundPort()` | The bound local port (resolves a `0`-bind to the OS-assigned port) |
| `#TlsStream accept()` | Accept the next connection and drive the server TLS handshake to completion |
| `#TlsStream acceptAsync()` | The fiber-parking twin of `accept`, over [`TcpListener.acceptAsync`](../TcpListener.md) |
| `void close()` | Stop listening (closes the underlying socket) |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/io/net/tls/TlsListener.cajeta`](../../../../../runtime/src/cajeta/io/net/tls/TlsListener.cajeta)
- [TlsConnection](TlsConnection.md) — the engine underneath; [TcpListener](../TcpListener.md) — the plaintext counterpart; [HttpServer](../http/HttpServer.md) — HTTPS serving over accepted streams
