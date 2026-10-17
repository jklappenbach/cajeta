# h2-streaming-request-body — an h2 stream's handler starts at end of HEADERS when its body streams

Status: draft, written 2026-10-17 with Julian (decisions 2026-10-17: flow
control is the only back-pressure and the window follows the slab; an unread
remainder is drained up to 256 KB, else RST_STREAM(NO_ERROR); PART mode on
h2 is in scope; ships as cajeta-http 0.8.0). Origin: request-dispatch plan
5.1.5, held because the h2 server had no streaming request-body mode.
Companion: `request-dispatch-spec.md` §2.2, §2.3, §2.4, §4.4.

## 1. Definition

### 1.1 Purpose

`dev.cajeta.http`'s HTTP/1.1 server hands a handler its request body at head
complete when `streamRequestBody` is on or the route is a `STREAM` multipart
route, and runs a handler per completed part on a `PART` route. The h2
server does neither: a stream's body is collected in its slot and the handler
runs at END_STREAM, whatever the route says. This spec gives h2 streams the
same three body modes as HTTP/1.1 requests, under both server models, with
h2 flow control as the only back-pressure, so a stream's handler can read a
body of any size in the memory of one connection buffer.

### 1.2 The problem, measured

- `Http2StreamSlot.append` copies DATA into the slot's input buffer behind
  the head and, once that is full, into a heap array that doubles as it
  grows; `endBody` binds it and the slot is dispatched. A 100 MB upload
  allocates about 200 MB on the way and is held whole until the last DATA
  frame. Anything over `maxBodyBytes` is refused with ENHANCE_YOUR_CALM.
- `ServerLimits.streamRequestBody` and `multipart(route, STREAM)` have no
  effect on an h2 stream; `multipart(route, PART)` runs the handler once at
  END_STREAM with no part set. The request-dispatch acceptance 5.1.5 ("a
  stream with a streaming body dispatches at end of HEADERS and the
  handler's body reads park") could not be written.
- The flow-control windows exist and are honored for the whole-body path:
  the server advertises a 1 MiB connection window and the default 65,535
  byte stream window, and returns credit in 512 KB steps as DATA arrives,
  regardless of whether anyone has consumed the bytes.

### 1.3 Scope

- h2 streams under the pool (the framer feeds the connection) and under
  `fiberPerConnection()` (the connection's own reader feeds it).
- The three modes: whole (today), streaming (`streamRequestBody`, a STREAM
  route), and PART.
- Flow control as the memory bound; the unread-remainder policy; the
  interaction with `maxInFlight`, `SETTINGS_MAX_CONCURRENT_STREAMS` and
  `serializedStreams`.

### 1.4 Non-goals

- The h2 client (`Http2Client`) is unchanged.
- Trailer fields are not exposed to the handler; trailers end the body as
  END_STREAM does (§7.1).
- Priority and push are unchanged.
- No new builder calls: the existing `streamRequestBody`, `multipart` and
  `maxPartBytes` gain their meaning on h2.

### 1.5 Constraints

- A streaming stream allocates nothing beyond the slot it already holds.
- The framer never parks: feeding a body channel is a copy into the slot's
  buffer and a wake, never a wait.
- The handler's reads park on the stream's body channel, never on the
  socket; the socket belongs to the framer (or, under the old model, the
  connection's reader fiber).
- Ordering stays as request-dispatch §4.4: streams of one connection run
  independently unless `serializedStreams(true)`.

## 2. When a stream streams

### 2.1 The hold point

A stream's hold point is END_STREAM, as today, unless one of these holds
when the request head is complete (END_HEADERS, after any CONTINUATION):

- `streamRequestBody` is on and the stream carries a body;
- the request's method and path match a `STREAM` multipart route;
- the request's method and path match a `PART` multipart route (§2.7).

Then the hold point is head complete: the slot is dispatched as a unit with
its body open, and `HttpRequest.bodyStreamed()` is true in the handler.

- 2.1.1 As a developer with `streamRequestBody` on, when a client opens a
  stream with HEADERS carrying content-length 2 GB, then my handler starts
  before any DATA arrives and `bodyStream()` is the body.
- 2.1.2 As a developer with a STREAM route and `streamRequestBody` off,
  when a 64 MB part arrives on that route, then my handler reads it
  incrementally; a request on another route is still held whole and
  bounded by `maxBodyBytes`.
- 2.1.3 As a developer, when a stream's HEADERS carry END_STREAM, then there
  is no body and the hold point and the handler are as today.

### 2.2 The body channel

An open stream's body is a `Body` whose reader parks until DATA is in the
slot's buffer or the stream has ended. DATA is copied into the slot's input
buffer behind the head, as `append` does today, and the reader consumes it
in place; the heap spill of `append` is never used for an open stream.

- 2.2.1 As a handler, when I read the body in 64 KB steps, then each read
  returns what has arrived, parks when nothing has, and returns 0 after the
  last DATA frame.
- 2.2.2 As a handler whose body read parks, then the framer goes on serving
  every other stream and connection; the park costs no carrier.
- 2.2.3 As a handler, when the client resets the stream before the body
  ends, then my next read raises and `Body.live()` is false; the response I
  may have started is dropped as today.

### 2.3 Flow control is the back-pressure

The server grants a streaming stream exactly the free space of its slot's
input buffer after the head, and returns credit only as the handler
consumes: a WINDOW_UPDATE for the stream and for the connection carries the
bytes consumed since the last one, sent once they reach a quarter of the
stream's window or the body ends. The client can therefore never have more
bytes in flight for one stream than fit in its slot, and a handler that
reads slowly slows only its own stream.

With the default 64 KB slab the stream window is 64 KB minus the head, about
one slab per round trip when the handler lags; `bufferPool` with a larger
slab raises it in step, with no new allocation. (Julian, 2026-10-17,
choosing this over a heap-backed per-stream queue: zero allocation per
streaming stream, throughput tuned by the slab.)

- 2.3.1 As a developer, when 100 streams each stream a 1 GB body at once
  through a server with the default slab, then the server's allocated bytes
  after warm-up grow by less than one slab per stream and every body is
  read to the end.
- 2.3.2 As a client, when one handler stops reading for a second, then that
  stream's window closes and my other streams on the same connection keep
  moving at full rate.
- 2.3.3 As a client, when the handler consumes, then I receive WINDOW_UPDATE
  frames for the stream and the connection that together equal what was
  consumed, never more than the slot can hold.

### 2.4 End of body

END_STREAM on a DATA frame, or a trailing HEADERS frame, ends the body: the
reader returns 0 at the end of what arrived. A content-length that does not
match the bytes received is a stream error (PROTOCOL_ERROR), as RFC 9113
§8.1.1 requires, raised to a reader that is still reading.

- 2.4.1 As a handler, when the body ends with trailers, then my read returns
  0 after the last DATA byte and the trailer fields are not visible (§7.1).
- 2.4.2 As a handler, when the client sends fewer bytes than content-length
  and END_STREAM, then my read raises a protocol error.

### 2.5 An unread remainder

When the handler returns with the body still open, the server reads and
discards up to 256 KB more; if the body has still not ended it sends
RST_STREAM(NO_ERROR) for the stream, which RFC 9113 §8.1 allows after a
complete response, and the connection goes on. (Julian, 2026-10-17; the
limit mirrors HTTP/1.1's `Http1Connection.DRAIN_LIMIT`.)

- 2.5.1 As a handler that answers 413 after reading one frame of a 1 GB
  upload, then the client sees my response followed by RST_STREAM(NO_ERROR)
  on that stream, and its next stream on the connection is served.
- 2.5.2 As a handler that returns with 100 KB unread, then the stream closes
  cleanly with no RST_STREAM.

### 2.6 Limits

`maxBodyBytes` does not bound a streaming stream's declared length; the
handler decides what to read, as for HTTP/1.1. `maxHeadBytes`, the slot's
head limit and `SETTINGS_MAX_CONCURRENT_STREAMS` apply as today.

- 2.6.1 As a developer with `maxBodyBytes(1 MB)` and `streamRequestBody`
  on, when a 2 GB upload arrives, then my handler is dispatched and reads it;
  with `streamRequestBody` off the stream is refused as today.

### 2.7 PART mode on h2

On a PART route the framer feeds `MultipartParser` from each DATA frame, and
each completed part is a unit of the stream, in order, dispatched once it is
known whether it is the last (request-dispatch plan 12.1.2's rule); the
response leaves after the last part. The parts of one stream run one at a
time in order; other streams of the connection run independently. A part
over `maxPartBytes` ends the request with 413 and the stream with
RST_STREAM(NO_ERROR) after the response, per §2.5. An empty form runs the
handler once with no part.

- 2.7.1 As a developer with `multipart("POST", "/parts", PART)`, when a
  five-file form arrives on one h2 stream, then my handler runs five times
  in order with `multipartPart()`, `multipartIndex()` and `multipartLast()`
  set as on HTTP/1.1, and the response is written after the fifth.
- 2.7.2 As a developer, when two streams each carry a five-file form, then
  their part runs interleave freely across streams and stay in order within
  each.
- 2.7.3 As a developer with `maxPartBytes(1024)`, when a 4 KB part arrives,
  then the request ends with 413.

### 2.8 Both server models

Under the pool the framer feeds the slot and a pooled fiber runs the handler
from head complete. Under `fiberPerConnection()` the connection's reader
fiber feeds the slot and the handler fiber is spawned at head complete
instead of at END_STREAM; `Http2Connection.serve` keeps its shape. The use
cases of §2.1 to §2.7 hold under both.

- 2.8.1 As a developer who built with `fiberPerConnection()`, when I stream
  a 2 GB body on one h2 stream, then it is read in the memory of one slot
  and the connection's other streams keep moving.

## 3. Accounting and ordering

- 3.1 An open streaming stream is one unit in flight from dispatch to the
  handler's return, under `maxInFlight`; one concurrent stream under
  `SETTINGS_MAX_CONCURRENT_STREAMS`; one slot in the connection's slot pool.
  A PART stream is one unit per part, as on HTTP/1.1.
- 3.2 Under `serializedStreams(true)` a streaming stream holds the
  connection's turn until its handler returns, as any stream does today.
- 3.3 Push promises and priority are unchanged.

- 3.1.1 As a test, when 300 clients each open one streaming stream against
  `maxInFlight(256)`, then 256 handlers run and 44 streams hold at head
  complete until a handler returns.

## 4. Surface

No new builder calls. `ServerLimits.streamRequestBody`, `multipart(method,
pattern, mode)` and `maxPartBytes` now mean on h2 what they mean on
HTTP/1.1. `HttpRequest.bodyStreamed()`, `bodyStream()`, `multipartPart()`,
`multipartIndex()` and `multipartLast()` are unchanged. The server's
counters gain `streamedDispatches()`: units dispatched at head complete
across protocols, for tests and operators.

## 5. Measurement and acceptance

- 5.1 A 2 GB upload on one h2 stream (a counting source, as
  MultipartDispatchTests 7.1.3 does at 64 MB) read to the end with less than
  one slab allocated after warm-up, under both models.
- 5.2 Fifty streaming uploads on one connection with handlers reading at
  different rates: every body complete, and a handler stalled for one second
  stalls only its own stream (the others' byte counts keep rising during the
  stall).
- 5.3 Early return (2.5.1) and the 100 KB remainder (2.5.2).
- 5.4 PART on h2: 2.7.1 to 2.7.3, plus the split-delimiter case of
  request-dispatch 12.1.2 across two DATA frames.
- 5.5 Every `Http2*Tests` suite, `Http2FlowParkTests` and
  `Http2ConcurrencyTests` pass unchanged; the HTTP/1.1 figures of
  request-dispatch §8 are unchanged.
- 5.6 The write-side allocation test (request-dispatch 5.1.6) still reads 0
  bytes for a warm whole-body stream.

## 6. Compatibility

- A server with `streamRequestBody` on that served h2 before now dispatches
  h2 handlers at head complete instead of END_STREAM. That is the documented
  meaning of the flag and a behavior change for h2 only; it ships as
  cajeta-http 0.8.0 with a note in `docs/http-spec.md` and the README.
- Routes without a mode, and servers without `streamRequestBody`, see no
  change: the hold point stays END_STREAM and `maxBodyBytes` still bounds
  the body.
- primavera and the samples need no source change.

## 7. Open questions

- 7.1 Trailers: exposed to the handler as `HttpRequest.trailers()` after the
  body ends, or dropped as today? Recommendation: dropped in 0.8.0; a
  `trailers()` view when a consumer asks for one, since the slot buffer can
  hold them behind the head.
- 7.2 Whether `streamedDispatches()` should split by protocol.
  Recommendation: one counter; tests tell protocols apart by the server
  they build.
