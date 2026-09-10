// cajeta.io.net — native `TcpListener` helpers, #included once at the bottom of
// `cajeta_runtime.c`. Carries only the intrinsics that encode a platform
// constant (SOL_SOCKET, SO_REUSEADDR, SO_REUSEPORT), keeping every `#if` in C.

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

// `cajeta_net_from_fd`, `CAJETA_SOCKET_ERROR` and the `CAJETA_NET_*` ordinals
// come from cajeta_net_socket.c, #included earlier into the same TU.

// Turns SO_REUSEADDR on when `enable` != 0, so bind() succeeds on an address
// still in TIME_WAIT (and shares the port on Windows). Returns 0, or -1 with
// the reason in `__cajeta_net_last_error()`.
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

// Reads SO_REUSEADDR back: 1 enabled, 0 disabled, -1 on a getsockopt error.
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

// Turns SO_REUSEPORT (load-balanced multi-socket bind to one port) on when
// `enable` != 0. Windows has no such flag — SO_REUSEADDR already shares the
// port there — so it is a success no-op. Returns 0 on success or no-op, -1 else.
int32_t __cajeta_net_set_reuseport(int32_t fd, int32_t enable) {
    if (fd < 0) return -1;
#if defined(SO_REUSEPORT)
    int on = enable ? 1 : 0;
    int r = setsockopt(cajeta_net_from_fd(fd), SOL_SOCKET, SO_REUSEPORT,
                       &on, (cajeta_socklen_t) sizeof(on));
    return r == CAJETA_SOCKET_ERROR ? -1 : 0;
#else
    (void) enable;
    return 0;
#endif
}

// 1 iff this platform has a distinct SO_REUSEPORT flag, so a test can tell the
// real-flag path from the no-op path without platform `#if`s of its own.
int32_t __cajeta_net_has_reuseport(void) {
#if defined(SO_REUSEPORT)
    return 1;
#else
    return 0;
#endif
}

// `__cajeta_net_getsockname` is deliberately NOT defined here: cajeta_net_getname.c
// owns it, and both files are textually #included into cajeta_runtime.c, so a
// second definition would be a duplicate-symbol error.
