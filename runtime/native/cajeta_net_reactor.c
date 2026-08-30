// cajeta.io.net.reactor — NET-3.1 native reactor engine (epoll / kqueue / IOCP).
//
// This translation unit is **#included once** at the bottom of
// `cajeta_runtime.c` — the same single-TU → LLVM-bitcode → embed build path
// the NET-1.1 socket intrinsics ride (see the header of `cajeta_net_socket.c`).
// It MUST be #included **after** the R9.4 I/O-reactor block in
// `cajeta_runtime.c` (the `__cajeta_io_wait` / `__cajeta_reactor_*` machinery)
// AND after `cajeta_net_socket.c`, because on Linux it *delegates* to that
// pre-existing epoll engine rather than standing up a second one, and it reuses
// `cajeta_net_socket.c`'s `cajeta_net_from_fd` narrowing helper. Those symbols
// are file-static in the single TU, so keeping this file separate is a
// *review* boundary, not a *compilation* boundary — no CMake change to the
// bitcode-embed path is required (only the one `#include` line listed in this
// item's `registration`).
//
// ## Scope of NET-3.1 (per plan/cajeta-net-plan.md and docs/Net.md §The
// reactor)
//
// NET-3.1 names a fixed five-intrinsic ABI the `cajeta.io.net` socket types lower
// their await points to:
//
//   __cajeta_net_reactor_init()                      — lazy-create the OS event
//                                                       handle + spawn the
//                                                       reactor thread.
//   __cajeta_net_reactor_register(fd, interest, h)   — arm `(fd, interest)` for
//                                                       fiber-handle `h`.
//   __cajeta_net_reactor_deregister(fd)              — drop any armed interest.
//   __cajeta_net_await_readable(fd)                  — park the current fiber
//                                                       until `fd` is readable.
//   __cajeta_net_await_writable(fd)                  — …until `fd` is writable.
//
// ## What lands here (sound) vs. what is explicitly deferred (honest)
//
// **Lands, compiling + tested:**
//   1. The five named intrinsics as the stable public ABI — the symbols the
//      NET-3.3 async socket ops and the `Reactor.cajeta` surface bind to.
//   2. On **Linux**, `await_readable` / `await_writable` *delegate to the
//      existing R9.4 epoll engine* (`__cajeta_io_wait`), so the fiber-park /
//      carrier-wake integration that engine already proved is reused verbatim
//      — no duplicate epoll fd, no second reactor thread. `register` /
//      `deregister` are no-ops there because `__cajeta_io_wait` arms+parks in
//      one call with `EPOLLONESHOT` (auto-disarm), so a separate persistent
//      registration table would be dead weight under the v1 one-shot model.
//   3. A **portable, deterministic readiness probe** —
//      `__cajeta_net_reactor_poll_fd(fd, interest, timeout_ms)` — built on
//      `select()`, which works identically on a POSIX fd and a Winsock SOCKET
//      and yields the **readiness** semantics Net.md promises across all three
//      OSes. It is the body the **non-fiber** caller path of the await
//      intrinsics uses today (mirroring `__cajeta_io_wait`'s own non-fiber
//      fallback), and the piece the golden-vector gtest pins over a real
//      loopback socket pair WITHOUT needing a live carrier/scheduler.
//
// **Deferred (NOT written as speculative bodies):**
//   - The **kqueue** (macOS/BSD) and **IOCP** (Windows) *reactor-thread*
//     engines that park a fiber and wake it from the carrier deque. The Linux
//     epoll engine already does this; porting the thread+wake machinery to
//     kqueue and to the IOCP **completion** model (post `WSARecv`/`AcceptEx`/
//     `ConnectEx`, drain via `GetQueuedCompletionStatus`) is a substantial
//     native subsystem tightly coupled with NET-3.3's overlapped ops, and is
//     only meaningfully testable against a running fiber scheduler. Until those
//     land, the fiber-park path on non-Linux falls through to the portable
//     `select()` probe (correct, but it blocks the *carrier* — acceptable for
//     bring-up, and the invariant the kqueue/IOCP engines will restore is
//     called out at the call site). The five-intrinsic ABI does not change when
//     they arrive.
//   - The self-pipe / `eventfd` / IOCP-post **wake-for-deregister-and-shutdown**
//     break is provided for the Linux engine by its existing 1-second
//     `epoll_wait` timeout (see the R9.4 block); a dedicated wake fd is a
//     refinement that belongs with NET-3.2 (reactor lifecycle) and is noted
//     there.
//
// The `interest` bitmask reuses the R9.4 `CAJETA_IO_READ` / `CAJETA_IO_WRITE`
// constants so there is one readiness vocabulary across the whole runtime.

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <sys/select.h>
#  include <sys/time.h>
#  include <sys/types.h>
#  include <errno.h>
#  include <unistd.h>
#endif

#include <stdint.h>

// Public readiness vocabulary — same bit values as the R9.4 reactor's
// CAJETA_IO_READ / CAJETA_IO_WRITE so `interest` is one contract runtime-wide.
// (Re-#defined defensively in case this TU is ever reviewed in isolation; the
// values MUST match cajeta_runtime.c's.)
#ifndef CAJETA_IO_READ
#  define CAJETA_IO_READ  1
#endif
#ifndef CAJETA_IO_WRITE
#  define CAJETA_IO_WRITE 2
#endif

// Return codes for the readiness probe (the await intrinsics narrow these to
// the cajeta surface's `1 = ready / 0 = timeout / -1 = error`).
#define CAJETA_REACTOR_READY    1
#define CAJETA_REACTOR_TIMEOUT  0
#define CAJETA_REACTOR_ERROR  (-1)

// NET-3.2 reactor-lifecycle hook (defined in cajeta_net_reactor_lifecycle.c,
// #included immediately after this TU in cajeta_runtime.c). Forward-declared so
// `__cajeta_net_reactor_init` below delegates its lazy-init (the `started`
// latch + shutdown wake pipe) to the lifecycle layer without an include-order
// constraint between the two files. The live-registration counter accounting
// already lives in this file (`cajeta_net_reactor_active`); NET-3.2's shutdown
// resets it via the `__cajeta_net_reactor_active_reset` hook below.
int32_t __cajeta_net_reactor_lifecycle_init(void);

// ---------------------------------------------------------------------------
// __cajeta_io_await_supported — can this platform wait on an ARBITRARY fd?
//
// `__cajeta_net_reactor_poll_fd` below is identical across POSIX fds and
// Winsock SOCKETs, which is exactly why it is the reactor's portable probe.
// It is NOT identical across POSIX fds and Windows HANDLES: Winsock select()
// accepts only SOCKETs, so a Windows console or pipe handle — stdin, the
// motivating case for `FileReader.awaitReadable` — cannot be polled by it at
// all. A correct Windows path needs PeekNamedPipe for pipes plus
// WaitForSingleObject for console handles, i.e. a native subsystem of its own,
// the same reason the kqueue and IOCP reactor engines are still deferred.
//
// So this reports the capability and `FileReader.awaitReadable` refuses up
// front where it is absent. Without it, a Windows caller would get the generic
// "bad descriptor" error from select() and reasonably conclude their fd was
// wrong, when the truth is that the platform cannot do this yet.
//
// Returns 1 when an arbitrary-fd readiness wait is supported, 0 otherwise.
// When the Win32 path lands, this returns 1 there and no caller changes.
// ---------------------------------------------------------------------------
int32_t __cajeta_io_await_supported(void) {
#if defined(_WIN32)
    return 0;
#else
    return 1;
#endif
}

// ---------------------------------------------------------------------------
// __cajeta_net_reactor_poll_fd — portable single-fd readiness probe.
//
// Blocks the *calling OS thread* until `fd` is ready for any bit in `interest`
// (CAJETA_IO_READ and/or CAJETA_IO_WRITE), or `timeout_ms` elapses. A negative
// `timeout_ms` blocks indefinitely. Returns:
//   CAJETA_REACTOR_READY   (1)  — fd became ready,
//   CAJETA_REACTOR_TIMEOUT (0)  — the timeout elapsed first,
//   CAJETA_REACTOR_ERROR  (-1)  — select failed (bad fd, etc.).
//
// Built on `select()` because it is the one readiness primitive whose API is
// byte-for-byte identical on POSIX fds and Winsock SOCKETs — exactly the
// cross-platform "readiness" surface Net.md's reactor table promises. This is
// the deterministic, scheduler-free body the native golden-vector test pins,
// and the non-fiber fallback the await intrinsics use everywhere plus the
// fiber path on platforms whose dedicated engine (kqueue/IOCP) hasn't landed.
//
// `select`'s fd_set is bounded by FD_SETSIZE on POSIX; for the single-fd probe
// that is never a constraint (one descriptor). On Winsock FD_SETSIZE bounds the
// *count* of sockets in a set, also a non-issue for one socket.
// ---------------------------------------------------------------------------
int32_t __cajeta_net_reactor_poll_fd(int32_t fd, int32_t interest,
                                     int32_t timeout_ms) {
    if (fd < 0) return CAJETA_REACTOR_ERROR;
    cajeta_native_socket_t s = cajeta_net_from_fd(fd);

#if !defined(_WIN32)
    // POSIX select cannot represent an fd >= FD_SETSIZE in an fd_set.
    if ((int) s >= FD_SETSIZE) return CAJETA_REACTOR_ERROR;
#endif

    fd_set rfds, wfds, efds;
    FD_ZERO(&rfds);
    FD_ZERO(&wfds);
    FD_ZERO(&efds);

    int want_read  = (interest & CAJETA_IO_READ)  ? 1 : 0;
    int want_write = (interest & CAJETA_IO_WRITE) ? 1 : 0;
    if (!want_read && !want_write) return CAJETA_REACTOR_ERROR;

    if (want_read)  FD_SET(s, &rfds);
    if (want_write) FD_SET(s, &wfds);
    // Always watch the exception set: a refused non-blocking connect surfaces
    // there on Winsock (and as writable+SO_ERROR on POSIX), so the caller's
    // post-readiness SO_ERROR check sees it either way.
    FD_SET(s, &efds);

    struct timeval tv;
    struct timeval* ptv = NULL;
    if (timeout_ms >= 0) {
        tv.tv_sec  = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        ptv = &tv;
    }

#if defined(_WIN32)
    // Winsock ignores the nfds argument; pass 0.
    int n = select(0, want_read ? &rfds : NULL, want_write ? &wfds : NULL,
                    &efds, ptv);
    if (n == SOCKET_ERROR) return CAJETA_REACTOR_ERROR;
#else
    int nfds = (int) s + 1;
    int n;
    do {
        n = select(nfds, want_read ? &rfds : NULL,
                   want_write ? &wfds : NULL, &efds, ptv);
    } while (n < 0 && errno == EINTR);
    if (n < 0) return CAJETA_REACTOR_ERROR;
#endif

    if (n == 0) return CAJETA_REACTOR_TIMEOUT;
    return CAJETA_REACTOR_READY;
}

// ---------------------------------------------------------------------------
// __cajeta_net_reactor_init — lazy-initialize the reactor engine.
//
// Idempotent. Returns 0 on success, -1 on failure. On Linux the actual epoll
// handle + reactor thread are created lazily by the R9.4 engine on the first
// `__cajeta_io_wait`, so init here only has to guarantee Winsock is up (so a
// `select` on a SOCKET works) and report readiness; the dedicated kqueue/IOCP
// engines, when they land (NET-3.2), do their handle creation here.
// ---------------------------------------------------------------------------
int32_t __cajeta_net_reactor_init(void) {
#if defined(_WIN32)
    // Reuse NET-1.1's idempotent WSAStartup guard so a reactor-first program
    // (no socket created yet) still has Winsock running before any select.
    if (!cajeta_net_ensure_init()) return -1;
#endif
    // NET-3.2 — lazy, idempotent lifecycle init (latches `started`, opens the
    // shutdown wake pipe on the first call). Must run after Winsock is up on
    // Windows so the wake pipe's loopback socket pair can be created.
    return __cajeta_net_reactor_lifecycle_init();
}

// ---------------------------------------------------------------------------
// __cajeta_net_reactor_register / _deregister — arm / drop interest for a
// fiber handle.
//
// Under the v1 one-shot model the Linux engine arms-and-parks atomically in
// `__cajeta_await_*` (EPOLLONESHOT auto-disarms on fire), and the portable
// `select` probe is likewise armed per-call, so there is no persistent
// per-fd registration table to maintain. These entry points exist so the
// NET-3.1 ABI is complete and stable for NET-3.3 / NET-3.4 (which call
// `_deregister` on the cancellation path); they are no-ops today and return 0.
// When the kqueue/IOCP persistent-registration engines land (NET-3.2/3.3),
// the bodies populate a per-fd waiter table here without changing the ABI.
//
// `fiber_handle` is the opaque `cajeta_fiber*` the caller is parking; it is
// accepted now (so the signature is final) and used by the persistent engines.
// ---------------------------------------------------------------------------
// Live-registration counter. Under the v1 one-shot model `register` /
// `deregister` keep no per-fd waiter table (the await intrinsic arms+parks
// atomically), but they DO maintain this single balance counter so the
// **registration-leak invariant** the NET-13.4 concurrency harness asserts —
// *"after a wave of awaitable ops settles, the live-registration count is back
// to zero"* — has a real, portable signal today, ahead of the kqueue/IOCP
// persistent-registration engines (NET-3.2/3.3) that will hang a full waiter
// table off these same entry points without changing the ABI or this counter's
// meaning. `_active_count` returns the current balance; a leak shows up as a
// non-zero residue once every parked op has been woken + deregistered.
//
// Single-threaded-reactor model (v1: one reactor thread per process) means the
// register/deregister calls for a given fd are serialized through the parking
// fiber's carrier; the counter is a plain int matching that discipline. (When
// the multi-carrier engines land, this becomes an atomic — noted at NET-3.2.)
static int32_t cajeta_net_reactor_active = 0;

int32_t __cajeta_net_reactor_register(int32_t fd, int32_t interest,
                                      void* fiber_handle) {
    (void) fd; (void) interest; (void) fiber_handle;
    cajeta_net_reactor_active++;
    return 0;
}

int32_t __cajeta_net_reactor_deregister(int32_t fd) {
    (void) fd;
    // Clamp at zero: the cancellation path (NET-3.4) may double-fire a
    // deregister (timeout race + completion both unwinding), and a never-armed
    // fd may be deregistered defensively; neither must drive the balance
    // negative and mask a real leak elsewhere.
    if (cajeta_net_reactor_active > 0) cajeta_net_reactor_active--;
    return 0;
}

// ---------------------------------------------------------------------------
// __cajeta_net_reactor_active_count — current live-registration balance.
//
// The number of `register` calls not yet matched by a `deregister`. The
// NET-13.4 harness pins this to zero after a settled wave of ops (no reactor
// leak) and reads it mid-flight to prove a parked op is in fact armed. Returns
// a snapshot; under the v1 single-reactor-thread model it is read on the same
// carrier that mutates it, so no fence is needed yet.
// ---------------------------------------------------------------------------
int32_t __cajeta_net_reactor_active_count(void) {
    return cajeta_net_reactor_active;
}

// ---------------------------------------------------------------------------
// __cajeta_net_reactor_active_reset — drain the live-registration balance to 0.
//
// NET-3.2 clean-shutdown hook: after the carriers + reactor thread have been
// joined at runtime teardown, no await can still be in flight, so the balance
// is forced back to zero (the "drain registrations" step). Idempotent. Kept
// here, next to the counter it owns, and called from
// `__cajeta_net_reactor_shutdown` in the lifecycle TU.
// ---------------------------------------------------------------------------
void __cajeta_net_reactor_active_reset(void) {
    cajeta_net_reactor_active = 0;
}

// ---------------------------------------------------------------------------
// __cajeta_net_await_readable / _await_writable — park until `fd` is ready.
//
// Returns CAJETA_REACTOR_READY (1) when the fd is ready, CAJETA_REACTOR_ERROR
// (-1) on failure. (A timeout-less await never returns TIMEOUT; the deadline
// composition is NET-3.4 via the timer wheel.)
//
// On **Linux** these delegate to the R9.4 `__cajeta_io_wait`, which:
//   - in a fiber: registers the fd with the shared epoll reactor and PARKS the
//     fiber (the carrier runs other fibers — the no-carrier-blocks invariant),
//     returning 1 when the reactor wakes it;
//   - off a fiber: does a direct blocking epoll_wait.
// So on Linux the fiber-park / carrier-wake path is reused verbatim — this is
// the integration NET-3.1 calls for, not a re-implementation.
//
// On **non-Linux** (until the kqueue/IOCP engines land) they fall through to
// the portable `select` probe. NOTE: that blocks the *carrier* thread, which
// the dedicated engines will fix; it is correct-but-not-yet-non-blocking and
// is called out here so a reader is not misled. The Cajeta surface and the
// NET-3.3 ops are written against this stable signature regardless.
// ---------------------------------------------------------------------------
// Does the native await block the CARRIER thread (rather than park the fiber)?
// 1 on the platforms whose dedicated fiber-park engine hasn't landed yet
// (Windows/macOS — the await below falls through to a blocking `select`), 0 on
// Linux (the epoll engine parks the fiber and frees the carrier). The Cajeta
// `Reactor.awaitReadable`/`awaitWritable` adapters read this to decide between
// the native fiber-park (Linux) and a cooperative **poll-and-park** loop
// (elsewhere): a non-blocking readiness probe interleaved with a timer-wheel
// fiber sleep, which yields the carrier between probes so a fiber awaiting
// socket I/O never holds its carrier hostage. Without that, two interdependent
// fibers (e.g. both halves of a TLS handshake over loopback) starve each other
// — one parks the only spare carrier in `select` while the peer it is waiting
// on sits un-runnable on that carrier's deque. This predicate keeps the Linux
// fast path while making the portable fallback correct under the fiber model.
int32_t __cajeta_net_await_carrier_blocking(void) {
#if defined(__linux__)
    return 0;
#else
    return 1;
#endif
}

int32_t __cajeta_net_await_readable(int32_t fd) {
#if defined(__linux__)
    // Reuse the proven epoll fiber-park engine.
    return __cajeta_io_wait(fd, CAJETA_IO_READ);
#else
    return __cajeta_net_reactor_poll_fd(fd, CAJETA_IO_READ, -1);
#endif
}

int32_t __cajeta_net_await_writable(int32_t fd) {
#if defined(__linux__)
    return __cajeta_io_wait(fd, CAJETA_IO_WRITE);
#else
    return __cajeta_net_reactor_poll_fd(fd, CAJETA_IO_WRITE, -1);
#endif
}

// Deadline-bounded twins (NET-3.4). On Linux these ride the fiber-parking
// __cajeta_io_wait_timed engine — the carrier stays free while the fiber
// waits, which is the whole point: the old path ran the blocking select()
// probe on the carrier thread, so a server head-read with a 30s budget
// starved every fiber co-hosted on that carrier (the cajeta-http loopback
// flake — the peer that would have sent the awaited bytes sat un-runnable
// on the blocked carrier's deque until the deadline dropped the
// connection). Non-fiber callers and non-Linux fall through to the
// blocking probe, whose thread-blocking semantics are correct there.
// Returns 1 READY / 0 TIMEOUT / -1 ERROR — the poll_fd contract.
int32_t __cajeta_net_await_readable_timed(int32_t fd, int32_t timeout_ms) {
#if defined(__linux__)
    return __cajeta_io_wait_timed(fd, CAJETA_IO_READ, timeout_ms);
#else
    return __cajeta_net_reactor_poll_fd(fd, CAJETA_IO_READ, timeout_ms);
#endif
}

int32_t __cajeta_net_await_writable_timed(int32_t fd, int32_t timeout_ms) {
#if defined(__linux__)
    return __cajeta_io_wait_timed(fd, CAJETA_IO_WRITE, timeout_ms);
#else
    return __cajeta_net_reactor_poll_fd(fd, CAJETA_IO_WRITE, timeout_ms);
#endif
}
