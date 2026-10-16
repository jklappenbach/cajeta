# primavera-live-sessions — issued sessions, WebSocket lifecycle hooks, byte-bounded sends

Status: **active**, approved by Julian 2026-10-16. Author: Proton - Cajeta; reviewer: Phoenix - Cajeta
(review applied 2026-10-16); approver: Julian. Repo of record: cajeta (ecosystem spec); the work lands in
cajeta-primavera. Origin: the 2026-10-16 review of cabra unit 12 against
primavera 0.3.0, and Julian's decisions that day.

## 1. Definition

### 1.1 Purpose

cabra serves a resident model to browsers over WebSocket. A conversation is a
session that outlives any one connection: a client that reconnects names its
session and finds its prefix cache warm (cabra spec §5.2). cabra built that
itself (`SessionRegistry`, its own accept loop, its own send ring) because
primavera could not express it. This spec adds what primavera lacks, so cabra
can retire its framework code and run on primavera (cabra spec §11).

What it adds:
- sessions the store issues and can account for after they end;
- open and close hooks on a WebSocket path;
- a send queue bounded in bytes, so a token stream is not closed after a
  fraction of a second of client stall;
- a static directory on the same server, for cabra's web client.

### 1.2 What exists (primavera 0.3.0)

- `SessionStore` / `SessionScope`: id-keyed sessions with idle and absolute
  TTLs, lazy expiry on resolve, an `evictExpired` sweep, `invalidate`,
  `invalidateAll`, a `TimeSource` seam. The caller supplies the id;
  `create` on an existing id displaces it. A lookup answers live or absent.
- `WebSocketEndpoints`: JSON message handlers per path; each `Connection`
  has a `Connection` scope, a `Message` scope per message, a registry entry,
  and a writer fiber draining a queue of at most 64 messages. A send that
  finds the queue full closes the connection with 1008.
- `ConnectionRegistry.send(id, text)` queues for a connection by id and
  reports false when it is not open.

### 1.3 Non-goals

- `@Rest` annotation routing. cabra uses typed `Endpoints` (Julian,
  2026-10-16).
- Request scope that follows a turn across fibers. cabra passes an explicit
  turn object to the fibers it starts (Julian, 2026-10-16).
- Changes to cajeta-http. The hooks sit in primavera, over `WsUpgrade`.
- Persistent or distributed sessions; sessions are process memory.
- Addressing connections by principal.
- Engine-side session multiplexing (the draft `llm-serving` spec). cabra keeps
  its sessions behind one interface so that could replace them later.

### 1.4 Constraints

- Builds on cajeta-http 0.6.0 and toolchain v0.42.1, primavera's current pins.
- Independent of request-dispatch: nothing here waits on it, and it ships
  first (Julian, 2026-10-16).
- Existing primavera applications keep their behaviour unless they opt in,
  except §4.1: `queueBound(n)` in messages gives way to a byte bound.

### 1.5 Systems

cajeta-primavera (`context/`, `web/`), cajeta-http 0.6.0 (`ws.WsUpgrade`,
`middleware.StaticFile`), the stdlib `cajeta.hash.SecureRandom`, Olla for the
release.

## 2. Issued sessions

- **2.1** When an application opens a session without naming an id, the store
  issues one and returns it. The id is drawn from `SecureRandom` and cannot be
  guessed from other ids. `create(id)` with a caller-supplied id stays.
- **2.2** When a lookup names an id, the answer is one of three: **open**,
  **gone** (this store issued it and it has since been closed, expired or
  invalidated), or **unknown** (this store never issued it: another host, or
  this host before a restart). cabra tells its client which (cabra §5.2.5).
- **2.3** When many sessions have come and gone, the gone/unknown answer
  stays correct without the store keeping every ended id; memory for ended
  sessions does not grow with the number ever issued.
- **2.3.1** When a client presents an id the store did not issue, including
  one forged to look like an earlier id, the answer is unknown, never gone.
- **2.3.2** When an open is refused (§2.5), it consumes nothing that a later
  lookup could read as a gone session.
- **2.4** When a lookup answers open, it refreshes the idle clock, as today.
  A gone or unknown answer never creates a session.
- **2.5** When the store has a capacity and holds that many live sessions,
  opening another is refused with a result the caller can tell apart from
  success, and no live session is displaced. Expired sessions not yet swept
  do not count against capacity. `create(id)` on an id that is live is a
  displacement: the count does not grow, so it is allowed at capacity.
- **2.6** When fibers open sessions concurrently at the capacity, no more than
  the capacity succeed.
- **2.7** When a session leaves the store for any reason, the application's
  eviction observer is called once with its id and the reason: **closed**
  (the application ended it: `invalidate` of that id, or `invalidateAll`),
  **expired** (a TTL passed), or **displaced** (`create` on its id). The
  observer runs before the session's `Session` components drop, so it can
  read them; they drop after it returns, as today. cabra uses the observer
  to return the KV slot to the pool (cabra §5.2.3, §5.2.4).
- **2.7.1** When different sessions leave the store at once, their observers
  may run concurrently; an observer guards any state it shares across
  sessions with its own lock.
- **2.8** When a server runs with an idle TTL set, a sweep fiber the server
  starts at `serve` and stops at shutdown removes expired sessions on an
  interval, so a client that vanished frees its session (cabra §5.2.4).
  Without a server, `evictExpired` works as today.
- **2.9** When a WebSocket message handler attaches a session by id, that
  session's components resolve for that message. One connection may attach
  different sessions on different messages (cabra §5.2.6). A connection
  closing ends no session (cabra §5.2.2).

## 3. WebSocket connection lifecycle

- **3.1** When a connection on a path opens, the path's open handler runs
  with the `Connection`, inside its `Connection` scope, once the connection is
  in the registry and before any message handler.
- **3.2** When a connection closes for any reason (peer close, a handler's
  `CloseException`, a handler failure, send overflow, server shutdown, a
  transport error), the path's close handler runs exactly once with the close
  code, inside the `Connection` scope, before that scope's components drop.
  It runs on the connection's reader fiber after the writer fiber has
  stopped. Server shutdown closes each connection with 1001, and the handler
  sees that code.
- **3.3** When the close handler has started, a send to that connection
  reports not open and queues nothing.
- **3.4** When the open handler throws, the connection closes as a message
  handler's throw would (1011, or the `CloseException` code), and the close
  handler still runs. A throw from the close handler is logged and goes no
  further.
- **3.5** When a path declares no hooks, it behaves as in 0.3.0.

## 4. Send queue bounded in bytes

Julian, 2026-10-16: keep data buffered and send it asynchronously while a
client is slow; bound the buffer; once it is full, the next send closes.

- **4.1** When a connection's queue is bounded, the bound is in bytes: each
  queued message counts its payload plus a 4-byte length, WebSocket framing
  excluded, configurable per server. The bound is then the queue's memory. It replaces the message-count bound (Julian,
  2026-10-16: bytes manage memory more precisely), so a connection's send
  memory is known whatever the message sizes. The default is set at plan time
  (§7.2).
- **4.2** When the queue is under its bound, a send never parks the caller.
  The writer fiber sends in order as the client reads, so a slow client delays
  only its own stream.
- **4.2.1** When a single message is larger than the whole bound, the send
  is refused and the connection closes as for any overflow (§4.3).
- **4.3** When a send would take the queue past its bound, the send reports
  that it was not queued, and the connection closes with the overflow code
  (1008 unless set). The close handler runs with that code (§3.2). Sessions
  are untouched, so the client reconnects and resumes by id.
- **4.4** When an operator sizes the bound, the registry reports the peak
  bytes queued on any connection, in place of the peak message count.

## 5. Static directory

- **5.1** When an application mounts a directory at a path prefix, GET and
  HEAD under it serve files with content types by extension, the prefix itself
  serves `index.html`, and a missing file answers 404. A path that escapes the
  directory, lexically or through a symlink that resolves outside it,
  answers 403 (cajeta-http `StaticFile`'s jail and realpath check). cabra
  mounts `web/dist` at `/` (cabra §10.1).
- **5.1.1** When a request under a mount uses a method other than GET or
  HEAD and no declared route takes it, it answers 405. (`StaticFile` passes
  such requests on; primavera answers them.)
- **5.2** When a server has a static mount, endpoints, and WebSocket paths,
  they share one port; a declared route wins over a file of the same path.
- **5.3** When security rules are configured, a static mount is a route like
  any other and is subject to the same default-closed policy.

## 6. Release

- **6.1** When this work is complete, it ships as primavera 0.4.0 on
  cajeta-http 0.6.0 and toolchain v0.42.1, published to Olla, ahead of
  request-dispatch.
- **6.2** When 0.4.0 is published, cabra pins it (cabra plan 12.2.1).
- **6.3** Migration: `WebSocketEndpoints.queueBound(n)` (messages) is
  replaced by a byte bound (§4.1). The release notes say so, and
  `peakQueued()` gives way to the peak in bytes (§4.4).

## 7. Decisions (resolved 2026-10-16)

- **7.1** *(agreed with Phoenix 2026-10-16)* How ids carry gone versus
  unknown (§2.2, §2.3). An id is a per-store epoch, a serial, and a keyed
  MAC over (epoch, serial) under a per-store `SecureRandom` key
  (`HmacSha256` truncated, or `SipHash`; both are in the stdlib). Gone: the
  MAC verifies, the epoch is this store's, the serial is below the next one,
  and it is not live. Unknown: anything else. A random suffix alone could
  not be checked without tombstones, so a forged id with a plausible serial
  would read as gone. Serials advance only on a successful open (§2.3.2).
- **7.2** *(Julian 2026-10-16)* The default byte bound is 1 MiB per
  connection. cabra sets its own.
- **7.3** *(Julian 2026-10-16)* The sweep interval when an idle TTL is set:
  a quarter of the TTL, at most 60 seconds, configurable; off when
  no TTL.
- **7.4** *(agreed with Phoenix 2026-10-16)* The eviction observer runs after
  the store lock is released, on the fiber that removed the session (a
  resolve, the sweep, `invalidate` or `create`), so it may call back into the
  store; see §2.7.1 for concurrency.

## 8. Acceptance (spec level)

- Each use case in §2–§5 has a primavera self-test, written first.
- primavera's existing self-tests pass unchanged, except `WsSelfTest`'s move
  from `queueBound(n)` to the byte bound (§6.3).
- §2.6 is shown by a test on real fibers: N fibers open at once against a
  store at capacity minus one, and exactly one succeeds. A serial test cannot
  show it.
- cabra's §5.2 session tests pass when its `SessionRegistry` is replaced by
  primavera's store (verified in cabra unit 12, not here).
