// cajeta.io.net — NET-1.7 non-blocking mode + WouldBlock-as-a-value intrinsics.
//
// This translation unit is **#included once** at the bottom of
// `cajeta_runtime.c` (the same single-TU → bitcode → embed build path the
// NET-1.1 socket intrinsics ride; see the header of `cajeta_net_socket.c`).
// It MUST be #included **after** `cajeta_net_socket.c` because it reuses that
// file's fd-ABI narrowing helpers (`cajeta_net_from_fd`,
// `CAJETA_SOCKET_ERROR`), the `cajeta_net_raw_errno()` reader, and the
// `CAJETA_NET_*` ordinal contract — those are file-static in the single TU, so
// keeping these helpers in a separate file is a *review* boundary, not a
// *compilation* boundary. No CMake change to the bitcode-embed path is needed.
//
// **Scope of NET-1.7** (per plan/cajeta-net-plan.md): "Non-blocking mode
// toggle on every socket type + `WouldBlock` surfaced as a distinct error
// (not an exception) so the reactor (NET-3) can drive readiness loops."
//
// NET-1.1 already shipped the *write* half of the toggle
// (`__cajeta_net_set_nonblocking`) and made `recv`/`send`/`sendto`/`recvfrom`/
// `accept`/`connect` return their failure sentinel while leaving the
// normalized WouldBlock ordinal readable via `__cajeta_net_last_error()`. What
// NET-1.7 adds is exactly the two pieces that turn that into a *value-based,
// non-throwing* contract the whole socket surface (and, later, the reactor)
// branches on:
//
//   __cajeta_net_get_nonblocking  — read the O_NONBLOCK / FIONBIO bit back, so
//                                   the cajeta `isNonBlocking()` getter and the
//                                   set→get round-trip test have a query to
//                                   pin against (NET-1.1 shipped only the
//                                   setter — there was no way to read it back).
//   __cajeta_net_is_wouldblock    — the non-throwing classifier: after any I/O
//                                   intrinsic returns its -1 sentinel, this
//                                   answers "was that a would-block, or a real
//                                   error?" as a 1/0 value WITHOUT constructing
//                                   an exception. This is the whole point of
//                                   NET-1.7: the hot readiness loop costs no
//                                   throw — the cajeta layer does
//                                   `if (n < 0 && Net.isWouldBlock()) return WOULD_BLOCK;`
//                                   and only falls through to
//                                   `NetErrors.fromErrno(...)` for a genuine
//                                   fault.
//
// Why a dedicated `is_wouldblock` rather than just comparing
// `__cajeta_net_last_error() == 1` in cajeta: (1) it keeps the WouldBlock
// ordinal a single source of truth in C (the cajeta side never hard-codes the
// "1"), and (2) it lets the classifier also fold in the platform's
// *connect-in-progress* spelling (EINPROGRESS / WSAEWOULDBLOCK) which, for a
// non-blocking `connect`, is the readiness equivalent of WouldBlock — the
// reactor waits for writability in exactly the same way. A separate
// `__cajeta_net_is_in_progress` exposes that distinctly for callers (the
// connect path) that want to tell a fresh in-flight connect from a retryable
// read.
//
// On Windows, getting the non-blocking bit back deserves a note:
// `ioctlsocket(FIONBIO)` is **write-only** — Winsock provides no query for the
// current blocking mode. So `__cajeta_net_get_nonblocking` maintains a tiny
// process-side shadow of the last value the cajeta layer *set* per fd, keyed
// in a small open-addressed table guarded by the existing pthread mutex
// machinery. POSIX has a real `fcntl(F_GETFL)` query and uses it directly (no
// shadow), so the shadow is a Windows-only fidelity shim, not the source of
// truth anywhere a real query exists.

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <fcntl.h>     // fcntl / F_GETFL / O_NONBLOCK
#  include <errno.h>
#endif

#include <stdint.h>
#include <pthread.h>

// ---------------------------------------------------------------------------
// __cajeta_net_is_wouldblock — non-throwing WouldBlock classifier.
//
// Call IMMEDIATELY after an I/O intrinsic (recv/send/sendto/recvfrom/accept)
// returned its -1 sentinel, on the same thread (errno / WSAGetLastError are
// thread-local and clobbered by the next syscall). Returns 1 iff the last
// socket error is EAGAIN/EWOULDBLOCK (POSIX) / WSAEWOULDBLOCK (Winsock), else
// 0. This is the value the cajeta read/recv/accept surface branches on to
// return its `WOULD_BLOCK` sentinel instead of throwing — the readiness loop's
// fast path. It reuses NET-1.1's `cajeta_net_raw_errno()` so there is exactly
// one place that reads the platform error.
// ---------------------------------------------------------------------------
int32_t __cajeta_net_is_wouldblock(void) {
    int e = cajeta_net_raw_errno();
#if defined(_WIN32)
    // WSAEWOULDBLOCK from a non-blocking connect means "in flight", not
    // "would block" — see cajeta_net_note_op in cajeta_net_socket.c.
    return (e == WSAEWOULDBLOCK && !cajeta_net_last_op_was_connect()) ? 1 : 0;
#else
    if (e == EAGAIN) return 1;
#  if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    if (e == EWOULDBLOCK) return 1;
#  endif
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// __cajeta_net_is_in_progress — non-throwing "connect in flight" classifier.
//
// A non-blocking `connect` returns -1 with the platform's "operation now in
// progress" code (POSIX EINPROGRESS; Winsock WSAEWOULDBLOCK — Winsock reuses
// the would-block code for an in-flight connect). That is NOT a failure: the
// reactor (Phase 3) then waits for the socket to become *writable* and checks
// SO_ERROR to learn whether the connect completed. This classifier lets the
// connect path tell that readiness case apart from a hard connect failure
// (ECONNREFUSED etc.) without a throw. Returns 1 iff in-progress, else 0.
// ---------------------------------------------------------------------------
int32_t __cajeta_net_is_in_progress(void) {
    int e = cajeta_net_raw_errno();
#if defined(_WIN32)
    // Winsock signals a non-blocking connect-in-flight as WSAEWOULDBLOCK; a
    // re-issued connect on an already-in-flight socket gives WSAEALREADY. The
    // WSAEWOULDBLOCK reading is only valid when the last intrinsic WAS the
    // connect — from recv/send/accept it means "would block" (see
    // cajeta_net_note_op), and reporting that as an in-flight connect is the
    // misclassification NetNonBlockingTests.wouldBlockClassifiedNotAsHardError
    // caught on Windows.
    if (e == WSAEWOULDBLOCK) return cajeta_net_last_op_was_connect() ? 1 : 0;
    return (e == WSAEINPROGRESS || e == WSAEALREADY) ? 1 : 0;
#else
    return (e == EINPROGRESS) ? 1 : 0;
#endif
}

// ---------------------------------------------------------------------------
// Windows-only shadow of the last non-blocking value the cajeta layer SET for
// each fd. Winsock's FIONBIO is write-only, so there is no kernel query for the
// current blocking mode; we remember what was last requested. The table is a
// small open-addressed map (linear probe) guarded by one mutex. It is a
// best-effort fidelity shim for `get_nonblocking` on Windows only — POSIX uses
// a real fcntl query and never touches this.
// ---------------------------------------------------------------------------
#if defined(_WIN32)
#  define CAJETA_NB_SHADOW_CAP 1024
static pthread_mutex_t g_cajeta_nb_lock = PTHREAD_MUTEX_INITIALIZER;
static int32_t g_cajeta_nb_fd[CAJETA_NB_SHADOW_CAP];    // fd, or -1 == empty
static int32_t g_cajeta_nb_val[CAJETA_NB_SHADOW_CAP];   // 0/1 non-blocking
static int      g_cajeta_nb_init = 0;

static void cajeta_nb_shadow_ensure_init_locked(void) {
    if (g_cajeta_nb_init) return;
    for (int i = 0; i < CAJETA_NB_SHADOW_CAP; ++i) g_cajeta_nb_fd[i] = -1;
    g_cajeta_nb_init = 1;
}

static unsigned cajeta_nb_slot(int32_t fd) {
    // fds are small kernel handles; a multiply-mix keeps the probe short.
    unsigned h = ((unsigned) fd) * 2654435761u;
    return h % CAJETA_NB_SHADOW_CAP;
}

// Record `val` (0/1) for `fd`. Overwrites an existing entry; inserts on miss.
static void cajeta_nb_shadow_put(int32_t fd, int32_t val) {
    pthread_mutex_lock(&g_cajeta_nb_lock);
    cajeta_nb_shadow_ensure_init_locked();
    unsigned start = cajeta_nb_slot(fd);
    for (unsigned i = 0; i < CAJETA_NB_SHADOW_CAP; ++i) {
        unsigned s = (start + i) % CAJETA_NB_SHADOW_CAP;
        if (g_cajeta_nb_fd[s] == fd || g_cajeta_nb_fd[s] == -1) {
            g_cajeta_nb_fd[s] = fd;
            g_cajeta_nb_val[s] = val ? 1 : 0;
            break;
        }
    }
    pthread_mutex_unlock(&g_cajeta_nb_lock);
}

// Look up `fd`. Returns 0/1 if known, or -1 if the cajeta layer never set the
// mode on this fd (default OS state is blocking, so a miss reads as blocking=0
// at the public entry point below).
static int32_t cajeta_nb_shadow_get(int32_t fd) {
    int32_t out = -1;
    pthread_mutex_lock(&g_cajeta_nb_lock);
    cajeta_nb_shadow_ensure_init_locked();
    unsigned start = cajeta_nb_slot(fd);
    for (unsigned i = 0; i < CAJETA_NB_SHADOW_CAP; ++i) {
        unsigned s = (start + i) % CAJETA_NB_SHADOW_CAP;
        if (g_cajeta_nb_fd[s] == fd) { out = g_cajeta_nb_val[s]; break; }
        if (g_cajeta_nb_fd[s] == -1) break;   // probe stops at first empty
    }
    pthread_mutex_unlock(&g_cajeta_nb_lock);
    return out;
}
#endif  // _WIN32

// ---------------------------------------------------------------------------
// __cajeta_net_set_nonblocking_tracked — the toggle the cajeta `setNonBlocking`
// surface lowers to. It delegates to NET-1.1's `__cajeta_net_set_nonblocking`
// for the real fcntl/ioctlsocket call, then (on Windows only) updates the
// shadow so a later `get_nonblocking` reports the right value. On POSIX it is a
// thin pass-through (the real fcntl query needs no shadow). Returns 0 / -1.
//
// Keeping the toggle here under a *tracked* name — rather than re-pointing the
// NET-1.1 setter — preserves NET-1.1's primitive unchanged (the reactor and
// the native tests call it directly) while giving the cajeta surface a setter
// whose effect is queryable on every platform.
// ---------------------------------------------------------------------------
int32_t __cajeta_net_set_nonblocking_tracked(int32_t fd, int32_t nonblocking) {
    int32_t r = __cajeta_net_set_nonblocking(fd, nonblocking);
#if defined(_WIN32)
    if (r == 0) cajeta_nb_shadow_put(fd, nonblocking ? 1 : 0);
#endif
    return r;
}

// ---------------------------------------------------------------------------
// __cajeta_net_get_nonblocking — read the non-blocking bit back. Returns 1 if
// the socket is in non-blocking mode, 0 if blocking, -1 on error / unknown.
//
// POSIX: a real `fcntl(F_GETFL)` query — authoritative regardless of who set
// the flag. Windows: FIONBIO is write-only, so we consult the shadow the
// tracked setter maintained; a miss (the cajeta layer never set the mode) reads
// as blocking (0), the OS default for a fresh socket.
// ---------------------------------------------------------------------------
int32_t __cajeta_net_get_nonblocking(int32_t fd) {
    if (fd < 0) return -1;
#if defined(_WIN32)
    int32_t v = cajeta_nb_shadow_get(fd);
    return (v < 0) ? 0 : v;   // unknown ⇒ OS default (blocking)
#else
    int flags = fcntl(cajeta_net_from_fd(fd), F_GETFL, 0);
    if (flags < 0) return -1;
    return (flags & O_NONBLOCK) ? 1 : 0;
#endif
}
