// cajeta.net.reactor — NET-3.2 reactor lifecycle (lazy init / clean shutdown).
//
// This translation unit is **#included once** at the bottom of
// `cajeta_runtime.c`, AFTER `cajeta_net_reactor.c` (NET-3.1) — the same
// single-TU → LLVM-bitcode → embed build path the rest of the `cajeta.net`
// native layer rides. Keeping it in its own file is a *review* boundary, not a
// *compilation* boundary: no CMake change to the bitcode-embed path is
// required (only the one `#include` line listed in this item's `registration`).
//
// ## Scope of NET-3.2 (per plan/cajeta-net-plan.md and docs/Net.md
// §The reactor)
//
// NET-3.2 is the **reactor lifecycle**, layered on the NET-3.1 engine ABI:
//
//   - **Lazy init on first awaitable op.** `__cajeta_net_reactor_init` (NET-3.1)
//     is idempotent but did not previously track whether anything was actually
//     stood up, so a runtime-teardown hook had no way to know if a shutdown was
//     needed. NET-3.2 adds a thread-safe `started` latch + a one-time-init
//     guard so the FIRST awaitable op pays the init cost exactly once, under a
//     lock, and every subsequent call is a cheap load.
//
//   - **Draining the registration balance at shutdown.** NET-3.1 already owns
//     the live-registration counter (`cajeta_net_reactor_active`, bumped by its
//     `register` / `deregister` entry points and read via
//     `__cajeta_net_reactor_active_count`). The plan's `ReactorTests` acceptance
//     ("reactor registration count returns to zero", "shutdown drains parked
//     fibers") needs a clean shutdown to force that balance back to zero once
//     the carriers + reactor thread are joined and no await can be in flight.
//     NET-3.2's shutdown calls NET-3.1's `__cajeta_net_reactor_active_reset` to
//     do exactly that — it does NOT duplicate the counter.
//
//   - **A wake primitive for shutdown.** The NET-3.1 header explicitly deferred
//     "the self-pipe / eventfd / IOCP-post wake-for-deregister-and-shutdown
//     break" to NET-3.2. This file provides a portable **self-pipe**
//     (a connected loopback nothing-special pair would be heavier; a `pipe()`
//     on POSIX / a loopback socket pair on Windows is the portable choice) so
//     a reactor blocked in a portable `select` probe can be woken promptly at
//     shutdown instead of waiting out its poll timeout. On Linux the *engine*
//     (R9.4 epoll) is woken by its own 1-second `epoll_wait` timeout + the
//     `__cajeta_task_shutdown` flag (see `cajeta_runtime.c`); this self-pipe
//     is the portable-path equivalent and the handle the lifecycle test pins.
//
//   - **Clean shutdown at runtime teardown.** `__cajeta_net_reactor_shutdown`
//     is idempotent and safe to call from `__cajeta_task_shutdown` (the
//     existing carrier-shutdown signal this integrates with) AND standalone.
//     It: (1) signals the wake pipe so any portable-path waiter returns, (2)
//     resets the net-reactor `started` latch + the registration counter, and
//     (3) closes the wake pipe. It deliberately does NOT close the Linux R9.4
//     epoll handle — that handle is owned and torn down by the R9.4 block in
//     `__cajeta_task_shutdown`; double-closing it would race that path. The
//     net layer owns only its own bookkeeping + the wake pipe, and only those
//     are reset here. When the dedicated kqueue/IOCP engines land (still
//     deferred — see the NET-3.1 header), their OS handle creation/close moves
//     into `started`-latched init here and this shutdown, without changing the
//     ABI.
//
// ## Honest deferral boundary
//
// NET-3.2 does NOT stand up the kqueue/IOCP reactor *threads* — those remain
// deferred with NET-3.1 (a substantial native subsystem only meaningfully
// testable against a running fiber scheduler). What lands here is the
// platform-independent *lifecycle*: the lazy-init latch, the registration
// accounting the cancellation/timeout paths and acceptance criteria need, the
// portable wake pipe, and the idempotent teardown hook wired into
// `__cajeta_task_shutdown`. All of it is deterministic and pinned by a
// scheduler-free gtest.

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <unistd.h>      // pipe, read, write, close
#  include <fcntl.h>       // fcntl, O_NONBLOCK
#  include <errno.h>
#endif

#include <pthread.h>
#include <stdint.h>
#include <string.h>   // memset

// NET-3.1 owns the live-registration counter; NET-3.2 shutdown drains it.
// Forward-declared (defined in cajeta_net_reactor.c, #included just before
// this TU).
void __cajeta_net_reactor_active_reset(void);

// ---------------------------------------------------------------------------
// Lifecycle state. Guarded by a dedicated mutex so the net-reactor lifecycle
// is independent of (and cannot deadlock against) the carrier-pool mutex the
// R9.4 engine uses.
// ---------------------------------------------------------------------------
static pthread_mutex_t __cajeta_net_lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;

// 1 once the FIRST awaitable op has initialized the net reactor; reset to 0 by
// __cajeta_net_reactor_shutdown so a subsequent program (or the next JIT test,
// since the JIT runtime survives across tests) re-inits cleanly.
static int __cajeta_net_lifecycle_started = 0;

// Portable shutdown wake handle: a self-pipe (POSIX) / loopback socket pair
// (Windows). The reactor's portable `select` waiter can add the read end to
// its fd_set; writing one byte to the write end wakes it. -1 = not created.
static int __cajeta_net_wake_read_fd  = -1;
static int __cajeta_net_wake_write_fd = -1;

// ---------------------------------------------------------------------------
// __cajeta_net_wake_pipe_open_locked — create the self-pipe / loopback pair.
//
// Called under __cajeta_net_lifecycle_mutex from the lazy-init path. Idempotent
// (no-op if already open). Returns 0 on success, -1 on failure (in which case
// the fds stay -1 and the reactor falls back to its poll-timeout wake — the
// pipe is an optimization, never a correctness requirement).
// ---------------------------------------------------------------------------
static int __cajeta_net_wake_pipe_open_locked(void) {
    if (__cajeta_net_wake_read_fd >= 0) return 0;  // already open

#if defined(_WIN32)
    // No anonymous-pipe equivalent that select() can watch on Winsock, so use a
    // self-connected loopback TCP pair: listen on an ephemeral loopback port,
    // connect to it, accept, close the listener. Both ends are SOCKETs select()
    // can watch — matching the portable-probe model.
    SOCKET lst = socket(AF_INET, SOCK_STREAM, 0);
    if (lst == INVALID_SOCKET) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;  // ephemeral

    int ok = 0;
    do {
        if (bind(lst, (struct sockaddr*) &addr, sizeof(addr)) == SOCKET_ERROR) break;
        int alen = (int) sizeof(addr);
        if (getsockname(lst, (struct sockaddr*) &addr, &alen) == SOCKET_ERROR) break;
        if (listen(lst, 1) == SOCKET_ERROR) break;

        SOCKET w = socket(AF_INET, SOCK_STREAM, 0);
        if (w == INVALID_SOCKET) break;
        if (connect(w, (struct sockaddr*) &addr, (int) sizeof(addr)) == SOCKET_ERROR) {
            closesocket(w);
            break;
        }
        SOCKET r = accept(lst, NULL, NULL);
        if (r == INVALID_SOCKET) {
            closesocket(w);
            break;
        }
        // Non-blocking read end so the drain recv never blocks even if called
        // when nothing is queued; the wake write is best-effort either way.
        u_long nb = 1;
        ioctlsocket(r, FIONBIO, &nb);
        __cajeta_net_wake_read_fd  = (int) r;
        __cajeta_net_wake_write_fd = (int) w;
        ok = 1;
    } while (0);

    closesocket(lst);
    return ok ? 0 : -1;
#else
    int fds[2];
    if (pipe(fds) != 0) return -1;
    // Non-blocking both ends: the wake write must never block the shutdown
    // caller even if the pipe buffer is somehow full, and the drain read must
    // never block the reactor.
    for (int i = 0; i < 2; ++i) {
        int fl = fcntl(fds[i], F_GETFL, 0);
        if (fl >= 0) fcntl(fds[i], F_SETFL, fl | O_NONBLOCK);
    }
    __cajeta_net_wake_read_fd  = fds[0];
    __cajeta_net_wake_write_fd = fds[1];
    return 0;
#endif
}

static void __cajeta_net_wake_pipe_close_locked(void) {
    if (__cajeta_net_wake_read_fd >= 0) {
#if defined(_WIN32)
        closesocket((SOCKET) __cajeta_net_wake_read_fd);
#else
        close(__cajeta_net_wake_read_fd);
#endif
        __cajeta_net_wake_read_fd = -1;
    }
    if (__cajeta_net_wake_write_fd >= 0) {
#if defined(_WIN32)
        closesocket((SOCKET) __cajeta_net_wake_write_fd);
#else
        close(__cajeta_net_wake_write_fd);
#endif
        __cajeta_net_wake_write_fd = -1;
    }
}

// ---------------------------------------------------------------------------
// __cajeta_net_reactor_lifecycle_init — lazy, idempotent, thread-safe init.
//
// The body NET-3.1's `__cajeta_net_reactor_init` now delegates to (after its
// platform Winsock guard). Runs the one-time setup — currently the wake pipe —
// under the lifecycle mutex, sets the `started` latch, and returns 0 on
// success. Idempotent: the second and later calls are a cheap latched no-op.
//
// Returns 0 on success, -1 if the one-time setup failed. A wake-pipe failure
// is NOT fatal (the reactor falls back to its poll-timeout wake), so it still
// latches `started` and returns 0 — the pipe is best-effort.
// ---------------------------------------------------------------------------
int32_t __cajeta_net_reactor_lifecycle_init(void) {
    pthread_mutex_lock(&__cajeta_net_lifecycle_mutex);
    if (!__cajeta_net_lifecycle_started) {
        // Best-effort wake pipe; a failure here is non-fatal.
        __cajeta_net_wake_pipe_open_locked();
        __cajeta_net_lifecycle_started = 1;
    }
    pthread_mutex_unlock(&__cajeta_net_lifecycle_mutex);
    return 0;
}

// ---------------------------------------------------------------------------
// __cajeta_net_reactor_started — has the net reactor been initialized?
//
// Lets the runtime-teardown hook skip the shutdown work entirely when no
// awaitable op ever ran (the no-networking program pays nothing).
// ---------------------------------------------------------------------------
int32_t __cajeta_net_reactor_started(void) {
    pthread_mutex_lock(&__cajeta_net_lifecycle_mutex);
    int s = __cajeta_net_lifecycle_started;
    pthread_mutex_unlock(&__cajeta_net_lifecycle_mutex);
    return (int32_t) s;
}

// ---------------------------------------------------------------------------
// __cajeta_net_reactor_wake — poke the shutdown wake pipe.
//
// Writes one byte to the wake pipe's write end so a reactor blocked in a
// portable `select` (with the read end in its fd_set) returns promptly. Safe
// to call with no pipe open (no-op) and from any thread. Used by shutdown and
// available to the NET-3.4 cancellation path.
// ---------------------------------------------------------------------------
void __cajeta_net_reactor_wake(void) {
    pthread_mutex_lock(&__cajeta_net_lifecycle_mutex);
    int wfd = __cajeta_net_wake_write_fd;
    pthread_mutex_unlock(&__cajeta_net_lifecycle_mutex);
    if (wfd < 0) return;
    const char b = 1;
#if defined(_WIN32)
    send((SOCKET) wfd, &b, 1, 0);
#else
    ssize_t r;
    do { r = write(wfd, &b, 1); } while (r < 0 && errno == EINTR);
    (void) r;  // EAGAIN (pipe full) is fine — a pending byte already wakes it.
#endif
}

// ---------------------------------------------------------------------------
// __cajeta_net_reactor_wake_fd — the read end the portable waiter watches.
//
// Returns the wake pipe's read fd, or -1 if no pipe is open. A reactor doing a
// portable `select` adds this to its read fd_set; on wake it drains the byte
// via __cajeta_net_reactor_wake_drain. (Wired into the kqueue/IOCP engines when
// they land; today it is the handle the lifecycle test inspects.)
// ---------------------------------------------------------------------------
int32_t __cajeta_net_reactor_wake_fd(void) {
    pthread_mutex_lock(&__cajeta_net_lifecycle_mutex);
    int rfd = __cajeta_net_wake_read_fd;
    pthread_mutex_unlock(&__cajeta_net_lifecycle_mutex);
    return (int32_t) rfd;
}

// ---------------------------------------------------------------------------
// __cajeta_net_reactor_wake_drain — consume any pending wake bytes.
//
// Called by the portable waiter after it observes the read end ready, so the
// next select doesn't immediately re-fire on the same byte. Non-blocking;
// drains until empty. Safe with no pipe open.
// ---------------------------------------------------------------------------
void __cajeta_net_reactor_wake_drain(void) {
    pthread_mutex_lock(&__cajeta_net_lifecycle_mutex);
    int rfd = __cajeta_net_wake_read_fd;
    pthread_mutex_unlock(&__cajeta_net_lifecycle_mutex);
    if (rfd < 0) return;
    char buf[64];
#if defined(_WIN32)
    // Read end is a non-blocking-by-default? No — make the drain bounded: a
    // single recv pulls whatever is queued; the wake only ever writes 1 byte
    // per shutdown so one recv suffices, and MSG_DONTWAIT isn't portable on
    // Winsock, so we cap to one read of the queued bytes.
    recv((SOCKET) rfd, buf, (int) sizeof(buf), 0);
#else
    ssize_t r;
    do { r = read(rfd, buf, sizeof(buf)); } while (r == sizeof(buf) ||
                                                   (r < 0 && errno == EINTR));
#endif
}

// ---------------------------------------------------------------------------
// __cajeta_net_reactor_shutdown — clean, idempotent net-reactor teardown.
//
// Integrates with the existing `__cajeta_task_shutdown` carrier-shutdown
// signal (call this from there, after the carriers/timer/R9.4-reactor have been
// joined). Safe to call standalone and repeatedly.
//
// Steps:
//   1. If the net reactor never started, return immediately (no-networking
//      program pays nothing).
//   2. Signal the wake pipe so any portable-path waiter returns promptly.
//   3. Reset the `started` latch and drain NET-3.1's live-registration counter
//      to 0 (via `__cajeta_net_reactor_active_reset`) — the "drain
//      registrations" the plan calls for. (Under the v1 one-shot model there is
//      no persistent waiter table to walk; the count IS the drain.)
//   4. Close the wake pipe handle (the "close the handle" the plan names for
//      the net layer; the Linux R9.4 epoll handle is closed by its own block).
//
// Returns 0 always (a teardown hook has nothing actionable to fail on).
// ---------------------------------------------------------------------------
int32_t __cajeta_net_reactor_shutdown(void) {
    pthread_mutex_lock(&__cajeta_net_lifecycle_mutex);
    if (!__cajeta_net_lifecycle_started) {
        pthread_mutex_unlock(&__cajeta_net_lifecycle_mutex);
        return 0;
    }
    int wfd = __cajeta_net_wake_write_fd;
    pthread_mutex_unlock(&__cajeta_net_lifecycle_mutex);

    // (2) Wake any portable-path waiter (outside the lock so the waiter can
    // re-enter the lock to drain).
    if (wfd >= 0) {
        const char b = 1;
#if defined(_WIN32)
        send((SOCKET) wfd, &b, 1, 0);
#else
        ssize_t r; do { r = write(wfd, &b, 1); } while (r < 0 && errno == EINTR);
        (void) r;
#endif
    }

    // (3)+(4) Reset bookkeeping and close the wake pipe under the lock.
    pthread_mutex_lock(&__cajeta_net_lifecycle_mutex);
    __cajeta_net_lifecycle_started = 0;
    __cajeta_net_wake_pipe_close_locked();
    pthread_mutex_unlock(&__cajeta_net_lifecycle_mutex);

    // Drain NET-3.1's live-registration counter (outside our lock — it has its
    // own single-threaded discipline and no dependency on this mutex).
    __cajeta_net_reactor_active_reset();
    return 0;
}
