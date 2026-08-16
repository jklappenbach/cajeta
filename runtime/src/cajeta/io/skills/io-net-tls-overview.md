---
id: io-net-tls-overview
applies-to: [cajeta/io/net/tls]
title: cajeta.io.net.tls — TLS streams, server termination, and the engine pump
description: Map of the TLS package — pick TlsStream for an encrypted socket, TlsListener for server termination, TlsConnection only when you drive the memory-BIO pump yourself.
---

# cajeta.io.net.tls

TLS 1.3 layered onto the `cajeta.io.net` socket types (plan NET-5). The package is a
**memory-BIO engine** ([`TlsConnection`](../net/tls/TlsConnection.cajeta)) wired to two
in-memory ciphertext queues, plus two ready-to-use wrappers that pump that engine over a
real socket. The engine never blocks — every handshake/read/write parks the *fiber* on
the reactor, never the carrier thread.

## Task → entry point

| You want to… | Use |
| --- | --- |
| Connect outbound over `https://`/`wss://` against the public PKI | `TlsStream.clientSystemTrust(#sock, host, hostLen)` then `handshake()` |
| Connect outbound trusting an explicit PEM anchor (private CA, self-signed loopback) | `TlsStream.client(#sock, host, hostLen, trustPem, trustLen)` then `handshake()` |
| Terminate TLS as a server (bind, accept, handshake done for you) | `TlsListener.bind(addr, cert, …, key, …)` then `accept()` / `acceptAsync()` |
| Wrap one already-accepted socket as a server stream | `TlsStream.server(#sock, cert, …, key, …)` then `handshake()` |
| Drive the handshake/record pump over a transport that is **not** a `TcpStream` | `TlsConnection` directly (see "the pump" below) |
| Send raw plaintext / read decrypted bytes through a buffered codec | a `TlsStream` *is* a [`ByteChannel`](../net/ByteChannel.cajeta) — hand it to [`AsyncReader`/`AsyncWriter`](io-net-async-io.md) unchanged |

Negative routing — this package does **not**: open the TCP connection (do that with
[`TcpStream.connect`](../net/TcpStream.cajeta) / [`TcpListener.bind`](../net/TcpListener.cajeta)
first, then wrap); parse HTTP/WS (the codecs above the `ByteChannel` do); manage cert
files (you pass PEM **bytes + length**, not paths); or expose a record read deadline
(`readWithin`'s `timeoutMs` is advisory and delegates to the unbounded read).

## Inventory

**Entry-point types** (you instantiate / call these):
- **`TlsStream`** — a [`ByteChannel`](../net/ByteChannel.cajeta) over a `TcpStream`. The
  workhorse for both client and server once you have a connected socket. Constructed only
  via its static factories (`client` / `clientSystemTrust` / `server`); the constructor is
  private.
- **`TlsListener`** — server-side termination over a `TcpListener`: bind once, then each
  `accept` returns a `TlsStream` whose handshake has already completed.

**Support type** (the engine — usually you do not touch it directly):
- **`TlsConnection`** — the per-connection TLS engine over the `__cajeta_tls_*` native
  intrinsics. Owns **no socket**; holds the `CERT_*` and `OK`/`WANT_IO`/`CLOSED`/`FAILED`
  status constants. `TlsStream` is the thing that pumps it over a socket.

**Exceptions:**
- **`TlsException`** (extends [`NetException`](../net/NetException.cajeta)) — handshake/
  protocol failure or a peer closing mid-handshake.
- **`CertificateInvalidException`** (extends `TlsException`) — peer cert verification
  failed; `.getReason()` returns one of the `TlsConnection.CERT_*` ordinals
  (`CERT_EXPIRED`, `CERT_HOSTNAME`, `CERT_UNTRUSTED`, `CERT_OTHER`).

## Collaboration

`TlsListener.accept()` → calls `TlsStream.server(#sock, …)` then `handshake()` for you,
returning a handshake-complete `#TlsStream`. `TlsStream` constructs and owns a
`TlsConnection` internally and runs its pump: after a step/`write` it `pull`s queued
ciphertext to the socket (`flush`), and on `WANT_IO` it reads socket ciphertext and
`feed`s it back (`fill`). You normally interact only with `TlsStream`/`TlsListener`;
`TlsConnection` surfaces only when you re-pump it over a non-socket transport.

## Ownership & lifecycle (the boundary rules)

- **`TlsStream` consumes its socket (`#`).** `TlsStream.client/clientSystemTrust/server`
  declare `#TcpStream stream`, so you must write `#sock` at the call site; do not use or
  close the raw socket afterwards.
- **`TlsListener.bind` takes no socket at all.** Its signature is
  `bind(addr, cert, certLen, key, keyLen)` — `addr` is **borrowed**, and the factory
  binds and owns its own `TcpListener` internally. There is no listener-taking overload
  and no raw listening socket for you to hand over or avoid touching.
- **`TlsStream` owns its stream + engine.** Dropping a `TlsStream` closes the socket and
  frees the TLS handles. `close()` sends `close_notify` then closes the socket (idempotent).
- **`TlsListener`'s cert/key are BORROWED, not `#`.** The listener stores the `int8[]`
  cert/key (and ALPN list) by reference — the **caller must keep them alive for the
  listener's whole lifetime** (a loopback test commonly reuses the server cert as the
  client's trust anchor). Dropping the listener closes only the listening socket.
- **PEM and host are bytes + explicit length**, e.g. `host, hostLen` / `cert, certLen` —
  there are no String/path overloads.
- **Factories return owned `#`** (`#TlsStream`, `#TlsListener`): the receiver owns and
  drops them.
- **Single-fiber** ownership, like the rest of `cajeta.io.net` — one fiber drives one
  stream; no internal locking. (Server fan-out spawns a fiber per accepted stream.)

## Worked example (test-backed: TlsLoopbackTest)

Client and server each on their own fiber complete a real TLS 1.3 handshake over a
loopback socket and echo one record. ALPN, if wanted, is set **before** `handshake()`.

```cajeta
import cajeta.io.net.IpAddress;
import cajeta.io.net.SocketAddress;
import cajeta.io.net.TcpStream;
import cajeta.io.net.TcpListener;
import cajeta.io.net.tls.TlsStream;
import cajeta.concurrent.Tasks;

// server fiber: TLS-wrap the accepted socket, handshake, echo one record
public static async int32 runServer(#TcpStream sock, int8[] cert, int32 certLen,
                                    int8[] key, int32 keyLen) {
    TlsStream st #= TlsStream.server(#sock, cert, certLen, key, keyLen); // consumes sock
    st.handshake();                                  // throws Tls/CertificateInvalid on failure
    int8[] inbuf = heap int8[256];
    int32 n = st.read(inbuf, 256);                   // 0 == clean peer close_notify (EOF)
    st.write(inbuf, n);
    return n;                                         // st drops here: socket closed, handles freed
}

// driver (cert/key are PEM bytes; host is "localhost" as int8[9])
TcpListener listener #= TcpListener.bind(bindAddr);
int32 port = listener.boundPort();                   // resolves a 0-bind to the OS port
TcpStream client #= TcpStream.connect(#connAddr);
TcpStream server #= listener.accept();
Task<int32> t = spawn runServer(#server, cert, cl, key, kl);

TlsStream ct #= TlsStream.client(#client, host, 9, cert, cl); // verifying client, anchor=cert
ct.handshake();
ct.write(ping, 4);
int8[] echo = heap int8[256];
int32 got = ct.read(echo, 256);
int32 sret = await t;
listener.close();
```

For the **server-listener** path the body shrinks to `TlsListener.bind(addr, cert, …,
key, …)` then `listener.accept()` (or `acceptAsync()` when the acceptor shares a
scheduler with the connecting peer) — accept + handshake in one call, returning a ready
`TlsStream`. Call `supportAlpn(protos, len)` on the listener before accepting to negotiate
ALPN.

## The pump (only when using `TlsConnection` directly)

If your transport is not a `TcpStream`, you run the same loop `TlsStream` runs:

1. `int32 step = conn.handshakeStep();` — `step == TlsConnection.OK` (`0`) means
   **handshake complete**; `WANT_IO` (`-1`) means more ciphertext must flow; `FAILED`
   (`-3`) is fatal.
2. `conn.pull(buf, max)` queued ciphertext (loop while `> 0`) and send it to the peer.
3. on `WANT_IO`, read ciphertext from the peer and `conn.feed(buf, len)` it, then retry.

After the handshake, `read`/`write` use the same `WANT_IO` shuttle; `read` returns
`CLOSED` (`-2`) on the peer's `close_notify`. On `FAILED`, check `conn.verifyResult()`:
non-`CERT_OK` is a certificate fault (map to `CertificateInvalidException`). Configure
SNI / trust / verify-host / ALPN **before** the first `handshakeStep`. `conn` owns no
socket; `drop()` frees its handles (NULL-safe).

## Pointers

- The plaintext surface a `TlsStream` exposes: [`ByteChannel`](../net/ByteChannel.cajeta);
  the buffered streaming over it: [`AsyncReader`/`AsyncWriter`](io-net-async-io.md).
- The sockets you wrap: [`TcpStream` / `TcpListener`](../net/skills/io-net-tcp.md).
