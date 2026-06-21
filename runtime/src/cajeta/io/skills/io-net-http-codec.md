---
id: io-net-http-codec
applies-to: [cajeta/io/net/http/HttpParser, cajeta/io/net/http/BodyReader, cajeta/io/net/http/BodyFraming, cajeta/io/net/http/HttpSerializer, cajeta/io/net/http/ChunkedEncoder]
title: HTTP/1.1 codec — incremental parse → framing → body stream, and serialize/chunk-encode
description: Wire the resumable HttpParser to a BodyFraming decision and a streaming BodyReader; serialize heads/bodies and stream chunks. Pure logic over int8[], no sockets.
---

# HTTP/1.1 codec

The transport-agnostic HTTP/1.1 read+write codec. **Pure logic over `cajeta.lang.String` + `int8[]` — no sockets, no clock, no native intrinsics.** The async transport (`AsyncReader`/`AsyncWriter`) layers over this later; everything here is fed bytes you already have and hands back bytes you then write. Golden-vector tested under `test/net/golden/http/`.

## Members and roles

| Class | Side | Role |
| --- | --- | --- |
| `HttpParser` | decode | Resumable head state machine: feed byte chunks until `CRLFCRLF`, then emit a parsed `HttpRequest`/`HttpResponse` + a `BodyFraming`. Stops at the head boundary. |
| `BodyFraming` | hand-off | Pure value type: the body-delimitation decision (`KIND_NONE`/`KIND_CONTENT_LENGTH`/`KIND_CHUNKED`/`KIND_CLOSE`, a `length`, a `keepAlive` flag). The clean boundary between parser and body reader. |
| `BodyReader` | decode | Streaming body decoder driven by a `BodyFraming`: consumes raw body bytes, `drain()`s decoded output, captures pipelined leftover. Never buffers the whole payload. |
| `HttpSerializer` | encode | Renders a whole `HttpRequest`/`HttpResponse` (head + buffered body) to wire bytes, length-framed or as a single chunk. |
| `ChunkedEncoder` | encode | Streaming chunk framer: `encodeChunk` per piece + one `encodeLast`, for a body whose length isn't known up front. |

## Read pipeline — the cross-class call sequence

`HttpParser.feed(...)` (loop) → `isComplete()` → `getRequest()`/`getFraming()` → `BodyFraming` → `BodyReader.forFraming(framing)` → seed with `parser.leftover()` → `feed`/`drain` loop → `leftover()` is the next pipelined message.

```cajeta
import cajeta.lang.String;
import cajeta.io.net.http.HttpParser;
import cajeta.io.net.http.HttpRequest;
import cajeta.io.net.http.BodyFraming;
import cajeta.io.net.http.BodyReader;

HttpParser p = HttpParser.forRequest();          // or forResponse()
p.feed(chunk1, chunk1.count());                  // partial head ok
boolean headDone = p.feed(chunk2, chunk2.count());
if (!p.isComplete()) { return; }                 // need more bytes
HttpRequest req = p.getRequest();                // BORROWED — parser still owns it
BodyFraming  f  = p.getFraming();

BodyReader br = BodyReader.forFraming(f);
int8[] seed = p.leftover();                       // body bytes that rode in with the head
br.feed(seed, seed.count());
while (!br.isComplete()) {
    int32 n = conn.read(buf);                     // (your transport)
    if (n == 0) { br.endInput(); break; }         // peer closed
    br.feed(buf, n);
    int8[] piece = br.drain();                     // stream decoded bytes out
    sink.write(piece, piece.count());
}
int8[] tail = br.leftover();                       // start of the next keep-alive message
```

The same `int8[]` + length pair threads through every `feed`. Pass the length as a local (`x.count()`), never inline a struct field into the call.

## Ownership across the component boundary

- **Factories return `#`-owned** values you own: `HttpParser.forRequest()`, `BodyReader.forFraming()`, `BodyFraming.none()/contentLength()/chunked()/closeDelimited()`.
- **`getRequest()`/`getResponse()`/`getFraming()` BORROW** — the parser still owns them and frees them on drop. If a parser is short-lived but you keep the message, call **`takeRequest()`/`takeResponse()`** instead: they detach (transfer ownership, null the parser's handle so its drop won't double-free). `HttpClient` and the WebSocket upgrade use the `take*` form.
- **`leftover()` / `drain()` each return a FRESH owned `#int8[]`** of exactly `leftoverLength()` / `decodedLength()` bytes — a copy, not a view. `drain()` additionally **clears** the decoded buffer (call it repeatedly across feeds for true streaming). `leftover()` does not clear.
- **`HttpSerializer.toBytes()` returns owned bytes AND resets** the serializer for reuse. `ChunkedEncoder.encodeChunk`/`encodeLast` return owned `#int8[]`.
- `BodyFraming.length` is bytes for `KIND_CONTENT_LENGTH`, `0` for `KIND_NONE`, `-1` for `KIND_CHUNKED`/`KIND_CLOSE`.

## Framing decision (RFC 7230 §3.3.3) — the order that matters

`HttpParser` resolves framing in this order: bodyless status (`1xx`/`204`/`304`, responses only) → `Transfer-Encoding: chunked` (**wins over** `Content-Length` if both present) → numeric `Content-Length` → default. The default is `KIND_NONE` for a **request** with no framing headers, `KIND_CLOSE` (read-to-EOF) for a **response**. A request is **never** close-delimited.

`BodyReader.forFraming` dispatches on `framing.kind`: `KIND_NONE` and `KIND_CONTENT_LENGTH` with length 0 are **complete from construction**. `KIND_CLOSE` **never completes from `feed` alone** — you must call `endInput()` (the connection close is the delimiter). For `KIND_CONTENT_LENGTH`/`KIND_CHUNKED`, `endInput()` on an unfinished body raises `UnexpectedEofException` (truncation).

## Write side — serialize vs stream-chunk

- One-shot whole message: `HttpSerializer.request(req)` / `response(resp)` (length-framed) or `requestChunked` / `responseChunked` (single-chunk). These auto-supply a framing header only when the caller set neither (`content-length` added iff no `Content-Length` and no `Transfer-Encoding`; `transfer-encoding: chunked` added iff absent). Auto-supplied names are emitted lowercase.

```cajeta
import cajeta.io.net.http.HttpResponse;
import cajeta.io.net.http.HttpSerializer;

HttpResponse resp = HttpResponse.ok();
resp.body(payload, payload.count());
int8[] wire = HttpSerializer.response(resp);     // owned; serializer resets after toBytes
```

- Streaming body whose length is unknown up front: write the head with `HttpSerializer.responseChunkedHead(resp)`, then per piece `ChunkedEncoder.encodeChunk(piece, n)`, then exactly one `ChunkedEncoder.encodeLast()`.

## What this component does NOT do

- **No I/O, no clock.** No sockets are opened; `Date` is not stamped (NET-9.1 does that), `Host` is not auto-added (use `HttpRequest.fromUri`), and `Connection` keep-alive *policy* is decided elsewhere (NET-7.6) — the serializer emits only headers already on the map. `BodyFraming.keepAlive` is just the per-message read, not the reuse decision.
- **No multi-chunk in the serializer.** `HttpSerializer.writeBody(..., chunked=true)` frames an *already-buffered* body as a single chunk; for incremental pieces use `ChunkedEncoder`.
- **`encodeChunk` of an empty piece emits nothing** (a zero-size chunk is the last-chunk terminator and would prematurely close the stream). Likewise the serializer omits the chunk for an empty body but still writes the `0\r\n\r\n` terminator.
- **A parser parses exactly one message.** On a keep-alive connection construct a fresh `HttpParser` (or `reset`) per message; the previous message's `leftover()` is the next parser's first `feed`.
- The parser does not validate the body, and chunked decoding ignores chunk-extensions and trailer field content (consumed, never emitted).

## Abuse / errors

- `HttpParser.feed` consults `HttpParserLimits` *as it accumulates* → `HeadersTooLargeException` (head/line/count ceilings); malformed head → `MalformedMessageException` (cites byte offset); `endInput()` before the head terminated → `UnexpectedEofException`.
- `BodyReader` chunked errors → `InvalidChunkEncodingException` (non-hex size, overflow, missing CRLF, unterminated trailer), citing the body-stream offset; truncated fixed/chunked body on `endInput()` → `UnexpectedEofException`.

For the message value types and their builders see `HttpRequest`/`HttpResponse`; for the header map see `cajeta/io/net/Headers`.
