# pollable-stdin — waiting on an fd without blocking the carrier

## 1. Definition

### 1.1 Purpose
Expose a readiness wait on `cajeta.io.file.FileReader` so a fiber can
wait for an fd to become readable while other fibers keep running. The
motivating fd is stdin: a resident server that reads line-oriented
requests cannot notice a second request while the first is being
answered, because every read available today blocks.

### 1.2 The problem, measured
cabra plan 4.2.1 recorded the reader half of its serve loop as blocked
on two facts (2026-08-27):

- **(a)** "cajeta has no non-blocking stdin — only `cajeta.io.net`
  carries O_NONBLOCK".
- **(b)** cajeta has no OS threads, and cooperative cancellation only
  fires at a yield point, which a blocking `FileReader.read` is not — so
  a reader fiber parked in `read` cannot be cancelled and `shutdown`
  hangs at the join.

**(a) is inaccurate.** The runtime already carries a portable single-fd
readiness probe that accepts any POSIX fd:
`__cajeta_net_reactor_poll_fd` (`runtime/native/cajeta_net_reactor.c`),
built on `select()`, plus a cooperative `Reactor.pollPark(fd, interest)`
that yields rather than holding its carrier. Measured 2026-08-30:
`select()` on fd 0 of a pipe reports not-ready with no data pending and
ready once a byte arrives. The capability exists; it is **unexposed** —
every member of `Reactor` is package-private to `cajeta.io.net.reactor`.

**(b) is a consequence of (a), not an independent blocker.** A reader
that waits via poll-and-park never enters a blocking `read`, so there is
nothing to cancel: the park is itself a yield point.

The work is therefore an API decision, not a new runtime capability.

### 1.3 Constraints
- **1.3.1** The no-carrier-blocks invariant holds: a fiber awaiting an fd
  must never stall its carrier thread.
- **1.3.2** `FileReader(int32 fd)` is already public, so `FileReader(0)`
  is the existing spelling for stdin. No new type is needed to name it.
- **1.3.3** The reactor's existing platform split is reused as-is: Linux
  parks the fiber via epoll, other platforms run the portable `pollPark`
  loop. This spec adds no new platform engine.

### 1.4 Non-goals
- **1.4.1** Line buffering. Readiness means at least one byte, never a
  whole line; assembling lines across waits stays with the caller.
- **1.4.2** A Windows implementation (see §4).
- **1.4.3** Making `read` itself non-blocking, or changing any existing
  `FileReader` behaviour.
- **1.4.4** Exposing `Reactor` publicly.

## 2. The readiness wait

### 2.1 Requirements
A public method on `FileReader` that waits until the reader's fd is
readable, with a timeout, reporting which of the two happened.

### 2.2 Use cases
- **2.2.1** When an fd has data pending, the wait returns ready
  immediately.
- **2.2.2** When an fd has no data pending and data arrives before the
  timeout, the wait returns ready.
- **2.2.3** When an fd has no data pending and the timeout elapses
  first, the wait returns not-ready rather than throwing.
- **2.2.4** When a timeout of zero is given, the wait polls and returns
  without parking — the caller gets a pure probe.
- **2.2.5** When the peer closes the fd, the wait returns ready, because
  a subsequent read returns 0 to signal EOF.
- **2.2.6** When the wait is called on a closed or invalid fd, it
  reports an error distinguishable from a timeout.
- **2.2.7** When a fiber is waiting, other fibers continue to run.
- **2.2.8** When a fiber is waiting, it is at a yield point, so
  cooperative cancellation and a scope join both complete.

## 3. Fd kinds

### 3.1 Requirements
Readiness must mean the same thing to a caller regardless of what stdin
happens to be attached to. Linux `epoll_ctl` rejects a regular file with
`EPERM`, while `select()` reports a regular file as always ready. A
driver script redirected from a file (`serve < requests.jsonl`) is a
first-class use, so the difference cannot reach the caller.

### 3.2 Use cases
- **3.2.1** When the fd is a pipe, readiness follows the data.
- **3.2.2** When the fd is a terminal, readiness follows the data.
- **3.2.3** When the fd is a socket, readiness follows the data.
- **3.2.4** When the fd is a regular file, the wait returns ready
  immediately, matching `select()` semantics, and never reports the
  platform's `EPERM`.
- **3.2.5** When the fd kind cannot be determined, the wait falls back to
  the portable probe rather than failing.

## 4. Platforms

### 4.1 Requirements
Linux gets the fiber-parking path already built for sockets. Other POSIX
platforms get the portable cooperative probe. Windows is out of scope
for v1 and must say so plainly: Winsock `select()` accepts only SOCKETs,
and a console or pipe HANDLE is not one, so there is no correct
implementation short of a Win32 native path (`PeekNamedPipe` /
`WaitForSingleObject`) — the same reason the reactor's kqueue and IOCP
engines are still deferred.

### 4.2 Use cases
- **4.2.1** When running on Linux, a waiting fiber parks and its carrier
  runs other fibers.
- **4.2.2** When running on another POSIX platform, a waiting fiber
  yields cooperatively and its carrier is not held.
- **4.2.3** When running on Windows, the wait throws an unsupported
  error naming the platform and the reason.
- **4.2.4** When Windows support later lands, no caller's source
  changes — the signature is fixed now.

## 5. Documentation

### 5.1 Requirements
The method's contract is easy to misread in exactly one way: readiness
is bytes, not lines. That has to be stated where it is read.

### 5.2 Use cases
- **5.2.1** When a reader consults the API docs, the byte-not-line
  meaning of readiness is stated with the method.
- **5.2.2** When a reader consults the API docs, the Windows limitation
  is stated with the method rather than only in a spec.
- **5.2.3** When a caller needs a line, the docs show the incremental
  buffering shape rather than leaving it implied.

## 6. Consumers

### 6.1 Requirements
cabra 4.2.1's reader half is the first caller and the reason this
exists. It is not delivered by this spec, but this spec must be
sufficient for it.

### 6.2 Use cases
- **6.2.1** When this ships, cabra's reader fiber can wait for a request
  line while a decode is in flight, with no carrier stall.
- **6.2.2** When this ships, cabra's `{"op":"shutdown"}` drains and exits
  without hanging on a parked reader.
- **6.2.3** When this ships, cabra's N-client acceptance (4.3.1) can run
  from a pipe with real overlap.

## 7. Open questions
- **7.1** Whether `dev.cajeta.llm`'s or any other repo's existing
  hand-rolled stdin loops should migrate onto this. Not surveyed.
- **7.2** Whether a `FileWriter.awaitWritable` counterpart is worth
  shipping in the same change. No caller has asked for it yet.
