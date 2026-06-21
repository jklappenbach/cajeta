---
id: io-net-http-messages
applies-to: [cajeta/io/net/http/HttpRequest, cajeta/io/net/http/HttpResponse, cajeta/io/net/Headers]
title: HTTP message model — HttpRequest / HttpResponse over a Headers map
description: Build HTTP request/response messages fluently over a case-insensitive multi-value Headers map; pure data, no I/O.
---

# HTTP message model (request + response + headers)

Three cooperating **pure-data** types — no sockets, no native intrinsics. They are the
in-memory shape of an HTTP/1.1 message; the parser (NET-7.3), body framing (NET-7.4),
serializer (NET-7.5), `HttpClient`, `Router`, and `HttpServer` all read and write
*through* them but live in other files. If you need to move bytes on a wire, you are in
the wrong skill — these types do **not** parse, serialize, connect, or send.

| Member | Role |
| --- | --- |
| `HttpRequest` | request-line triple (`method`, `target`, `version`) + `Headers` + optional in-memory body. Method is **case-sensitive**. |
| `HttpResponse` | status-line triple (`version`, `status`, `reason`) + `Headers` + optional in-memory body, plus `1xx`–`5xx` classifier predicates. |
| `Headers` (in `cajeta.io.net`, not `.http`) | case-insensitive, multi-value, insertion-ordered field map shared by both messages. |

## Object graph & ownership

- `HttpRequest`/`HttpResponse` each **own** a `Headers` (created in their ctor, never
  null). `getHeaders()` returns the **live, borrowed** map — mutating it mutates the
  message; do not free it.
- Factories return `#`-owned messages the caller owns: `HttpRequest.get/post/of/fromUri`,
  `HttpResponse.ok/notFound/of`.
- Fluent mutators (`header`, `setHeader`, `body`, `version`, `reason`) return the
  **borrowed `this`** (not a new object, not a `#` transfer) for chaining — the same
  convention as `JsonWriter`.
- `body(data, length)` copies `length` bytes into a **fresh owned** buffer and sets
  `hasBody`; it does not alias your input. `hasBody == false` is "no body";
  a zero-length buffer with `hasBody == true` is a present-but-empty body (distinct).
- `Headers.add/set` take `String` args **borrowed** but store internal copies
  (name lowercased, value OWS-trimmed). `Headers.get(name)` and `getAll(name)` return
  **fresh `#`-owned view `String`(s)** over the stored bytes — owned by you, never an
  alias of the map's storage; copy nothing, but you own what you get back.

## Worked example (mirrors HttpMessageTests over the JIT)

```cajeta
import cajeta.lang.String;
import cajeta.io.net.Headers;
import cajeta.io.net.uri.Uri;
import cajeta.io.net.http.HttpRequest;
import cajeta.io.net.http.HttpResponse;

// Request: factory -> chained borrowed-`this` mutators.
HttpRequest req = HttpRequest.get("/index.html")
    .header("Host", "example.test")
    .header("Accept", "text/html");

// Or derive origin-form target + Host header straight from a Uri:
Uri u = Uri.parse("http://h.test/a/b?x=1&y=2");
HttpRequest r2 = HttpRequest.fromUri("GET", u);   // target == "/a/b?x=1&y=2", Host: h.test

// Response with a body:
int8[] payload = heap int8[2];
payload[0] = (int8) 104;   // 'h'
payload[1] = (int8) 105;   // 'i'
HttpResponse resp = HttpResponse.ok()
    .header("Content-Type", "text/plain")
    .body(payload, 2);
if (resp.isSuccess()) { /* 2xx */ }
```

## Headers: the two read views (the cross-class subtlety)

A repeated header occupies one slot per occurrence, in wire order. Pick the accessor by
semantics:

- `getAll(name)` — raw per-occurrence `#String[]` (length 0 when absent, **never null**),
  never folded. Correct for `Set-Cookie`.
- `get(name)` — single value or **`null`** when absent. For ordinary multi-value headers
  it returns the **comma-folded** `"a, b"` (RFC 7230 §3.2.2); for `Set-Cookie` it returns
  only the **first** value (folding cookies is lossy) — use `getAll` there.
- `add` appends (multi-value); `set` is last-write-wins (removes all then appends one).
- `nameAt(i)`/`valueAt(i)` over `0..count()` is the serializer's slot walk; `count()` is
  the **slot** count (repeats included), not distinct-name count.

## Sharp edges

- **Method is case-sensitive** — `"get"` != `"GET"`. Header names are case-insensitive
  (lowercased on insert), so `header("Accept", …)` is found by `getAll("accept")`.
- `get(name)` is the **only** message/header read that returns null; `getAll` returns a
  zero-length array instead. Null-check `get`, length-check `getAll`.
- These types carry no defensive copies and expose public fields ("immutable-ish"):
  freeze-by-convention once handed to the serializer.
- `reason` is advisory — `HttpResponse.of(code)` fills the standard RFC 7231 phrase for
  known codes and an **empty** string for unknown ones; clients key off `status`, read via
  `statusCode()` or the `isSuccess`/`isRedirect`/`isClientError`/`isServerError`/
  `isInformational` predicates.
- `HttpRequest.pathParams` is null until a `Router` calls `bindPathParams`; `pathParam(name)`
  returns null when no router ran or no such `{name}` — see the `Router` (NET-9.3) skill.
