---
id: io-net-ws-handshake
applies-to: [cajeta/io/net/ws/WsClientHandshake, cajeta/io/net/ws/WsServerHandshake, cajeta/io/net/ws/WsUpgrade]
title: WebSocket opening handshake — client builds/validates Upgrade, server computes Sec-WebSocket-Accept
description: The RFC 6455 §4 opening handshake — pure HttpRequest/HttpResponse logic (WsClientHandshake + WsServerHandshake) plus the socket-facing WsUpgrade glue; how the three cooperate, who throws, and the accept-token rule.
---

# WebSocket opening handshake (RFC 6455 §4)

Three cooperating classes in `cajeta.io.net.ws` turn an HTTP/1.1 `Upgrade: websocket`
exchange into a live `WebSocket`. Pick by what you have:

- Have a connected `AsyncReader`/`AsyncWriter` and want a `WebSocket` → call
  **`WsUpgrade.acceptServer`** (server) or **`WsUpgrade.connectClient`** (client). This is
  the only one that touches the transport; it drives the other two.
- Have/produce an `HttpRequest`/`HttpResponse` and want pure, socketless handshake logic
  (testing, or routing inside an existing HTTP server) → call **`WsServerHandshake`**
  (validate the client request, build the `101`) or **`WsClientHandshake`** (build the
  request, validate the `101`).

What this component does **not** do: no socket connect/listen (caller opens the transport
and owns it), no TLS negotiation, no CSPRNG for the client key (see the placeholder note
below), no framing — once the `101` is exchanged, message I/O is `WebSocket` +
`WsFrameDecoder`/`WsFrameEncoder` (a different skill). All methods are `static`; you never
instantiate these classes.

## Members and roles

- **`WsServerHandshake`** — server side. Validates a client Upgrade request, computes the
  `Sec-WebSocket-Accept` token, builds the `101 Switching Protocols` response. Also the
  home of the accept-token math `acceptKey(key)` = `base64(SHA-1(key + GUID))`, which the
  *client* reuses to verify the server.
- **`WsClientHandshake`** — client side. Builds the Upgrade `HttpRequest` with a
  `Sec-WebSocket-Key`, and validates the server's `101` by recomputing the expected accept
  via `WsServerHandshake.acceptKey`.
- **`WsUpgrade`** — socket-facing glue (NET-10.7). Reads/writes the HTTP head over a
  borrowed `AsyncReader`/`AsyncWriter`, calls the two handshake classes, and returns a
  role-correct `WebSocket`.

## Call sequence

Server (`WsUpgrade.acceptServer`): read request head off reader → `WsServerHandshake.accept(request)`
(which runs `validate` then `acceptKey` then builds the `101`) → serialize + write +
flush → `WebSocket.forServer(reader, writer)`.

Client (`WsUpgrade.connectClient`): `WsClientHandshake.placeholderKey(seed)` →
`WsClientHandshake.buildRequest(uri, key)` → serialize + write + flush → read response head
→ `WsClientHandshake.requireAccept(response, key)` → `WebSocket.forClient(reader, writer)`.

The two handshake classes interoperate directly — `WsClientHandshake.validateAccept` calls
`WsServerHandshake.acceptKey(key)` to compute what the server should have returned.

## Ownership across the boundary

- `WsUpgrade.acceptServer/connectClient` return `#WebSocket` (owned — caller owns and must
  drop it). The `AsyncReader`/`AsyncWriter` are **borrowed**: the caller that opened the
  socket owns both halves and must keep them alive for the returned `WebSocket`'s lifetime
  (the `WebSocket` borrows them).
- `WsClientHandshake.buildRequest` returns `#HttpRequest`, `placeholderKey` returns
  `#String`, `WsServerHandshake.accept`/`reject`/`switchingResponse`/`acceptKey`/`guid`
  return `#...` (owned) — the caller owns each.
- Header values from `Headers.get(name)` are **borrowed** views (nullable) — `null` means
  absent. The validation helpers treat `null` as "missing".

## Errors

- `WsServerHandshake.validate` / `accept` throw `#HandshakeRejectedException` (a
  `WebSocketException`, `kind == 12` / `KIND_INVALID`) carrying `httpStatus`: **`426`** for
  a `Sec-WebSocket-Version` other than `13` (server should echo `Sec-WebSocket-Version: 13`
  — `WsServerHandshake.reject(e)` builds that response), **`400`** for any other
  malformation (non-`GET`, bad/missing `Upgrade`/`Connection`/`Sec-WebSocket-Key`).
- `WsServerHandshake.isUpgradeRequest` is the non-throwing predicate (a router branches on
  it); note a version mismatch returns `false` here rather than distinguishing `426`.
- `WsClientHandshake.validateAccept` returns `boolean` (`false` on wrong status, missing or
  mismatched accept); `requireAccept` is the throwing form (`HandshakeRejectedException`,
  status `400`). `WsUpgrade.connectClient` uses `requireAccept`.

## The accept-token rule (RFC 6455 §1.3)

`Sec-WebSocket-Accept = base64(SHA-1(Sec-WebSocket-Key + GUID))` with the fixed protocol
GUID `258EAFA5-E914-47DA-95CA-C5AB0DC85B11`. `acceptKey` streams key then GUID into `Sha1`
as two `update`s (no concat buffer). SHA-1's weakness is irrelevant — the GUID is a public
magic string, not a secret; this is a handshake proof, not a MAC. Acceptance vector: key
`"dGhlIHNhbXBsZSBub25jZQ=="` → accept `"s3pPLMBiTxaQ9kYGzzhZRbK+xOo="`.

Validation specifics the methods rely on: `Connection` is matched as a case-insensitive
**token containment** check (so `keep-alive, Upgrade` matches `upgrade`), `Upgrade` is
case-insensitive, `Sec-WebSocket-Key` must be present and non-empty but is **not** decoded
or length-checked, `Sec-WebSocket-Version` must equal exactly `13`.

> **Client key is a placeholder, not for production.** `placeholderKey(seed)` derives the
> 16 key bytes from `seed` with an LCG mixer — deterministic, explicitly **not** a CSPRNG
> (no stdlib CSPRNG in the tree yet). Distinct seeds give distinct keys; vary it per
> connection. The key is not a security secret in the handshake, so this does not break
> correctness — only cross-protocol cache-poisoning defense.

## Pure handshake example (no socket)

```cajeta
import cajeta.lang.String;
import cajeta.io.net.uri.Uri;
import cajeta.io.net.http.HttpRequest;
import cajeta.io.net.http.HttpResponse;
import cajeta.io.net.ws.WsClientHandshake;
import cajeta.io.net.ws.WsServerHandshake;

Uri uri = Uri.builder().scheme("ws").host("example.test").port(80).path("/chat").build();
String key = WsClientHandshake.placeholderKey(42);      // #String, owned
HttpRequest req = WsClientHandshake.buildRequest(uri, key);   // #HttpRequest, owned

if (!WsServerHandshake.isUpgradeRequest(req)) { return -1; }
HttpResponse resp = WsServerHandshake.accept(req);      // #HttpResponse (101); throws on bad req
if (resp.statusCode() != 101) { return -2; }
if (!WsClientHandshake.validateAccept(resp, key)) { return -3; }   // client checks the accept token
```

(Mirrors `WsEntryPointTests.clientHandshakeRoundTripsWithServer`;
`WsServerHandshakeTests` pins the `acceptKey` RFC vector and the rejection statuses.)

## Over a transport

```cajeta
import cajeta.io.net.AsyncReader;
import cajeta.io.net.AsyncWriter;
import cajeta.io.net.uri.Uri;
import cajeta.io.net.ws.WebSocket;
import cajeta.io.net.ws.WsUpgrade;

// reader/writer are borrowed — caller opened the socket and keeps them alive.
WebSocket server = WsUpgrade.acceptServer(reader, writer);          // #WebSocket
WebSocket client = WsUpgrade.connectClient(reader, writer, uri, 42); // #WebSocket, key seed 42
```
