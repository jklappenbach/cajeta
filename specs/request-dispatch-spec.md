# request-dispatch — the server assigns a fiber when a unit of work is complete

Status: draft, 2026-10-16. Repo of record: cajeta (ecosystem spec); the work
lands in cajeta-http with two runtime enablers in cajeta. Origin:
primavera-web plan 1.2.8.6, decided with Julian 2026-10-15 and 2026-10-16.
Companion: `fiber-stack-pool-spec.md` (runtime stack reuse and size classes).

## 1. Definition

### 1.1 Purpose

A cajeta HTTP server assigns a fiber to a connection only when that
connection holds a complete unit of work: an HTTP request, a multipart part,
an h2 stream, a WebSocket message, or a unit a developer-supplied framer
recognizes. Fibers scale with concurrent work, not with connections. Idle
connections cost a socket, buffers and TLS state, and no fiber.

### 1.2 The problem, measured

- `sharedPool(N)` today hands a worker a connection. The worker runs the
  HTTP/1.1 keep-alive loop until the peer closes
  (`Http1Connection.serve`), so an idle keep-alive connection pins a worker
  and the ninth idle browser waits in the accept backlog behind eight.
- The HTTP server's TLS path spawns a fiber per connection and h2 spawns a
  fiber per stream regardless of the configured model. For what primavera
  serves, the pool setting is accepted and unused.
- Every `spawn` maps a fresh 1 MiB guard-paged stack and unmaps it on exit:
  `mmap`, `mprotect`, `munmap` per fiber, two mappings each against Linux's
  default 65,530, so about 32K live fibers is a wall. The "45 bytes per
  request" in primavera-web 1.2.8.4 was the fiber record alone.
- Fibers are stackful. `await` parks the fiber and keeps its stack. The cost
  of a waiting request is its touched stack pages, and the lever is how many
  fibers exist and how big their stacks are, never a release at `await`.

### 1.3 Scope

- cajeta-http: `HttpServer`, its builder, the HTTP/1.1, h2 and WebSocket
  server paths, multipart body modes, the upgrade path, and a developer
  framer API.
- cajeta runtime, two enablers: a multiplexed readiness wait so one fiber
  can wait on many sockets (§3.4), and a carrier count surface (§3.1).
- A cajeta-written load client for the acceptance measurements (§8).

### 1.4 Non-goals

- A stackless rewrite of `async`. Deferred by the concurrency spec; nothing
  here needs it.
- A parked worker fiber per h2 stream slot per connection. Rejected
  2026-10-15: up to 100 idle fibers per connection is the wrong direction
  for a mostly idle fan-out.
- Stack reuse and stack size classes. `fiber-stack-pool-spec.md`.
- The HTTP client. Unchanged.
- Changing the stdlib `cajeta.io.net.Server` default model. The stdlib
  keeps fiber per connection as its default; the HTTP server chooses.

### 1.5 Constraints

- Handler code is unchanged. `(HttpRequest, HttpResponse) -> void` and
  `WebSocketHandler` keep their signatures and semantics.
- Framers never run developer code and never park on a single socket.
- Nothing buffers without a bound. Every hold point has a cap, and a peer
  over it is answered or closed, never queued.
- Per-connection order is preserved by default.
- The 1.2.8.4 result stands: 1000 h2 responses through the write side
  allocate 0 bytes.

## 2. Units of work and hold points

A unit of work is one connection holding one complete consumable per its
protocol. The hold point is where the framer stops reading for that unit
and makes it dispatchable.

### 2.1 HTTP/1.1 request

Hold until the head is complete (the scanner finds the blank line) and the
body is complete: `Content-Length` bytes present, or the chunked terminator
present. Caps: the existing `ServerLimits` head caps (431 over them),
`maxBodyBytes` (413, declared or crossed), and the head and body timeouts,
measured by the framer from the first byte of the unit.

- 2.1.1 As a client, when I send a GET, then the handler runs once the head
  is complete and no fiber existed before that.
- 2.1.2 As a client, when I send a POST with `Content-Length: 1000`, then the
  handler runs only after the thousandth body byte, and `q.body()` reads
  from memory without parking.
- 2.1.3 As a client, when I send a chunked body, then the handler runs after
  the terminator and the body is dechunked in place.
- 2.1.4 As a client, when I send `Expect: 100-continue`, then the framer
  answers 100 after the head when the declared length is under the cap and
  413 when it is over, with no fiber assigned for either.
- 2.1.5 As a client, when I open a keep-alive connection and send nothing,
  then the server holds a socket and buffers and no fiber, for as long as
  the idle timeout allows.
- 2.1.6 As a client, when I pipeline three requests in one write, then the
  three handlers run in order on one fiber turn and the responses leave in
  order.
- 2.1.7 As a client, when I send a head larger than the input buffer, then I
  get 431 and the connection closes, with no fiber assigned.
- 2.1.8 As a client, when I stall mid-head past `headReadTimeoutMs`, then I
  get 408 and the connection closes, with no fiber assigned.

### 2.2 Streaming request bodies

With `streamRequestBody` on, or when the declared length exceeds
`maxBodyBytes` under a route that accepts streaming, the hold point is head
complete. The handler is assigned at that point and its body reads park on
the socket as they do today.

- 2.2.1 As a developer with streaming on, when a 2 GB upload arrives, then my
  handler starts after the head and reads the body incrementally; at no
  point does the server hold more than the input buffer.

### 2.3 Multipart

Per route, one of three modes. `whole` is the default: the body is held
under `maxBodyBytes` as any other body. `part`: the handler is assigned once
per completed part, in order, on the same connection turn, with
`MultipartParser` fed by the framer. `stream`: head complete, as 2.2.

- 2.3.1 As a developer in `whole` mode, when a 3 MB form with two files
  arrives, then my handler runs once with both parts in memory.
- 2.3.2 As a developer in `part` mode, when a form with five files arrives,
  then my part handler runs five times in order, each time after that part's
  closing boundary, and a part over `maxPartBytes` ends the request with 413.
- 2.3.3 As a developer in `stream` mode, when a 2 GB file part arrives, then
  my handler reads it incrementally as in 2.2.1.

### 2.4 HTTP/2 streams

The connection's reader role moves to the framer: frame decode, SETTINGS,
PING, WINDOW_UPDATE, flow control and HPACK stay where they are and run on
the framer. The hold point for a stream is END_STREAM, which the server
already waits for before `ready` is set. A stream with a streaming body
dispatches at the end of HEADERS as 2.2.

- 2.4.1 As a client, when I open 50 streams on one connection and complete
  10 of them, then 10 units dispatch, each on its own pooled fiber, and 40
  hold (§4.4).
- 2.4.2 As a client, when I exceed `SETTINGS_MAX_CONCURRENT_STREAMS`, then
  the excess is refused with REFUSED_STREAM on the framer, as today.
- 2.4.3 As a client over TLS with ALPN h2, then the handshake and record
  layer run on the framer and the first stream's handler runs on a pooled
  fiber.

### 2.5 WebSocket messages

The frame decoder and the protocol engine (`WsProtocol`, `WsReadAction`)
run on the framer. Pings are answered on the framer. The hold point is a
`MESSAGE` action: a complete text or binary message after fragment
reassembly, under `maxMessageLength`. `onConnect` runs on the first unit's
turn before the first message; `onClose` runs on a final turn.

- 2.5.1 As a developer, when a client sends three messages quickly, then
  `onMessage` runs three times in order on one fiber turn.
- 2.5.2 As a developer, when 10K clients are connected and silent, then the
  server holds 10K sockets and no handler fibers, and answers their pings.
- 2.5.3 As a developer, when a client sends a 100 MB message over a 64 MB
  limit, then the connection closes with MESSAGE_TOO_BIG on the framer and
  no fiber is assigned.
- 2.5.4 As a developer, when my `onMessage` parks on a database call, then
  the next message from that client waits for it, and messages from other
  clients proceed.

### 2.6 Custom protocols after an upgrade

A route that upgrades with `r.upgrade(#takeover)` keeps today's behavior: the
takeover owns the connection on its own fiber. A route that upgrades with a
`Protocol` (§5) is framed and dispatched like any other.

- 2.6.1 As a developer with a length-prefixed binary protocol, when I
  register a `Framer` and handlers by kind, then each complete unit runs its
  handler in order, and a unit over `maxUnit` closes the connection.

## 3. Framers

### 3.1 One framer fiber per carrier

The server spawns K framer fibers, K the runtime's carrier count by default
(`CAJETA_CARRIERS`, default min(cores, 4)), overridable on the builder. A
connection is assigned to one framer at accept and stays with it. The
runtime exposes the carrier count to cajeta code.

- 3.1.1 As an operator with `CAJETA_CARRIERS=8`, when the server starts, then
  it reports 8 framers.

### 3.2 What a framer does

Reads ready sockets into their connection buffers, runs TLS, runs the
protocol parser for that connection to its hold point, answers protocol
traffic that needs no handler (100-continue, 431, 413, 408, h2 control
frames, WebSocket pings and close handshakes), and enqueues a connection
that holds a complete unit. It never calls developer code, never allocates
per byte, and never waits on a single socket.

- 3.2.1 As an operator, when one client dribbles a head one byte per second,
  then every other connection on that framer is unaffected.

### 3.3 Resumable scanning

A parser invoked again with more bytes resumes where it stopped. The head
scanner records its scan offset; the chunked decoder, the multipart parser
and the frame decoders already do. A parser's cost per call is proportional
to new bytes.

- 3.3.1 As a test, when a 16 KB head arrives one byte at a time, then the
  scanner's total work is linear in 16 KB, asserted by a scan counter.

### 3.4 Multiplexed readiness (runtime)

Today a fiber parks on one fd (`awaitReadable(fd)`, EPOLLONESHOT). A framer
needs to wait on all of its sockets at once. The runtime gains a readiness
set: a fiber registers fds with interest against a set it owns, parks on the
set, and wakes with the list of ready fds and events. Linux first (epoll);
the kqueue and IOCP engines follow the same shape when they land.

- 3.4.1 As a framer, when any of my 5,000 sockets becomes readable, then I
  wake once with that fd, and no other fiber is woken.
- 3.4.2 As a framer, when I stop reading a socket (§4.3), then it is
  removed from my interest until I re-add it, and bytes wait in the kernel.

### 3.5 Time budget

Framer work per wake is measured. A parser call over a budget (default 1 ms)
is counted and logged; the harness fails on any count.

## 4. Dispatch and the pool

### 4.1 The unit is a connection with work

A framer enqueues a connection once when it transitions from no complete
unit to at least one. A pooled fiber takes the connection, drains its
complete units in order, each through the handler, and returns the
connection to the framer when the mailbox is empty. For HTTP/1.1, WebSocket
and custom protocols one connection is on at most one fiber at a time; for
h2 the unit is a stream (§4.4).

- 4.1.1 As a test, when a client pipelines 100 requests, then the pool
  records one dispatch and the handler runs 100 times.

### 4.2 Admission bounds in-flight work, not fibers

`maxInFlight` bounds the number of units dispatched and not yet returned: a
connection turn for HTTP/1.1, WebSocket and custom protocols, a stream for
h2. A fiber parked in a handler counts toward it; a unit held but not yet
dispatched does not. The default is 256.

- 4.2.1 As a developer with `maxInFlight(2)`, when three requests arrive and
  each handler sleeps 100 ms, then the third starts after the first
  returns and completes in about 200 ms.
- 4.2.2 As a developer, when 200 handlers are parked on a slow database and
  `maxInFlight` is 256, then new requests still dispatch.

### 4.3 Back-pressure

At `maxInFlight`, framers keep accepting and keep reading until a
connection's mailbox reaches `maxPendingUnits` (default 4) or its buffer is
full; then the framer removes that socket from its readiness set until the
connection returns. TCP pushes back on the peer. Accepts obey
`connectionLimits` as today; nothing here refuses a request for being over
the cap.

- 4.3.1 As a test at `maxInFlight(1)` with a 1 s handler, when ten clients
  each send ten requests, then no request is refused, all 100 complete, and
  the server's resident memory does not grow with the backlog.

### 4.4 Ordering per protocol

HTTP/1.1, WebSocket and custom protocols: units from one connection run in
order on one fiber at a time, because pipelined responses and message order
are those protocols' promises. h2: complete streams on one connection
dispatch independently by default, each a unit under `maxInFlight` and
bounded per connection by `SETTINGS_MAX_CONCURRENT_STREAMS`, because a
stream is one request and holding one stream behind another is the
head-of-line blocking h2 removes. This matches today's h2 server, which
spawns a fiber per stream behind a send gate. `serializedStreams(true)` opts
a server into one stream at a time per connection.

- 4.4.1 As a client, when I complete streams 1 and 3 and stream 3's handler
  is fast, then stream 3's response may leave first, and the frames of both
  interleave correctly on the wire.
- 4.4.2 As a developer with `serializedStreams(true)`, when the same happens,
  then stream 3's handler starts after stream 1's returns.
- 4.4.3 As a test, when one client completes 100 streams at once against a
  pool at `maxInFlight(256)`, then at most 100 of its streams are in flight
  and other connections still dispatch into the remaining 156.

### 4.5 What a handler sees

The same `HttpRequest` and `HttpResponse`. A body already held is read from
memory without parking. Writes park on the socket as today. A handler that
calls a remote service parks inside the client; the carrier runs other
fibers. `await` is unchanged and is for fan-out only.

### 4.6 The fiber-per-connection model

`fiberPerConnection()` keeps today's behavior exactly: one fiber per
accepted socket, the connection's own loop, h2 spawning per stream,
`WsUpgrade.run` on the connection fiber. It is the model for a small number
of long, busy connections.

## 5. Developer-defined framing

### 5.1 The Framer contract

```cajeta
public interface Framer {
    /** Length of one complete unit at buf[start, end), or 0 for more bytes.
        Throws ProtocolViolationException for a malformed stream. */
    int32 unit(int8[] buf, int32 start, int32 end);
    /** The unit's kind, used to select its handler. */
    int32 kind(int8[] buf, int32 start, int32 length);
    /** How far `unit` scanned when it returned 0, so the next call starts
        there. Default: `start`. */
    int32 scanned();
}
```

Pure, allocation-free, non-blocking. Runs on the framer under the time
budget of §3.5.

### 5.2 Protocol registration

```cajeta
route.upgrade(Protocol.over(heap Wire())
    .maxUnit(65536)
    .on(Msg.SUBSCRIBE, (c, u) -> Feed.subscribe(c, u))
    .on(Msg.PUBLISH,   (c, u) -> Feed.publish(c, u))
    .onClose((c, code) -> Feed.drop(c)));
```

- 5.2.1 As a developer, when a unit arrives whose kind has no handler, then
  the connection closes with a protocol error and the event is counted.
- 5.2.2 As a developer, when `unit` throws, then the connection closes and
  nothing was dispatched.
- 5.2.3 As a developer, when a peer sends 1 MB with no unit boundary under
  `maxUnit(65536)`, then the connection closes once the buffer passes the
  cap.
- 5.2.4 As a developer, when a client sends two units in one TCP segment,
  then both handlers run, in order, on one turn.

### 5.3 WebSocket sub-framing

`Protocol.overWebSocket(framer)` applies a `Framer` to the payloads of
WebSocket binary messages, for applications that pack several application
units into one message.

## 6. Builder surface and defaults

| Call | Meaning |
|---|---|
| default | the request-dispatch pool |
| `fiberPerConnection()` | today's model, explicit |
| `maxInFlight(n)` | §4.2, default 256 |
| `maxPendingUnits(n)` | §4.3, default 4 |
| `framers(k)` | §3.1, default the carrier count |
| `serializedStreams(b)` | §4.4, default false |
| `multipart(route, mode)` | §2.3 |

`sharedPool(n)` is removed from `HttpServerBuilder` and `model(ServerModel)`
no longer accepts the stdlib shared-pool kind: a server that passed either
fails to compile with a diagnostic naming `maxInFlight`. The stdlib
`ServerModel.sharedPool` itself is untouched; the HTTP server simply does
not take it.

- 6.1 As a developer, when I build with no model call, then the server
  reports the pool with `maxInFlight` 256, `maxPendingUnits` 4 and K
  framers.
- 6.2 As a developer who wrote `sharedPool(8)`, when I rebuild, then the
  compile fails naming `maxInFlight`, and the migration guide explains that
  the old call meant eight workers owning connections.

## 7. Observability and limits

Counters on `HttpServer`: connections, idle connections, units pending,
in-flight, dispatches, framer wakes, framer budget overruns, sockets
paused for back-pressure, stack high-water per pool fiber. `ServerLimits`
and `ConnectionLimits` apply unchanged. Timeouts are measured by the framer.

- 7.1 As an operator, when I read the counters under a 10K idle load, then
  in-flight is 0 and idle connections is 10K.

## 8. Measurement and acceptance

No load tool is installed on Phoenix and `cajeta_harness` is unbuilt, so the
instrument is a cajeta load client in cajeta-http's tools: it opens N idle
keep-alive connections, keeps M requests in flight with a configurable
handler delay, and reports server RSS, fiber count, mapping count and
latency percentiles.

- 8.1 10K idle connections plus 1K in flight on Phoenix: fibers equal
  framers plus in-flight; RSS per idle connection reported; p99 latency
  reported against the fiber-per-connection model on the same box.
- 8.2 100K idle connections: attempted with `vm.max_map_count` raised; the
  result and the wall it hit, if any, recorded in the plan.
- 8.3 The 1.2.8.4 scripted h2 connection still allocates 0 bytes through
  the write side, and its per-request figure is reported under both models.
- 8.4 The existing cajeta-http suite passes under both models, including
  autobahn for WebSocket.

## 9. Compatibility

- `WsUpgrade.run` and `Takeover` remain for the fiber-per-connection model
  and for takeovers under the pool.
- primavera and the samples need no source change; they pick up the new
  default.
- The migration guide documents the removal of `sharedPool` and the new
  default.

## 10. Open questions

- 10.1 Closed 2026-10-16: h2 streams dispatch in parallel by default (§4.4).
- 10.2 Should the 100-continue answer and 413 refusal remain on the framer
  for routes with custom `Expect` handling? Recommendation: yes; a route
  that needs otherwise uses streaming mode.
- 10.3 The time budget value and whether it is per call or per wake.
