// cajeta.io.net — native socket intrinsics (BSD sockets / Winsock), #included once at
// the bottom of cajeta_runtime.c. Blocking-mode primitives; a descriptor crosses the
// cajeta boundary as int32 (a Windows SOCKET narrows), with -1 as the "no socket" value.

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
// No `#pragma comment` here: it is MSVC-only, and under MinGW the ws2_32 link comes
// from CMake, while the JIT-embedded bitcode resolves against the already-loaded copy.
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

// ---- Platform shims: handle type, close spelling, invalid sentinel, last error ----
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

// ---- Cross-platform error enum ----
// The stable contract with the cajeta exception mapping, which switches on this value
// to pick a NetException subtype: the ordinals are append-only and never renumbered.
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

// Maps a raw platform code, as returned by cajeta_net_raw_errno(), to the stable
// ordinal: the single funnel every intrinsic's failures are reported through.
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

// The `cajeta_net_err` ordinal for the last failure. Must be called on the same thread
// immediately after it: errno and WSAGetLastError are thread-local and soon clobbered.
int32_t __cajeta_net_last_error(void) {
    return cajeta_net_map_errno(cajeta_net_raw_errno());
}

// ---- One-time Winsock init: WSAStartup on the first socket op, under a once-guard ----
#if defined(_WIN32)
static pthread_once_t g_cajeta_wsa_once = PTHREAD_ONCE_INIT;
static int g_cajeta_wsa_ok = 0;

static void cajeta_net_wsa_startup_once(void) {
    WSADATA data;
    g_cajeta_wsa_ok = (WSAStartup(MAKEWORD(2, 2), &data) == 0);
}
#endif

// One-time SIGPIPE suppression (POSIX). Writing to a peer that closed its read half
// would otherwise kill the process, where the cajeta layer wants the EPIPE sentinel it
// maps to BrokenPipeException. Belt and braces with MSG_NOSIGNAL, which is not portable.
#if !defined(_WIN32)
static pthread_once_t g_cajeta_sigpipe_once = PTHREAD_ONCE_INIT;
static void cajeta_net_ignore_sigpipe_once(void) {
    signal(SIGPIPE, SIG_IGN);
}
#endif

// Called at the top of every socket-creating entry point: 1 when sockets are usable,
// which POSIX always is, once it has ignored SIGPIPE.
static int cajeta_net_ensure_init(void) {
#if defined(_WIN32)
    pthread_once(&g_cajeta_wsa_once, cajeta_net_wsa_startup_once);
    return g_cajeta_wsa_ok;
#else
    pthread_once(&g_cajeta_sigpipe_once, cajeta_net_ignore_sigpipe_once);
    return 1;
#endif
}

// Narrows a native handle to the int32 ABI: identity on POSIX, a truncation of a
// (always small in practice) SOCKET on Windows. CAJETA_INVALID_SOCKET becomes -1.
static int32_t cajeta_net_to_fd(cajeta_native_socket_t s) {
    if (s == CAJETA_INVALID_SOCKET) return -1;
    return (int32_t) s;
}

// The inverse, for handing an fd back to a Winsock or POSIX call.
static cajeta_native_socket_t cajeta_net_from_fd(int32_t fd) {
    if (fd < 0) return CAJETA_INVALID_SOCKET;
    return (cajeta_native_socket_t) fd;
}

// ---- Socket lifecycle + transfer intrinsics ----
// family/type/protocol are native constants and the addresses opaque (ptr, len) sockaddr
// buffers; the cajeta layer owns both tables. An fd-returning intrinsic answers the fd
// or -1, a status one 0 or -1, a byte-count one the count or -1.

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

// Marks `fd` passive with the given accept backlog. Returns 0 / -1.
int32_t __cajeta_net_listen(int32_t fd, int32_t backlog) {
    if (fd < 0) return -1;
    int r = listen(cajeta_net_from_fd(fd), (int) backlog);
    return r == CAJETA_SOCKET_ERROR ? -1 : 0;
}

// Accepts one pending connection, writing the peer into the caller-sized `addr_out` and
// updating `*addrlen_inout`; either out pointer may be NULL to discard it.
// Which intrinsic last produced an error, per thread — Windows only. Winsock reuses
// WSAEWOULDBLOCK for both "would block" and "connect in flight", so the non-throwing
// classifiers consult this: connect sets it, every other error path clears it.
#if defined(_WIN32)
static _Thread_local int g_cajeta_net_last_op_connect = 0;
static inline void cajeta_net_note_op(int is_connect) {
    g_cajeta_net_last_op_connect = is_connect ? 1 : 0;
}
static inline int cajeta_net_last_op_was_connect(void) {
    return g_cajeta_net_last_op_connect;
}
#else
static inline void cajeta_net_note_op(int is_connect) { (void) is_connect; }
static inline int cajeta_net_last_op_was_connect(void) { return 0; }
#endif

int32_t __cajeta_net_accept(int32_t fd, void* addr_out, int32_t* addrlen_inout) {
    if (fd < 0) return -1;
    cajeta_net_note_op(0);
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

// Connects `fd` to `addr[0..addrlen)`. Returns 0 / -1; on a non-blocking socket an
// in-progress connect is -1 with a last-error of IN_PROGRESS or WOULDBLOCK.
int32_t __cajeta_net_connect(int32_t fd, const void* addr, int32_t addrlen) {
    if (fd < 0 || !addr || addrlen <= 0) return -1;
    cajeta_net_note_op(1);
    int r = connect(cajeta_net_from_fd(fd),
                    (const struct sockaddr*) addr,
                    (cajeta_socklen_t) addrlen);
    return r == CAJETA_SOCKET_ERROR ? -1 : 0;
}

// The outcome of a non-blocking connect, read once the socket is writable: OK when it
// succeeded, else the normalized ordinal. The other half of the IN_PROGRESS dance.
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
    return cajeta_net_map_errno(soerr);
}

// Sends `len` bytes from `buf`, returning the count sent or -1. `flags` is the native
// send() value the cajeta layer supplies, MSG_NOSIGNAL included on Linux.
int64_t __cajeta_net_send(int32_t fd, const void* buf, int64_t len, int32_t flags) {
    if (fd < 0 || (!buf && len > 0) || len < 0) return -1;
    cajeta_net_note_op(0);
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

// Receives up to `len` bytes into `buf`: the count read, 0 for an orderly peer shutdown,
// or -1 on error, where WouldBlock is the normal empty non-blocking outcome.
int64_t __cajeta_net_recv(int32_t fd, void* buf, int64_t len, int32_t flags) {
    if (fd < 0 || (!buf && len > 0) || len < 0) return -1;
    cajeta_net_note_op(0);
#if defined(_WIN32)
    int chunk = (len > 0x40000000) ? 0x40000000 : (int) len;
    int n = recv(cajeta_net_from_fd(fd), (char*) buf, chunk, (int) flags);
    return n == CAJETA_SOCKET_ERROR ? -1 : (int64_t) n;
#else
    ssize_t n = recv(cajeta_net_from_fd(fd), buf, (size_t) len, (int) flags);
    return n < 0 ? -1 : (int64_t) n;
#endif
}

// UDP: sends to the explicit destination `addr[0..addrlen)`. Count sent, or -1.
int64_t __cajeta_net_sendto(int32_t fd, const void* buf, int64_t len,
                            int32_t flags, const void* addr, int32_t addrlen) {
    if (fd < 0 || (!buf && len > 0) || len < 0) return -1;
    if (!addr || addrlen <= 0) return -1;
    cajeta_net_note_op(0);
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

// UDP: receives into `buf` and the sender into `addr_out`, updating `*addrlen_inout`;
// either out pointer may be NULL to discard the sender. Count read, or -1.
int64_t __cajeta_net_recvfrom(int32_t fd, void* buf, int64_t len, int32_t flags,
                              void* addr_out, int32_t* addrlen_inout) {
    if (fd < 0 || (!buf && len > 0) || len < 0) return -1;
    cajeta_net_note_op(0);
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

// Wakes any fiber parked on this descriptor before it goes away: closing an fd drops it
// from the epoll interest list with no event, so a parked fiber would never resume.
extern int32_t __cajeta_io_close_fd(int32_t fd);

// Closes the socket; idempotent, as the cajeta layer nulls its fd field. Returns 0 / -1.
int32_t __cajeta_net_close(int32_t fd) {
    if (fd < 0) return 0;   // already closed — no-op success
    // Order matters: wake the waiters while `fd` is still valid, then close. A fiber
    // woken this way retries, gets EBADF, and the library maps it to a NetException.
#if defined(__linux__)
    // Wake and close under one lock; a socket fd is an ordinary descriptor here.
    return __cajeta_io_close_fd(fd);
#else
    int r = cajeta_closesocket(cajeta_net_from_fd(fd));
    return r == CAJETA_SOCKET_ERROR ? -1 : 0;
#endif
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

// setsockopt passthrough: `level` and `optname` are native constants and `optval` the
// raw payload, an int32 for the boolean options or a `struct linger` for SO_LINGER.
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

// getsockopt passthrough: reads into `optval` and updates `*optlen_inout`. 0 / -1.
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

// Toggles non-blocking mode: a non-zero `nonblocking` sets O_NONBLOCK or FIONBIO, 0
// restores blocking. Returns 0 / -1.
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
