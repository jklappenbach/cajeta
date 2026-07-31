// cajeta.io.net — NET-1.1 native socket intrinsics (BSD sockets / Winsock).
//
// This translation unit is **#included once** at the bottom of
// `cajeta_runtime.c` (the runtime is built as a single TU → LLVM bitcode →
// embedded into the compiler, then linker-merged into every user module).
// Keeping the net intrinsics in their own file makes them reviewable and
// editable independently while still riding the existing single-TU build —
// no CMake change to the bitcode-embed path is required. The functions are
// plain `extern "C"`-ABI symbols (`__cajeta_net_*`) that the `cajeta.io.net`
// stdlib wrappers (NET-1.2 onward) lower their method bodies to, exactly the
// way `File` lowers to `__cajeta_file_*` and `Channel` to `Cajeta.lockNew`.
//
// Scope of NET-1.1 (per plan/cajeta-net-plan.md):
//   socket / bind / listen / accept / connect / send / recv / sendto /
//   recvfrom / close / shutdown / setsockopt / getsockopt / set_nonblocking,
//   plus the `cajeta_net_errno` shim that maps the platform error code
//   (POSIX `errno` / `WSAGetLastError`) to a stable cross-platform enum the
//   cajeta layer turns into a `NetException` subtype (NET-1.8). Address
//   marshalling (`__cajeta_net_sockaddr_pack/_unpack`) is NET-1.2 and is
//   intentionally NOT here.
//
// No async here — these are the blocking-mode primitives (Phase 1). The
// reactor (Phase 3) drives them in non-blocking mode via the same symbols
// plus `__cajeta_net_set_nonblocking`.
//
// **fd ABI.** The descriptor crosses the cajeta boundary as `int32`, the
// same width `File.fd` uses, so the intrinsic codegen addresses it
// identically across file + socket types. On POSIX a socket fd is already a
// small `int`. On Windows a `SOCKET` is an opaque unsigned handle that is, in
// practice, a small kernel-handle value that fits in 32 bits for every socket
// a process actually opens; we round-trip it through `int32` and re-widen on
// the way back into Winsock calls. `-1` is the universal "no socket"
// sentinel on both platforms (POSIX error return and the value
// `INVALID_SOCKET` narrows to under `int32`).

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
// MinGW links ws2_32 for the test/native object build; the JIT-embedded
// bitcode resolves these via the process symbol generator against the host's
// already-loaded ws2_32 (the build tool's Subprocess port + TestHttpServer
// already pull it in). The `#pragma comment` is MSVC-only and harmless to
// omit under MinGW where the linker flag is supplied by CMake.
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>   // TCP_NODELAY
#  include <sys/socket.h>
#  include <sys/types.h>
#  include <fcntl.h>         // fcntl / O_NONBLOCK
#  include <unistd.h>        // close
#  include <errno.h>
#  include <signal.h>        // signal / SIG_IGN (SIGPIPE suppression)
#endif

// ---------------------------------------------------------------------------
// Platform abstraction shims. POSIX and Winsock are ~95% API-compatible; the
// real differences are the handle type, the close/shutdown spellings, the
// invalid-handle sentinel, the non-blocking toggle, and how the last error is
// read (thread-local `errno` vs `WSAGetLastError`).
// ---------------------------------------------------------------------------
#if defined(_WIN32)
typedef SOCKET cajeta_native_socket_t;
typedef int    cajeta_socklen_t;
#  define CAJETA_INVALID_SOCKET   INVALID_SOCKET
#  define CAJETA_SOCKET_ERROR     SOCKET_ERROR
#  define cajeta_closesocket(s)   closesocket(s)
#else
typedef int       cajeta_native_socket_t;
typedef socklen_t cajeta_socklen_t;
#  define CAJETA_INVALID_SOCKET   (-1)
#  define CAJETA_SOCKET_ERROR     (-1)
#  define cajeta_closesocket(s)   close(s)
#endif

// ---------------------------------------------------------------------------
// Cross-platform error enum (the `cajeta_net_errno` shim).
//
// These ordinals are the stable contract between the native layer and the
// `cajeta.io.net` exception mapping (NET-1.8): the cajeta side switches on this
// value to pick the `NetException` subtype. Keep the ordinals append-only;
// never renumber. The mapping table lives in `docs/specification/io/net/Errors.md`
// (authored by NET-1.8 / NET-11.6).
// ---------------------------------------------------------------------------
enum cajeta_net_err {
    CAJETA_NET_OK                  = 0,   // no error
    CAJETA_NET_WOULDBLOCK          = 1,   // EAGAIN/EWOULDBLOCK/WSAEWOULDBLOCK
    CAJETA_NET_CONNECTION_REFUSED  = 2,   // ECONNREFUSED
    CAJETA_NET_CONNECTION_RESET    = 3,   // ECONNRESET
    CAJETA_NET_CONNECTION_ABORTED  = 4,   // ECONNABORTED
    CAJETA_NET_ADDRESS_IN_USE      = 5,   // EADDRINUSE
    CAJETA_NET_ADDRESS_NOT_AVAIL   = 6,   // EADDRNOTAVAIL
    CAJETA_NET_HOST_UNREACHABLE    = 7,   // EHOSTUNREACH
    CAJETA_NET_NETWORK_UNREACHABLE = 8,   // ENETUNREACH
    CAJETA_NET_BROKEN_PIPE         = 9,   // EPIPE
    CAJETA_NET_TIMED_OUT           = 10,  // ETIMEDOUT
    CAJETA_NET_INTERRUPTED         = 11,  // EINTR (caller retries)
    CAJETA_NET_INVALID             = 12,  // EINVAL / EBADF / ENOTSOCK
    CAJETA_NET_ACCESS              = 13,  // EACCES / EPERM
    CAJETA_NET_IN_PROGRESS         = 14,  // EINPROGRESS / WSAEINPROGRESS
    CAJETA_NET_OTHER               = 99   // anything not specifically mapped
};

// Read the platform's last socket error code (raw, un-normalized).
static int cajeta_net_raw_errno(void) {
#if defined(_WIN32)
    return WSAGetLastError();
#else
    return errno;
#endif
}

// Map a raw platform error code → the stable `cajeta_net_err` ordinal.
// Pass the value from `cajeta_net_raw_errno()`. Centralizing the mapping
// here is the entire point of the shim: every intrinsic reports failures
// through this single funnel so the cajeta exception layer sees one enum on
// all three OSes.
static int32_t cajeta_net_map_errno(int e) {
#if defined(_WIN32)
    switch (e) {
        case 0:                   return CAJETA_NET_OK;
        case WSAEWOULDBLOCK:      return CAJETA_NET_WOULDBLOCK;
        case WSAECONNREFUSED:     return CAJETA_NET_CONNECTION_REFUSED;
        case WSAECONNRESET:       return CAJETA_NET_CONNECTION_RESET;
        case WSAECONNABORTED:     return CAJETA_NET_CONNECTION_ABORTED;
        case WSAEADDRINUSE:       return CAJETA_NET_ADDRESS_IN_USE;
        case WSAEADDRNOTAVAIL:    return CAJETA_NET_ADDRESS_NOT_AVAIL;
        case WSAEHOSTUNREACH:     return CAJETA_NET_HOST_UNREACHABLE;
        case WSAENETUNREACH:      return CAJETA_NET_NETWORK_UNREACHABLE;
        case WSAESHUTDOWN:        return CAJETA_NET_BROKEN_PIPE;
        case WSAETIMEDOUT:        return CAJETA_NET_TIMED_OUT;
        case WSAEINTR:            return CAJETA_NET_INTERRUPTED;
        case WSAEINPROGRESS:      return CAJETA_NET_IN_PROGRESS;
        case WSAEALREADY:         return CAJETA_NET_IN_PROGRESS;
        case WSAEINVAL:           return CAJETA_NET_INVALID;
        case WSAEBADF:            return CAJETA_NET_INVALID;
        case WSAENOTSOCK:         return CAJETA_NET_INVALID;
        case WSAEACCES:           return CAJETA_NET_ACCESS;
        default:                  return CAJETA_NET_OTHER;
    }
#else
    switch (e) {
        case 0:               return CAJETA_NET_OK;
        case EAGAIN:          return CAJETA_NET_WOULDBLOCK;
#  if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
        case EWOULDBLOCK:     return CAJETA_NET_WOULDBLOCK;
#  endif
        case ECONNREFUSED:    return CAJETA_NET_CONNECTION_REFUSED;
        case ECONNRESET:      return CAJETA_NET_CONNECTION_RESET;
        case ECONNABORTED:    return CAJETA_NET_CONNECTION_ABORTED;
        case EADDRINUSE:      return CAJETA_NET_ADDRESS_IN_USE;
        case EADDRNOTAVAIL:   return CAJETA_NET_ADDRESS_NOT_AVAIL;
        case EHOSTUNREACH:    return CAJETA_NET_HOST_UNREACHABLE;
        case ENETUNREACH:     return CAJETA_NET_NETWORK_UNREACHABLE;
        case EPIPE:           return CAJETA_NET_BROKEN_PIPE;
        case ETIMEDOUT:       return CAJETA_NET_TIMED_OUT;
        case EINTR:           return CAJETA_NET_INTERRUPTED;
        case EINPROGRESS:     return CAJETA_NET_IN_PROGRESS;
        case EINVAL:          return CAJETA_NET_INVALID;
        case EBADF:           return CAJETA_NET_INVALID;
        case ENOTSOCK:        return CAJETA_NET_INVALID;
        case EACCES:          return CAJETA_NET_ACCESS;
        case EPERM:           return CAJETA_NET_ACCESS;
        default:              return CAJETA_NET_OTHER;
    }
#endif
}

// Public intrinsic: the cajeta layer reads the normalized error after any
// intrinsic that returned a failure sentinel, then maps it to a `NetException`
// subtype. Must be called immediately after the failing syscall on the same
// thread (errno / WSAGetLastError are thread-local and clobbered by the next
// syscall). Returns a `cajeta_net_err` ordinal.
int32_t __cajeta_net_last_error(void) {
    return cajeta_net_map_errno(cajeta_net_raw_errno());
}

// ---------------------------------------------------------------------------
// One-time Winsock init. On POSIX this is a no-op. On Windows every process
// using sockets must call WSAStartup once before any socket call and
// WSACleanup at teardown. We init lazily + idempotently on the first socket
// op via a pthreads once-guard (pthread is already a hard runtime dependency —
// the fiber scheduler uses it, and MinGW's winpthreads provides it on
// Windows). This mirrors the spec's "WSAStartup once at runtime init".
// ---------------------------------------------------------------------------
#if defined(_WIN32)
static pthread_once_t g_cajeta_wsa_once = PTHREAD_ONCE_INIT;
static int g_cajeta_wsa_ok = 0;

static void cajeta_net_wsa_startup_once(void) {
    WSADATA data;
    g_cajeta_wsa_ok = (WSAStartup(MAKEWORD(2, 2), &data) == 0);
}
#endif

// One-time SIGPIPE suppression (POSIX only). Writing to a socket whose peer
// has closed its read half raises SIGPIPE, whose default disposition is to
// kill the process — invisible on Windows (no SIGPIPE) but a real Linux/POSIX
// crash on any write to a closed peer. We ignore it process-wide once, at the
// first socket-creating op (which always precedes any send), so a broken-pipe
// write instead returns the EPIPE failure sentinel the cajeta layer already
// maps to BrokenPipeException via __cajeta_net_last_error(). Idempotent via a
// pthread_once guard. (Linux sends with MSG_NOSIGNAL too — see __cajeta_net_send
// — but ignoring SIGPIPE is the portable belt-and-suspenders for platforms /
// paths without MSG_NOSIGNAL, e.g. macOS sendto without SO_NOSIGPIPE.)
#if !defined(_WIN32)
static pthread_once_t g_cajeta_sigpipe_once = PTHREAD_ONCE_INIT;
static void cajeta_net_ignore_sigpipe_once(void) {
    signal(SIGPIPE, SIG_IGN);
}
#endif

// Idempotent, thread-safe Winsock init. Returns 1 on success / already-up,
// 0 if startup failed. POSIX always returns 1 (after ignoring SIGPIPE once).
// Called at the top of every socket-creating entry point.
static int cajeta_net_ensure_init(void) {
#if defined(_WIN32)
    pthread_once(&g_cajeta_wsa_once, cajeta_net_wsa_startup_once);
    return g_cajeta_wsa_ok;
#else
    pthread_once(&g_cajeta_sigpipe_once, cajeta_net_ignore_sigpipe_once);
    return 1;
#endif
}

// Narrow a native socket handle to the int32 ABI the cajeta layer expects.
// On POSIX this is identity; on Windows it truncates a SOCKET (always a small
// handle in practice) to int32. `CAJETA_INVALID_SOCKET` maps to -1.
static int32_t cajeta_net_to_fd(cajeta_native_socket_t s) {
    if (s == CAJETA_INVALID_SOCKET) return -1;
    return (int32_t) s;
}

// Re-widen an int32 fd back to the native socket handle for a Winsock/POSIX
// call. -1 → CAJETA_INVALID_SOCKET.
static cajeta_native_socket_t cajeta_net_from_fd(int32_t fd) {
    if (fd < 0) return CAJETA_INVALID_SOCKET;
    return (cajeta_native_socket_t) fd;
}

// ---------------------------------------------------------------------------
// Socket lifecycle + transfer intrinsics.
//
// `family`/`type`/`protocol` are passed as the **native** integer constants
// by the cajeta layer (it owns the enum→constant table, since AF_INET etc.
// are platform values). The address arguments are raw `sockaddr` byte
// buffers the cajeta layer packs/unpacks via NET-1.2's
// `__cajeta_net_sockaddr_pack/_unpack`; here they are opaque `(ptr, len)`.
//
// Convention: every fd-returning intrinsic returns the int32 fd or -1 on
// failure; every status intrinsic returns 0 on success or -1 on failure;
// every byte-count intrinsic returns the count (>= 0) or a NEGATIVE
// `cajeta_net_err` ordinal so the caller distinguishes WouldBlock (-1 *
// WOULDBLOCK) from a real error without a second errno read. After any
// failure the cajeta layer may also call `__cajeta_net_last_error()`.
// ---------------------------------------------------------------------------

// Create a socket. Returns the int32 fd, or -1 on failure.
int32_t __cajeta_net_socket(int32_t family, int32_t type, int32_t protocol) {
    if (!cajeta_net_ensure_init()) return -1;
    cajeta_native_socket_t s = socket(family, type, protocol);
    return cajeta_net_to_fd(s);
}

// Bind `fd` to the address in `addr[0..addrlen)`. Returns 0 / -1.
int32_t __cajeta_net_bind(int32_t fd, const void* addr, int32_t addrlen) {
    if (fd < 0 || !addr || addrlen <= 0) return -1;
    int r = bind(cajeta_net_from_fd(fd),
                 (const struct sockaddr*) addr,
                 (cajeta_socklen_t) addrlen);
    return r == CAJETA_SOCKET_ERROR ? -1 : 0;
}

// Mark `fd` as a passive listening socket with the given accept backlog.
// Returns 0 / -1.
int32_t __cajeta_net_listen(int32_t fd, int32_t backlog) {
    if (fd < 0) return -1;
    int r = listen(cajeta_net_from_fd(fd), (int) backlog);
    return r == CAJETA_SOCKET_ERROR ? -1 : 0;
}

// Accept one pending connection on listening socket `fd`. The peer's address
// is written into `addr_out[0..*addrlen_inout)` (caller-sized sockaddr
// buffer); `*addrlen_inout` is updated to the actual written length. Either
// out pointer may be NULL to discard the peer address. Returns the new
// connection's int32 fd, or -1 on failure (incl. WouldBlock — the caller
// checks `__cajeta_net_last_error()`).
int32_t __cajeta_net_accept(int32_t fd, void* addr_out, int32_t* addrlen_inout) {
    if (fd < 0) return -1;
    cajeta_socklen_t len = 0;
    cajeta_socklen_t* lenp = NULL;
    if (addr_out && addrlen_inout && *addrlen_inout > 0) {
        len = (cajeta_socklen_t) *addrlen_inout;
        lenp = &len;
    }
    cajeta_native_socket_t c = accept(cajeta_net_from_fd(fd),
                                      (struct sockaddr*) addr_out, lenp);
    if (c == CAJETA_INVALID_SOCKET) return -1;
    if (addrlen_inout && lenp) *addrlen_inout = (int32_t) len;
    return cajeta_net_to_fd(c);
}

// Connect `fd` to the address in `addr[0..addrlen)`. Returns 0 on success,
// -1 on failure. For a non-blocking socket an in-progress connect returns -1
// with last-error == IN_PROGRESS/WOULDBLOCK; the reactor (Phase 3) then waits
// for writability and checks SO_ERROR.
int32_t __cajeta_net_connect(int32_t fd, const void* addr, int32_t addrlen) {
    if (fd < 0 || !addr || addrlen <= 0) return -1;
    int r = connect(cajeta_net_from_fd(fd),
                    (const struct sockaddr*) addr,
                    (cajeta_socklen_t) addrlen);
    return r == CAJETA_SOCKET_ERROR ? -1 : 0;
}

// Read the pending socket error on `fd` after a non-blocking connect has
// signalled completion (the reactor saw the socket become writable). Returns
// CAJETA_NET_OK (0) when the connect succeeded, otherwise the normalized
// `cajeta_net_err` ordinal for the failure (ECONNREFUSED → CONNECTION_REFUSED,
// ETIMEDOUT → TIMED_OUT, …). This is the post-readiness half of the
// non-blocking connect dance described above __cajeta_net_connect: the caller
// issues the connect, gets IN_PROGRESS, parks on `await_writable`, then calls
// this to learn the outcome. Keeping the SO_ERROR / SOL_SOCKET platform
// constants in the C layer matches the POSIX-native / Windows-shim convention
// (the cajeta surface only ever sees the stable ordinal).
int32_t __cajeta_net_connect_result(int32_t fd) {
    if (fd < 0) return CAJETA_NET_INVALID;
    int soerr = 0;
    cajeta_socklen_t len = (cajeta_socklen_t) sizeof(soerr);
    int r = getsockopt(cajeta_net_from_fd(fd), SOL_SOCKET, SO_ERROR,
#if defined(_WIN32)
                       (char*) &soerr,
#else
                       &soerr,
#endif
                       &len);
    if (r == CAJETA_SOCKET_ERROR) {
        return cajeta_net_map_errno(cajeta_net_raw_errno());
    }
    // soerr == 0 → connect succeeded (map_errno's case 0 → CAJETA_NET_OK).
    return cajeta_net_map_errno(soerr);
}

// Send `len` bytes from `buf`. Returns the count sent (>= 0), or -1 on error
// (caller reads `__cajeta_net_last_error()` — WouldBlock is a normal
// non-blocking outcome). `flags` is the native send() flags value (0 in the
// common case; MSG_NOSIGNAL on Linux to suppress SIGPIPE — the cajeta layer
// supplies it).
int64_t __cajeta_net_send(int32_t fd, const void* buf, int64_t len, int32_t flags) {
    if (fd < 0 || (!buf && len > 0) || len < 0) return -1;
#if defined(_WIN32)
    // Winsock send() takes an `int` length; clamp to INT_MAX-safe chunk.
    int chunk = (len > 0x40000000) ? 0x40000000 : (int) len;
    int n = send(cajeta_net_from_fd(fd), (const char*) buf, chunk, (int) flags);
    return n == CAJETA_SOCKET_ERROR ? -1 : (int64_t) n;
#else
    ssize_t n = send(cajeta_net_from_fd(fd), buf, (size_t) len, (int) flags);
    return n < 0 ? -1 : (int64_t) n;
#endif
}

// Receive up to `len` bytes into `buf`. Returns the count read (>= 0; 0 ==
// orderly peer shutdown / EOF), or -1 on error (caller reads last-error;
// WouldBlock is the normal empty-non-blocking outcome).
int64_t __cajeta_net_recv(int32_t fd, void* buf, int64_t len, int32_t flags) {
    if (fd < 0 || (!buf && len > 0) || len < 0) return -1;
#if defined(_WIN32)
    int chunk = (len > 0x40000000) ? 0x40000000 : (int) len;
    int n = recv(cajeta_net_from_fd(fd), (char*) buf, chunk, (int) flags);
    return n == CAJETA_SOCKET_ERROR ? -1 : (int64_t) n;
#else
    ssize_t n = recv(cajeta_net_from_fd(fd), buf, (size_t) len, (int) flags);
    return n < 0 ? -1 : (int64_t) n;
#endif
}

// UDP: send `len` bytes to the explicit destination `addr[0..addrlen)`.
// Returns count sent or -1.
int64_t __cajeta_net_sendto(int32_t fd, const void* buf, int64_t len,
                            int32_t flags, const void* addr, int32_t addrlen) {
    if (fd < 0 || (!buf && len > 0) || len < 0) return -1;
    if (!addr || addrlen <= 0) return -1;
#if defined(_WIN32)
    int chunk = (len > 0x40000000) ? 0x40000000 : (int) len;
    int n = sendto(cajeta_net_from_fd(fd), (const char*) buf, chunk, (int) flags,
                   (const struct sockaddr*) addr, (cajeta_socklen_t) addrlen);
    return n == CAJETA_SOCKET_ERROR ? -1 : (int64_t) n;
#else
    ssize_t n = sendto(cajeta_net_from_fd(fd), buf, (size_t) len, (int) flags,
                       (const struct sockaddr*) addr, (cajeta_socklen_t) addrlen);
    return n < 0 ? -1 : (int64_t) n;
#endif
}

// UDP: receive up to `len` bytes; write the sender's address into
// `addr_out[0..*addrlen_inout)` and update `*addrlen_inout` to the actual
// length. Either out pointer may be NULL to discard the sender. Returns count
// read or -1.
int64_t __cajeta_net_recvfrom(int32_t fd, void* buf, int64_t len, int32_t flags,
                              void* addr_out, int32_t* addrlen_inout) {
    if (fd < 0 || (!buf && len > 0) || len < 0) return -1;
    cajeta_socklen_t alen = 0;
    cajeta_socklen_t* alenp = NULL;
    if (addr_out && addrlen_inout && *addrlen_inout > 0) {
        alen = (cajeta_socklen_t) *addrlen_inout;
        alenp = &alen;
    }
#if defined(_WIN32)
    int chunk = (len > 0x40000000) ? 0x40000000 : (int) len;
    int n = recvfrom(cajeta_net_from_fd(fd), (char*) buf, chunk, (int) flags,
                     (struct sockaddr*) addr_out, alenp);
    if (n == CAJETA_SOCKET_ERROR) return -1;
#else
    ssize_t n = recvfrom(cajeta_net_from_fd(fd), buf, (size_t) len, (int) flags,
                         (struct sockaddr*) addr_out, alenp);
    if (n < 0) return -1;
#endif
    if (addrlen_inout && alenp) *addrlen_inout = (int32_t) alen;
    return (int64_t) n;
}

// Wake any fiber parked on this descriptor in the I/O reactor before the
// descriptor goes away (defined in cajeta_rt_concurrent_exec.c; a no-op off
// Linux). Closing an fd removes it from the epoll interest list without
// delivering an event, so without this a parked fiber is never resumed —
// the shutdown-and-await hang. See the comment on the definition.
extern void __cajeta_io_close_wake(int32_t fd);

// Close the socket. Idempotent at the cajeta layer (it nulls its fd field).
// Returns 0 / -1.
int32_t __cajeta_net_close(int32_t fd) {
    if (fd < 0) return 0;   // already closed — no-op success
    // Order matters: publish the waiters while `fd` is still valid, then
    // close. A fiber woken this way retries its I/O, gets EBADF, and the
    // library maps that to the NetException its accept loop expects.
    __cajeta_io_close_wake(fd);
    int r = cajeta_closesocket(cajeta_net_from_fd(fd));
    return r == CAJETA_SOCKET_ERROR ? -1 : 0;
}

// Shut down part of a full-duplex connection. `how`: 0 = read (SHUT_RD),
// 1 = write (SHUT_WR), 2 = both (SHUT_RDWR). Returns 0 / -1.
int32_t __cajeta_net_shutdown(int32_t fd, int32_t how) {
    if (fd < 0) return -1;
#if defined(_WIN32)
    int native = (how == 0) ? SD_RECEIVE : (how == 1) ? SD_SEND : SD_BOTH;
#else
    int native = (how == 0) ? SHUT_RD : (how == 1) ? SHUT_WR : SHUT_RDWR;
#endif
    int r = shutdown(cajeta_net_from_fd(fd), native);
    return r == CAJETA_SOCKET_ERROR ? -1 : 0;
}

// setsockopt passthrough. `level`/`optname` are the native constants supplied
// by the cajeta option surface (NET-1.6). `optval[0..optlen)` is the raw
// option payload (an int32 for the boolean/int options, a `struct linger`
// for SO_LINGER, etc.). Returns 0 / -1.
int32_t __cajeta_net_setsockopt(int32_t fd, int32_t level, int32_t optname,
                                const void* optval, int32_t optlen) {
    if (fd < 0 || !optval || optlen <= 0) return -1;
    int r = setsockopt(cajeta_net_from_fd(fd), (int) level, (int) optname,
#if defined(_WIN32)
                       (const char*) optval,
#else
                       optval,
#endif
                       (cajeta_socklen_t) optlen);
    return r == CAJETA_SOCKET_ERROR ? -1 : 0;
}

// getsockopt passthrough. Reads the option into `optval[0..*optlen_inout)`
// and updates `*optlen_inout` to the actual length. Returns 0 / -1.
int32_t __cajeta_net_getsockopt(int32_t fd, int32_t level, int32_t optname,
                                void* optval, int32_t* optlen_inout) {
    if (fd < 0 || !optval || !optlen_inout || *optlen_inout <= 0) return -1;
    cajeta_socklen_t len = (cajeta_socklen_t) *optlen_inout;
    int r = getsockopt(cajeta_net_from_fd(fd), (int) level, (int) optname,
#if defined(_WIN32)
                       (char*) optval,
#else
                       optval,
#endif
                       &len);
    if (r == CAJETA_SOCKET_ERROR) return -1;
    *optlen_inout = (int32_t) len;
    return 0;
}

// Toggle non-blocking mode. `nonblocking` != 0 enables O_NONBLOCK
// (POSIX) / FIONBIO (Winsock); 0 restores blocking. Returns 0 / -1. The
// reactor (Phase 3) and `WouldBlock` surfacing (NET-1.7) depend on this.
int32_t __cajeta_net_set_nonblocking(int32_t fd, int32_t nonblocking) {
    if (fd < 0) return -1;
#if defined(_WIN32)
    u_long mode = nonblocking ? 1u : 0u;
    int r = ioctlsocket(cajeta_net_from_fd(fd), FIONBIO, &mode);
    return r == CAJETA_SOCKET_ERROR ? -1 : 0;
#else
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    if (nonblocking) flags |= O_NONBLOCK;
    else             flags &= ~O_NONBLOCK;
    return fcntl(fd, F_SETFL, flags) < 0 ? -1 : 0;
#endif
}
