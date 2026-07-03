# TlsConnection

`cajeta.io.net.tls.TlsConnection` — a single TLS connection's engine state,
the surface over the memory-BIO TLS intrinsics. It owns no socket: the
connection is the TLS engine wired to two in-memory byte queues, and all I/O
is the caller shuttling ciphertext between this object and the network. The
pump is the same loop for both peers: call `handshakeStep` (or `write` /
`read`), `pull` any queued ciphertext and send it to the peer, and on
`WANT_IO` `feed` ciphertext received from the peer and retry. Step results are
the static ordinals `OK` / `WANT_IO` / `CLOSED` / `FAILED`; post-handshake
verification verdicts are `CERT_OK` / `CERT_EXPIRED` / `CERT_HOSTNAME` /
`CERT_UNTRUSTED` / `CERT_OTHER`.

```cajeta
TlsConnection tls = TlsConnection.verifyingClient();
tls.useSystemTrust();
int32 step = tls.handshakeStep();     // OK, WANT_IO, or FAILED
int8[] out = heap int8[4096];
int32 n = tls.pull(out, 4096);        // ciphertext to send to the peer
tls.drop();
```

## Methods

| Signature | |
|---|---|
| `static #TlsConnection client()` ⚑ | A fresh client connection (its own context) |
| `static #TlsConnection verifyingClient()` ⚑ | A verifying client: the peer chain is required and validated, so the handshake fails (and `verifyResult` reports why) on a bad cert |
| `static #TlsConnection server(int8[] certPem, int32 certLen, int8[] keyPem, int32 keyLen)` ⚑ | A fresh server connection presenting `certPem` (chain) + `keyPem` |
| `void setSni(int8[] host, int32 len)` | Client SNI: the server name to request |
| `int32 addTrust(int8[] pem, int32 len)` | Add a trust-anchor (CA / root) from PEM bytes to this context's store |
| `int32 useSystemTrust()` | Trust the operating-system default CA store |
| `void setVerifyHost(int8[] host, int32 len)` | Require the peer cert to match `host` (SAN, CN fallback, wildcards) |
| `int32 verifyResult()` | Post-handshake verification verdict — one of the `CERT_*` ordinals |
| `void setAlpn(int8[] protos, int32 len)` | Offer an ALPN protocol list (wire format: 1-byte length + bytes each) |
| `int32 negotiatedAlpn(int8[] out, int32 max)` | Copy the negotiated ALPN protocol bytes into `out`; returns the length |
| `void setServerAlpn(int8[] protos, int32 len)` | Server-side: install the ALPN-select callback offering `protos` as the supported list |
| `int32 handshakeStep()` | One handshake step: `OK` (complete), `WANT_IO`, or `FAILED` |
| `int32 feed(int8[] buf, int32 len)` | Feed `len` ciphertext bytes received from the network |
| `int32 pull(int8[] out, int32 max)` | Pull queued ciphertext to send to the network into `out`; returns bytes copied |
| `int32 pending()` | Bytes of ciphertext currently queued to send |
| `int32 write(int8[] buf, int32 len)` | Encrypt + queue `len` plaintext bytes; returns bytes accepted, or `WANT_IO` |
| `int32 read(int8[] out, int32 max)` | Decrypt available app data into `out`; returns bytes, `WANT_IO`, or `CLOSED` |
| `int32 shutdown()` | Queue a clean `close_notify` |
| `void drop()` | Release the engine handles |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/io/net/tls/TlsConnection.cajeta`](../../../../../runtime/src/cajeta/io/net/tls/TlsConnection.cajeta)
- [TlsListener](TlsListener.md) — server-side termination built on this engine; [HttpClient](../http/HttpClient.md) — `https://` over it
