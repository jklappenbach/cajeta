# `cajeta.io.net` — error taxonomy (the consolidated chart)

The complete networking error hierarchy, rooted at
[`cajeta.io.net.NetException`](../../../../runtime/src/cajeta/io/net/NetException.cajeta).
This is the networking peer of
[`cajeta.io.file`'s `Errors.md`](../file/Errors.md) — same shape,
same throws-clause discipline, a parallel-but-separate tree (file I/O
and network I/O share the fiber model but nothing else; see
[`Networking.md`](Networking.md) §Design principles).

> **Status — this is the NET-11.6 consolidation.** The whole
> networking taxonomy is gathered here: **every** phase's exceptions
> hang off the single [`NetException`](../../../../runtime/src/cajeta/io/net/NetException.cajeta)
> root, exactly as the `cajeta.io.file` subtypes hang off `IoException`.
> The chart is intentionally written **ahead** of the later phases so
> each phase's exception subtypes have a named home before they land —
> the [§Hierarchy](#hierarchy-by-phase) table flags each row as **built**
> (in the tree today) or **planned** (the phase that introduces it). As
> each phase ships, flip its rows from *planned* to *built*; the
> hierarchy shape does not change. The errno → exception **mapping
> chart** below is complete and authoritative today (Phase 1 / NET-1.8
> is fully built).

## The two-stage funnel

A platform error code reaches a typed cajeta exception in **two**
stages, by design:

```
  POSIX errno / Winsock WSAGetLastError      (platform-specific)
            │
            │  stage 1 — runtime/native/cajeta_net_socket.c
            │            cajeta_net_map_errno()  →  __cajeta_net_last_error()
            ▼
  cajeta_net_err ordinal                      (one value on all OSes)
            │
            │  stage 2 — cajeta.io.net.NetErrors.fromErrno(ordinal, detail)
            ▼
  NetException subtype                         (typed, catchable)
```

Stage 1 keeps every platform `#if` in C (where `EAGAIN`,
`WSAEWOULDBLOCK`, … are defined). Stage 2 keeps the class hierarchy in
cajeta. Neither side knows the other's spelling — only the shared
ordinal contract below. The ordinals are **append-only**: never
renumber an existing value (they are an ABI between the native shim
and the stdlib).

This funnel only governs the **socket-layer** failures (the ones that
originate as a platform errno). The higher-layer subtypes — DNS, TLS,
URI, HTTP, WebSocket — are raised directly by their own cajeta code
from a logical condition (a malformed URI, a chunk-encoding violation,
a bad WebSocket opcode), not via the errno map. They still root at the
same `NetException` so a single `catch (NetException e)` at a request
boundary catches *every* networking failure regardless of which layer
produced it.

## Hierarchy (by phase)

The single tree, grouped by the phase that introduces each subtype.
**Built** = a `.cajeta` file exists under
[`runtime/src/cajeta/io/net/`](../../../../runtime/src/cajeta/io/net/) today;
**planned** = the named phase adds it (its file extends `NetException`
when it lands — for the already-built URI exception, NET-6.5 reparents
it, see the note below).

```cajeta
public class NetException                    extends RecoverableException;  // root; carries `int32 kind`

// ── Phase 1 — socket layer (NET-1.8) ──────────────── all BUILT
public class WouldBlockException             extends NetException;  // kind 1   ¹
public class ConnectionRefusedException      extends NetException;  // kind 2
public class ConnectionResetException        extends NetException;  // kind 3
public class ConnectionAbortedException      extends NetException;  // kind 4
public class AddressInUseException           extends NetException;  // kind 5
public class AddressNotAvailableException    extends NetException;  // kind 6
public class HostUnreachableException        extends NetException;  // kind 7
public class NetworkUnreachableException     extends NetException;  // kind 8
public class BrokenPipeException             extends NetException;  // kind 9
public class TimedOutException               extends NetException;  // kind 10
public class MalformedAddressException       extends NetException;  // NET-1.2  (BUILT)

// ── Phase 2 — DNS (NET-2.5) ───────────────────────── BUILT
public class UnknownHostException            extends NetException;  // NONAME/NODATA — name not found; carries `int32 resolveErrno`, kind = KIND_OTHER (99)
public class ResolutionFailedException       extends NetException;  // getaddrinfo failed (non-NXDOMAIN); carries `int32 resolveErrno`, kind = KIND_OTHER (99)

// ── Phase 5 — TLS (NET-5.6) ───────────────────────── BUILT
public class TlsException                    extends NetException;       // TLS family root; non-certificate handshake / protocol faults; kind = KIND_OTHER (99)
public class CertificateInvalidException     extends TlsException;       // carries `int32 reason` (see below)

// ── Phase 6 — URI (NET-6.5) ───────────────────────── BUILT (reparent pending)
public class MalformedUriException           extends NetException;  // carries `int64 position`  ²

// ── Phase 7 — HTTP message (NET-7.7) ──────────────── built
public class HttpException                   extends NetException;  // HTTP family root, kind = KIND_INVALID (12)
public class MalformedMessageException       extends HttpException;
public class HeadersTooLargeException        extends HttpException;  // carries the limit that was exceeded
public class InvalidChunkEncodingException   extends HttpException;
public class UnexpectedEofException          extends HttpException;
public class PayloadTooLargeException        extends HttpException;  // request/response body over the configured cap

// ── Phase 10 — WebSocket (NET-10.8) ───────────────── BUILT
public class WebSocketException              extends NetException;       // WS family root, kind = KIND_INVALID (12)
public class HandshakeRejectedException      extends WebSocketException; // carries `int32 httpStatus`
public class ProtocolViolationException      extends WebSocketException; // carries `int64 position`
public class MessageTooLargeException        extends WebSocketException; // carries `int64 limit`
public class ConnectionClosedException       extends WebSocketException; // carries the RFC 6455 close code (+ reason); kind = KIND_OTHER (99)
```

Three intermediate roots — `TlsException` (NET-5.6), `HttpException`
(NET-7.7) and `WebSocketException` (NET-10.8) — group the TLS, HTTP and
WS families so a handler can `catch (HttpException e)` for *any*
protocol-layer fault without also swallowing a transport-layer
`ConnectionResetException`. All three still descend from `NetException`,
so the request-boundary `catch (NetException e)` remains the universal
net-out. (`CertificateInvalidException` extends `TlsException`, not
`NetException` directly, so `catch (TlsException e)` covers both the
generic handshake/protocol fault and the certificate-rejection case.)

Every instance carries an `int32 kind` — the `cajeta_net_err` ordinal
it was classified to — so a single `catch (NetException e)` can branch
on `e.kind` without a chain of `instanceof` tests, and so the
unmapped-ordinal fallback still preserves the cause. Higher-layer
subtypes (DNS / URI / HTTP / WS) are primarily discriminated by their
**type**, but they still set a *coarse* `kind` so the `e.kind` branch
remains total: the parse-failure subtypes
(`MalformedAddressException`, `MalformedUriException`) classify to
`KIND_INVALID` (12, the "invalid input" bucket); the rest default to
`KIND_OTHER` (99). Their fine-grained discriminant is the structured
detail they carry (`position`, `reason`, close code, …), tabulated
below.

## Mapping chart (socket layer, stage 2 — authoritative today)

`NetErrors.fromErrno(ordinal, detail)` — total over the
`cajeta_net_err` ordinal space:

| `cajeta_net_err` (ordinal) | POSIX errno | Winsock | `NetException` subtype | `kind` |
|---|---|---|---|---|
| `OK` (0) | — | — | `NetException` (caller bug — never on success) | 0 |
| `WOULDBLOCK` (1) | `EAGAIN` / `EWOULDBLOCK` | `WSAEWOULDBLOCK` | `WouldBlockException` ¹ | 1 |
| `CONNECTION_REFUSED` (2) | `ECONNREFUSED` | `WSAECONNREFUSED` | `ConnectionRefusedException` | 2 |
| `CONNECTION_RESET` (3) | `ECONNRESET` | `WSAECONNRESET` | `ConnectionResetException` | 3 |
| `CONNECTION_ABORTED` (4) | `ECONNABORTED` | `WSAECONNABORTED` | `ConnectionAbortedException` | 4 |
| `ADDRESS_IN_USE` (5) | `EADDRINUSE` | `WSAEADDRINUSE` | `AddressInUseException` | 5 |
| `ADDRESS_NOT_AVAIL` (6) | `EADDRNOTAVAIL` | `WSAEADDRNOTAVAIL` | `AddressNotAvailableException` | 6 |
| `HOST_UNREACHABLE` (7) | `EHOSTUNREACH` | `WSAEHOSTUNREACH` | `HostUnreachableException` | 7 |
| `NETWORK_UNREACHABLE` (8) | `ENETUNREACH` | `WSAENETUNREACH` | `NetworkUnreachableException` | 8 |
| `BROKEN_PIPE` (9) | `EPIPE` | `WSAESHUTDOWN` | `BrokenPipeException` | 9 |
| `TIMED_OUT` (10) | `ETIMEDOUT` | `WSAETIMEDOUT` | `TimedOutException` | 10 |
| `INTERRUPTED` (11) | `EINTR` | `WSAEINTR` | `NetException` (caller retries) ³ | 11 |
| `INVALID` (12) | `EINVAL` / `EBADF` / `ENOTSOCK` | `WSAEINVAL` / `WSAEBADF` / `WSAENOTSOCK` | `NetException` | 12 |
| `ACCESS` (13) | `EACCES` / `EPERM` | `WSAEACCES` | `NetException` | 13 |
| `IN_PROGRESS` (14) | `EINPROGRESS` | `WSAEINPROGRESS` / `WSAEALREADY` | `NetException` ⁴ | 14 |
| `OTHER` (99) | anything else | anything else | `NetException` | 99 |

¹ **`WouldBlock` is surfaced as a *value*, not thrown, on the reactor
/ non-blocking hot path** (see [`docs/Net.md`](Networking.md)
§Sockets and NET-1.7): a non-blocking `recv` on an empty socket returns
a distinct "would block" result so the reactor can drive its readiness
loop without a throw. `WouldBlockException` exists so the taxonomy is
total (every ordinal has a subtype) and for the rare caller that
explicitly asks for the throwing variant; the `*Async` forms and the
reactor never raise it.

² **`MalformedUriException`** carries an `int64 position` (the byte
offset where the parse failed) and classifies to `KIND_INVALID` (12).
It is **built today** and (as of NET-6.5) already extends
`NetException` directly — its `(message, position)` constructor
contract is identical to the NET-6.1 placeholder that rooted on
`RecoverableException`. See the in-file
[hierarchy note](../../../../runtime/src/cajeta/io/net/uri/MalformedUriException.cajeta).
`MalformedAddressException` (NET-1.2) follows the same pattern.

³ **`INTERRUPTED`** (`EINTR`) is normally handled by the socket layer
retrying the syscall, not surfaced; it has no dedicated subtype.

⁴ **`IN_PROGRESS`** (`EINPROGRESS`) is the expected outcome of a
non-blocking `connect`; the reactor (NET-3) waits for writability and
checks `SO_ERROR` rather than treating it as a failure. It has no
dedicated subtype.

## Mapping chart (DNS resolve layer — NET-2.5)

`getaddrinfo` reports an `EAI_*` code in a **separate ordinal space**
from socket `errno`, which the NET-2.1 native shim
([`cajeta_net_getaddrinfo.c`](../../../../runtime/native/cajeta_net_getaddrinfo.c))
normalizes into `enum cajeta_resolve_err`. The DNS layer keeps its own
total mapper —
[`ResolveErrors.fromResolveErrno`](../../../../runtime/src/cajeta/io/net/dns/ResolveErrors.cajeta)
— rather than folding that space into `NetErrors.fromErrno` (whose
ordinal `1` means `WOULDBLOCK`, not `NONAME`). It splits the space into
two leaves; the precise ordinal rides on each leaf's `resolveErrno`
field (the coarse `kind` is `KIND_OTHER` 99 for both):

| `cajeta_resolve_err` (ordinal) | POSIX `getaddrinfo` | Winsock | subtype | `resolveErrno` |
|---|---|---|---|---|
| `OK` (0) | — | — | `ResolutionFailedException` (caller bug — never on success) | 0 |
| `NONAME` (1) | `EAI_NONAME` | `WSAHOST_NOT_FOUND` | `UnknownHostException` | 1 |
| `NODATA` (2) | `EAI_NODATA` | `WSANO_DATA` | `UnknownHostException` | 2 |
| `AGAIN` (3) | `EAI_AGAIN` | `WSATRY_AGAIN` | `ResolutionFailedException` (transient — retry may succeed) | 3 |
| `FAIL` (4) | `EAI_FAIL` | `WSANO_RECOVERY` | `ResolutionFailedException` | 4 |
| `FAMILY` (5) | `EAI_FAMILY` | `WSAEAFNOSUPPORT` | `ResolutionFailedException` | 5 |
| `MEMORY` (6) | `EAI_MEMORY` | `WSA_NOT_ENOUGH_MEMORY` | `ResolutionFailedException` | 6 |
| `SYSTEM` (7) | `EAI_SYSTEM` | — | `ResolutionFailedException` | 7 |
| `BADFLAGS` (8) | `EAI_BADFLAGS` | `WSAEINVAL` | `ResolutionFailedException` | 8 |
| `SERVICE` (9) | `EAI_SERVICE` | `WSATYPE_NOT_FOUND` | `ResolutionFailedException` | 9 |
| `OTHER` (99) | any other `EAI_*` | any other | `ResolutionFailedException` | 99 |

`Dns.resolve` (NET-2.2) raises these on a failed/empty lookup, citing
the host in the message; the NET-2.4 `DnsCache` still raises the base
`NetException` on a cached negative (its typed-narrowing is a tracked
follow-up).

## Discriminants carried by the higher-layer subtypes

The non-socket subtypes don't classify a platform errno, so instead of
a `kind` ordinal they each carry the structured detail a caller needs:

| Subtype | Phase | Carried detail | Why |
|---|---|---|---|
| `UnknownHostException` | NET-2.5 | `int32 resolveErrno` ∈ { NONAME 1, NODATA 2 } | distinguish "no such name" from "no record of that family" |
| `ResolutionFailedException` | NET-2.5 | `int32 resolveErrno` ∈ { AGAIN 3, FAIL 4, FAMILY 5, MEMORY 6, SYSTEM 7, BADFLAGS 8, SERVICE 9, OTHER 99 } | a retry policy can single out the transient `AGAIN` (3) |
| `MalformedUriException` | NET-6.5 | `int64 position` | point at the offending byte |
| `CertificateInvalidException` | NET-5.6 | `int32 reason` — a `TlsConnection.CERT_*` ordinal ∈ { `CERT_EXPIRED` 1, `CERT_HOSTNAME` 2, `CERT_UNTRUSTED` 3, `CERT_OTHER` 4 } | the verifier's specific rejection |
| `ConnectionClosedException` (WS) | NET-10.8 | RFC 6455 close code (+ reason) | distinguish a clean 1000 from a 1002 protocol close |
| `HeadersTooLargeException` | NET-7.7 | the limit that was exceeded | actionable abuse diagnostic |

These mirror the proven NET-6.1 pattern: a recoverable parse/validation
failure sets the inherited `message` / `cause` and its own typed field
directly in its constructor (the compiler still rejects `super(...)`
chaining for `Throwable` subtypes — see
[`cajeta.error.Throwable`](../../../../runtime/src/cajeta/error/Throwable.cajeta)).

## Recoverable, not Unrecoverable

`NetException extends RecoverableException` (see
[`docs/specification/error/ErrorModel.md`](../../error/ErrorModel.md)) — and so does
**every** subtype above, transitively. Every networking call site
either:

- catches the relevant subtype (or an intermediate root like
  `HttpException`, or the `NetException` root) explicitly, or
- declares it in its `throws` clause and propagates upward.

The compiler's uncaught-throws check keeps the discipline visible. The
single-rooted tree is what makes the throws clause tractable: a façade
method (`HttpClient.send`, `WebSocket.connect`) declares `throws
NetException` and that one clause covers every fault its transport,
TLS, DNS, and protocol layers can raise.

## Using the map (socket layer, NET-1.3+)

After an intrinsic returns its failure sentinel, the socket wrapper
reads the normalized ordinal (each socket class exposes the native shim
through a private `lastErrorNative()`) and raises the mapped subtype,
citing the operation it was performing:

```cajeta
int64 n = TcpStream.recvNative(this.fd, buf, off, len);
if (n < 0) {
    throw NetErrors.fromErrno(TcpStream.lastErrorNative(), "readAsync");
}
```

`NetErrors.fromErrno(ordinal, detail)` is **total** over the ordinal
space and returns a freshly heap-allocated, *unthrown* exception — the
caller decides whether to `throw` it (the throwing path) or inspect it
(rarely, e.g. logging then continuing).

## Catch-granularity guidance

| You want to catch… | Catch this type |
|---|---|
| one specific transport failure (e.g. refused connect) | `ConnectionRefusedException` |
| any transport failure, branch on cause | `NetException`, switch on `e.kind` |
| any TLS handshake / certificate fault | `TlsException` |
| any HTTP-protocol parse fault | `HttpException` |
| any WebSocket-protocol fault | `WebSocketException` |
| *anything* the networking stack can throw | `NetException` (the universal root) |

## See also

- [`docs/Net.md`](Networking.md) §Error model — the spec table
  this chart consolidates.
- [`docs/specification/io/file/Errors.md`](../file/Errors.md) — the
  sibling file-I/O taxonomy this mirrors.
- [`docs/specification/error/ErrorModel.md`](../../error/ErrorModel.md) —
  `Recoverable` / `Unrecoverable` semantics, throws-clause rules.
- [`runtime/native/cajeta_net_socket.c`](../../../../runtime/native/cajeta_net_socket.c)
  — `enum cajeta_net_err` + `cajeta_net_map_errno` (stage 1).
- [`runtime/src/cajeta/io/net/NetException.cajeta`](../../../../runtime/src/cajeta/io/net/NetException.cajeta)
  — the root + the `KIND_*` ordinal mirror.
- [`runtime/src/cajeta/io/net/NetErrors.cajeta`](../../../../runtime/src/cajeta/io/net/NetErrors.cajeta)
  — `fromErrno` (stage 2).
