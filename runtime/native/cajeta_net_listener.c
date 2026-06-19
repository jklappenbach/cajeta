// cajeta.io.net — NET-1.4 native `TcpListener` support intrinsics.
//
// This translation unit is **#included once** at the bottom of
// `cajeta_runtime.c` (the same single-TU → bitcode → embed build path the
// NET-1.1 socket intrinsics ride; see the header of `cajeta_net_socket.c`).
// No CMake change to the bitcode-embed path is required.
//
// **Scope of NET-1.4** (per plan/cajeta-net-plan.md): a TCP *listening*
// socket — `bind` + `listen` + a blocking `accept() -> TcpStream`, plus
// `SO_REUSEADDR` / `SO_REUSEPORT` via the option surface and a
// `localAddress()` query.
//
// The verbs `socket` / `bind` / `listen` / `accept` / `setsockopt` /
// `close` are **already** provided by NET-1.1 (`cajeta_net_socket.c`); the
// `TcpListener` Cajeta surface lowers straight to those — NET-1.4 does NOT
// re-implement them. What NET-1.4 adds natively is the small set of helpers
// that NET-1.1 deliberately left out because they encode a *platform
// constant* that must not leak into the Cajeta layer (exactly as NET-1.1
// keeps `AF_*` behind the `sockaddr_pack` ordinal):
//
//   __cajeta_net_set_reuseaddr   — SO_REUSEADDR on/off (SOL_SOCKET lives here)
//   __cajeta_net_get_reuseaddr   — read SO_REUSEADDR back (for the option test)
//   __cajeta_net_set_reuseport   — SO_REUSEPORT on/off, with the Windows
//                                  "no such option" reality handled in C
//   __cajeta_net_getsockname     — the bound local address, for localAddress()
//
// Putting `SOL_SOCKET` / `SO_REUSEADDR` / `SO_REUSEPORT` here (rather than
// passing them as ints from Cajeta through the generic
// `__cajeta_net_setsockopt`) keeps every platform `#if` in C. The typed,
// general option surface (`setNoDelay`, buffer sizes, linger, …) is NET-1.6;
// it will systematize the constant table. NET-1.4 needs only these two
// reuse flags + getsockname, so it ships exactly them.
//
// No async here — blocking-mode listener (Phase 1). The reactor (Phase 3)
// drives `accept` in non-blocking mode via NET-1.1's
// `__cajeta_net_set_nonblocking` + `__cajeta_net_accept`.

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <netinet/in.h>
#  include <sys/socket.h>
#  include <errno.h>
#endif

#include <stdint.h>

// These helpers reuse the int32 ⇄ native-handle narrowing + the
// `cajeta_net_err` shim defined in cajeta_net_socket.c (which is #included
// earlier in cajeta_runtime.c, so `cajeta_net_from_fd`, `CAJETA_SOCKET_ERROR`
// and the `CAJETA_NET_*` ordinals are already in scope at the single-TU
// level). Keeping them in a separate file is a *review* boundary, not a
// *compilation* boundary.

// ---------------------------------------------------------------------------
// SO_REUSEADDR — let bind() succeed on an address still in TIME_WAIT (the
// canonical server-restart need) and, on Windows, share the port. `enable`
// != 0 turns it on. Returns 0 on success, -1 on failure (caller reads
// `__cajeta_net_last_error()`).
//
// SO_REUSEADDR semantics differ between POSIX and Windows, but the *intent*
// the cajeta `setReuseAddress` surface expresses — "I want to rebind a
// recently-used local address without EADDRINUSE" — is satisfied by
// SO_REUSEADDR on both, so one flag maps cleanly. (The stronger Windows
// SO_EXCLUSIVEADDRUSE is out of scope.)
// ---------------------------------------------------------------------------
int32_t __cajeta_net_set_reuseaddr(int32_t fd, int32_t enable) {
    if (fd < 0) return -1;
    int on = enable ? 1 : 0;
    int r = setsockopt(cajeta_net_from_fd(fd), SOL_SOCKET, SO_REUSEADDR,
#if defined(_WIN32)
                       (const char*) &on,
#else
                       &on,
#endif
                       (cajeta_socklen_t) sizeof(on));
    return r == CAJETA_SOCKET_ERROR ? -1 : 0;
}

// Read SO_REUSEADDR back. Returns 1 if enabled, 0 if disabled, -1 on a
// getsockopt error. Pins the `optionsSetAndGetBack` acceptance at the
// listener level (set → read-back identity).
int32_t __cajeta_net_get_reuseaddr(int32_t fd) {
    if (fd < 0) return -1;
    int val = 0;
    cajeta_socklen_t len = (cajeta_socklen_t) sizeof(val);
    int r = getsockopt(cajeta_net_from_fd(fd), SOL_SOCKET, SO_REUSEADDR,
#if defined(_WIN32)
                       (char*) &val,
#else
                       &val,
#endif
                       &len);
    if (r == CAJETA_SOCKET_ERROR) return -1;
    return val != 0 ? 1 : 0;
}

// ---------------------------------------------------------------------------
// SO_REUSEPORT — load-balanced multi-socket bind to the SAME port (Linux
// 3.9+, the BSDs, macOS). **Windows has no SO_REUSEPORT** (its SO_REUSEADDR
// already permits port sharing), so on Windows this is a definitional no-op
// that reports success — the cajeta `setReusePort(true)` call is honored as
// "best effort, satisfied by the platform's SO_REUSEADDR semantics" rather
// than raising on a platform that structurally lacks the flag.
//
// `enable` != 0 turns it on. Returns 0 on success / no-op, -1 on failure.
// ---------------------------------------------------------------------------
int32_t __cajeta_net_set_reuseport(int32_t fd, int32_t enable) {
    if (fd < 0) return -1;
#if defined(SO_REUSEPORT)
    int on = enable ? 1 : 0;
    int r = setsockopt(cajeta_net_from_fd(fd), SOL_SOCKET, SO_REUSEPORT,
                       &on, (cajeta_socklen_t) sizeof(on));
    return r == CAJETA_SOCKET_ERROR ? -1 : 0;
#else
    // No SO_REUSEPORT on this platform (Windows): success no-op.
    (void) enable;
    return 0;
#endif
}

// True (1) iff this platform actually has a distinct SO_REUSEPORT flag.
// Lets a test distinguish the real-flag path (POSIX) from the no-op path
// (Windows) without compiling platform `#if`s into the test itself.
int32_t __cajeta_net_has_reuseport(void) {
#if defined(SO_REUSEPORT)
    return 1;
#else
    return 0;
#endif
}

// ---------------------------------------------------------------------------
// getsockname — the local address a socket is bound to, written into
// `addr_out[0..*addrlen_inout)` (a caller-sized sockaddr buffer, normally
// `__cajeta_net_sockaddr_storage_size()` bytes), with `*addrlen_inout`
// updated to the actual length the cajeta layer then feeds to
// `__cajeta_net_sockaddr_unpack` to rebuild the `SocketAddress`.
//
// This is the query `TcpListener.localAddress()` (and, in NET-1.3,
// `TcpStream.localAddress()`) lowers to.
//
// NOTE: `__cajeta_net_getsockname` itself is defined once, by NET-1.3 in
// `cajeta_net_getname.c` (which also owns `__cajeta_net_getpeername`). Both
// files are textually #included into `cajeta_runtime.c`, so defining it here
// too is a duplicate-symbol error — `TcpListener.localAddress()` simply calls
// the NET-1.3 definition. NET-1.4 contributes only the listener-specific
// intrinsics above.
// ---------------------------------------------------------------------------
