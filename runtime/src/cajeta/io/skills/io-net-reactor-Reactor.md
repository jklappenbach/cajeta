---
id: io-net-reactor-Reactor
applies-to: [cajeta/io/net/reactor/Reactor]
title: Reactor — internal async I/O engine; never named by application code
description: The epoll/kqueue/IOCP fiber-park engine under cajeta.io.net; do not call it — use socket *Async ops, which park on it for you.
---

# Reactor

**Do not write code against `Reactor`.** It is the **internal async I/O engine**
that lets a fiber wait for socket readiness without blocking its carrier thread.
Application code never names it. If you reached this skill looking for "how do I
do non-blocking / awaitable socket I/O," the answer is: **call the socket types'
`*Async` ops** — they drive `Reactor` underneath. This is a `package`-private
support class (`static` methods, no public surface beyond the `READ`/`WRITE`
interest constants), not an access point.

## What to use instead (the routing answer)

| You want… | Call this (not `Reactor`) |
|---|---|
| Non-blocking read | [`TcpStream.readAsync`](../net/TcpStream.cajeta) |
| Non-blocking write | [`TcpStream.writeAsync`](../net/TcpStream.cajeta) / `writeAllAsync` |
| Read bounded by a timeout | [`TcpStream.readWithin`](../net/TcpStream.cajeta) |
| Non-blocking connect | [`TcpStream.connectAsync`](../net/TcpStream.cajeta) |
| Non-blocking accept | [`TcpListener.acceptAsync`](../net/TcpListener.cajeta) |
| Cancel / deadline an await | wrap the op in `Tasks.withTimeout` ([`cajeta.concurrent.Tasks`](../../concurrent/Tasks.cajeta)) |

Each of those is a pure-cajeta readiness loop that, on a `WouldBlock`, parks the
current fiber via `Reactor.awaitReadable` / `awaitWritable` and retries when the
fd is ready — so **no carrier thread ever blocks on socket I/O**. You get that
for free by calling the `*Async` op; you never call `Reactor` to get it.

## Idiomatic example — what application code actually writes

```cajeta
import cajeta.io.net.TcpStream;
import cajeta.io.net.SocketAddress;
import cajeta.io.net.IpAddress;
import cajeta.concurrent.Tasks;

// Runs on a fiber. No mention of Reactor anywhere — the *Async ops own it.
TcpStream conn #= TcpStream.connectAsync(#SocketAddress(IpAddress.loopback(), 8080));

int8[] buf = heap int8[4096];
// readAsync parks this fiber on the reactor on a WouldBlock; the carrier runs
// other fibers meanwhile, and this fiber resumes when the socket is readable.
int64 n = conn.readAsync(buf, 0, 4096);

// Deadline/cancellation composes at the op level, not the reactor level:
// the sentinel unwinds through the await's finally and the reactor deregisters.
int64 m = Tasks.withTimeout(5000, spawn conn.readAsync(buf, 0, 4096));

conn.close();   // you own the connection's lifecycle; the reactor owns nothing of yours
```

## Ownership & lifecycle — nothing of yours crosses into the reactor

- `Reactor` takes **no ownership** of your buffers, sockets, or addresses. It
  only ever sees a raw `int32 fd` (borrowed for the duration of one await) and
  parks/wakes the *current* fiber. Buffer and connection ownership stay entirely
  with the socket op and you.
- **Lifecycle is automatic and process-global.** One reactor thread per process,
  lazily initialized on the first awaitable op. The runtime tears it down from
  `__cajeta_task_shutdown` at carrier-pool teardown. You do **not** create,
  init, or shut down the reactor — `init()` / `shutdown()` / `started()` exist
  only for the lifecycle tests' deterministic control.
- **Registrations are self-balancing.** Every await brackets its park in
  `register → try → finally deregister`, so completion, error, *and* the
  cancellation-sentinel unwind (from `Tasks.withTimeout`) all drop the
  registration — a cancelled or timed-out await never leaks. `activeCount()`
  returns to zero after a settled wave; it is a test invariant, not an app API.

## What `Reactor` does NOT do (so you don't go hunting)

- **No public/constructible surface.** There is no `Reactor` instance to make,
  no event-loop handle to pass around, no callback or `Future` registry to
  subscribe to. You cannot "get the reactor" — and you don't need to.
- **It is not a general event loop or scheduler.** It only translates socket-fd
  readiness into fiber wakeups. Timers, channels, and fiber scheduling live
  elsewhere (`cajeta.concurrent`); deadlines/cancellation are composed with
  `Tasks.withTimeout`, not configured on the reactor.
- **It is not the place to add a new awaitable op.** New async socket ops are
  written in the socket class as readiness loops over the lowered non-blocking
  syscall + `awaitReadable`/`awaitWritable`; you extend `TcpStream` /
  `TcpListener`, not `Reactor`.
- **Platform caveat (status):** the fiber-park path is live on Linux (epoll);
  macOS/BSD (kqueue) and Windows (IOCP) currently fall back to a portable
  cooperative poll-and-park that still yields the carrier. This is invisible at
  the `*Async` surface — another reason not to special-case the reactor.
