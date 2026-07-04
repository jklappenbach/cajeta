# Cajeta TLS tour

A real TLS handshake + echo over loopback (`127.0.0.1`, no external network)
through `cajeta.io.net.tls` — the OpenSSL-backed stdlib TLS surface:

- **`TlsListener.bind(addr, cert, key)`** — server-side termination. `bind`
  holds the certificate/key PEM bytes; `accept()` performs the TCP accept
  *and* the server half of the handshake, returning a ready `TlsStream`.
- **`TlsStream.client(#tcp, host, trust)`** — the verifying client. There is
  no skip-verify mode: for a self-signed cert you pass the server's own cert
  PEM as the client's **trust anchor**, and `host` (`"localhost"`, matching
  the cert's SAN) drives both SNI and hostname verification.
- The two blocking ends overlap via structured concurrency: the accept/echo
  side is `spawn`ed as an async fiber, the client handshakes on the current
  one, `await` joins them, and the non-async `main` enters fiber-land with
  `Tasks.runBlocking`.

```
samples/tour/tls/
├── README.md
├── cajeta.json                  ← build-tool manifest (network + filesystem)
├── run-tls.sh                   ← cert gen (openssl) + cajeta build + execute
└── src/tour/tls/TlsTour.cajeta  ← the demo (package tour.tls)
```

## Run it

The compiler must be built first (`cd <repo> && ./build.sh`). Then:

```sh
./run-tls.sh
```

The script generates an ephemeral self-signed EC P-256 certificate
(CN/SAN `localhost`, 1-day validity) into `build/certs/` with `openssl`,
then builds and runs the demo. No cert fixtures are checked in; nothing
leaves the machine. If `openssl` is unavailable (or the certs are missing)
the binary prints a skip notice and exits 0.

## Expected output

```
=== Cajeta TLS tour ===
  TlsListener/TlsStream: handshake + echo over 127.0.0.1,
  self-signed cert as the client's trust anchor.

-- certs: cert.pem 607 bytes, key.pem 241 bytes --

-- listener bound on 127.0.0.1:42577 (OS-assigned) --
-- handshake complete (TLS 1.2+; hostname 'localhost' verified) --
-- echo: sent 'ping' (4 bytes), got 4 bytes back, payload match = true --

=== tls tour complete: 4 self-checks passed ===
```

(The port is OS-assigned and the cert sizes vary run to run.)

The engine pins a TLS 1.2 minimum (loopback negotiates 1.3). See
`runtime/src/cajeta/io/net/tls/` for the API and
`runtime/native/cajeta_tls.c` for the memory-BIO OpenSSL engine.
