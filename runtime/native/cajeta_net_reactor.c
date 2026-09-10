// cajeta.io.net.reactor — the NET-3.1 five-intrinsic reactor ABI. #included ONCE at the
// bottom of cajeta_runtime.c, AFTER the R9.4 I/O-reactor block and cajeta_net_socket.c,
// whose file-static symbols it delegates to rather than standing up a second engine.

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

// Public readiness vocabulary — the same bit values as the R9.4 reactor's, so `interest`
// is one contract runtime-wide. Re-#defined defensively; the values MUST match.
#ifndef CAJETA_IO_READ
#  define CAJETA_IO_READ  1
#endif
#ifndef CAJETA_IO_WRITE
#  define CAJETA_IO_WRITE 2
#endif

// Probe results; the await intrinsics narrow these to 1 ready / 0 timeout / -1 error.
#define CAJETA_REACTOR_READY    1
#define CAJETA_REACTOR_TIMEOUT  0
#define CAJETA_REACTOR_ERROR  (-1)

// NET-3.2 reactor-lifecycle hook, defined in cajeta_net_reactor_lifecycle.c (#included
// right after this TU). Forward-declared so init below can delegate its lazy-init with
// no include-order constraint between the two files.
int32_t __cajeta_net_reactor_lifecycle_init(void);

// Can this platform wait on an ARBITRARY fd? 1 when yes, 0 otherwise. Winsock select()
// accepts only SOCKETs, so a Windows console or pipe handle cannot be polled at all, and
// FileReader.awaitReadable refuses up front rather than reporting "bad descriptor".
int32_t __cajeta_io_await_supported(void) {
#if defined(_WIN32)
    return 0;
#else
    return 1;
#endif
}

// Portable single-fd readiness probe: blocks the CALLING OS THREAD until `fd` is ready
// for any bit in `interest`, or `timeout_ms` (negative = forever) elapses. Built on
// select(), the one readiness primitive identical on POSIX fds and Winsock SOCKETs.
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
    // Always watch the exception set: a refused non-blocking connect surfaces there on
    // Winsock (and as writable + SO_ERROR on POSIX), so the caller's check sees it.
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

// Lazy-initialize the reactor engine; idempotent, 0 on success and -1 on failure. On
// Linux the epoll handle and reactor thread are created by the R9.4 engine at the first
// __cajeta_io_wait, so this need only guarantee Winsock is up and report readiness.
int32_t __cajeta_net_reactor_init(void) {
#if defined(_WIN32)
    // NET-1.1's idempotent WSAStartup guard, for a reactor-first program.
    if (!cajeta_net_ensure_init()) return -1;
#endif
    // Lazy, idempotent lifecycle init. Must run after Winsock is up on Windows so the
    // wake pipe's loopback socket pair can be created.
    return __cajeta_net_reactor_lifecycle_init();
}

// register / deregister — arm or drop interest for a fiber handle. No-ops today: the v1
// one-shot model arms-and-parks atomically (EPOLLONESHOT auto-disarms), so there is no
// per-fd table. The signature is final; `fiber_handle` is the parking cajeta_fiber*.

// Live-registration balance. register/deregister keep no waiter table but do maintain
// this counter, so the registration-leak invariant — zero once a wave of ops settles —
// has a portable signal. A plain int: v1 serializes these through one reactor thread.
static int32_t cajeta_net_reactor_active = 0;

int32_t __cajeta_net_reactor_register(int32_t fd, int32_t interest,
                                      void* fiber_handle) {
    (void) fd; (void) interest; (void) fiber_handle;
    cajeta_net_reactor_active++;
    return 0;
}

int32_t __cajeta_net_reactor_deregister(int32_t fd) {
    (void) fd;
    // Clamp at zero: the cancellation path may double-fire a deregister and a
    // never-armed fd may be deregistered defensively; neither must mask a real leak.
    if (cajeta_net_reactor_active > 0) cajeta_net_reactor_active--;
    return 0;
}

// The number of `register` calls not yet matched by a `deregister`. Read on the same
// carrier that mutates it under the v1 model, so no fence is needed yet.
int32_t __cajeta_net_reactor_active_count(void) {
    return cajeta_net_reactor_active;
}

// Drain the balance to 0 at clean shutdown, once the carriers and reactor thread have
// joined and no await can still be in flight. Idempotent.
void __cajeta_net_reactor_active_reset(void) {
    cajeta_net_reactor_active = 0;
}

// Park until `fd` is ready: 1 READY, -1 ERROR (a timeout-less await never times out).
// On Linux these delegate to the R9.4 __cajeta_io_wait, which parks the fiber and frees
// the carrier; elsewhere they fall through to the portable, carrier-blocking select.

// Does the native await BLOCK THE CARRIER rather than park the fiber? 1 where the
// dedicated fiber-park engine has not landed and the await falls through to a blocking
// select, 0 on Linux. The Cajeta adapters poll-and-park instead wherever this is 1.
int32_t __cajeta_net_await_carrier_blocking(void) {
#if defined(__linux__)
    return 0;
#else
    return 1;
#endif
}

int32_t __cajeta_net_await_readable(int32_t fd) {
#if defined(__linux__)
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

// Deadline-bounded twins. On Linux these ride the fiber-parking __cajeta_io_wait_timed so
// the carrier stays free — a blocking probe there starved every fiber co-hosted on it.
// Non-fiber and non-Linux callers get the blocking probe. 1 READY / 0 TIMEOUT / -1 ERROR.
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
