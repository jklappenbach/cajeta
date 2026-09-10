// cajeta.io.net.reactor — NET-3.2 reactor lifecycle: the lazy-init latch, the
// portable shutdown wake pipe, and idempotent teardown. Textually #included by
// cajeta_runtime.c AFTER cajeta_net_reactor.c (NET-3.1), whose ABI it layers on.

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

// Owned by NET-3.1 (cajeta_net_reactor.c, #included just before this TU).
void __cajeta_net_reactor_active_reset(void);

// ---- lifecycle state: its own mutex, never the carrier-pool one ------------
static pthread_mutex_t __cajeta_net_lifecycle_mutex = PTHREAD_MUTEX_INITIALIZER;

// 1 once the first awaitable op initialized the net reactor; reset by shutdown
// so the next program (or JIT test, which shares the runtime) re-inits cleanly.
static int __cajeta_net_lifecycle_started = 0;

// Portable shutdown wake handle: a self-pipe (POSIX) / loopback socket pair
// (Windows). One byte to the write end wakes a select on the read end. -1 = none.
static int __cajeta_net_wake_read_fd  = -1;
static int __cajeta_net_wake_write_fd = -1;

// Create the self-pipe / loopback pair. Called under the lifecycle mutex from
// the lazy-init path, idempotent, returns 0 / -1. On failure the fds stay -1 and
// the reactor falls back to its poll timeout — the pipe is never correctness.
static int __cajeta_net_wake_pipe_open_locked(void) {
    if (__cajeta_net_wake_read_fd >= 0) return 0;

#if defined(_WIN32)
    // Winsock select() cannot watch an anonymous pipe, so self-connect a
    // loopback TCP pair: bind ephemeral, connect, accept, close the listener.
    SOCKET lst = socket(AF_INET, SOCK_STREAM, 0);
    if (lst == INVALID_SOCKET) return -1;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;

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
    // Non-blocking both ends: neither the wake write nor the drain read may block.
    for (int i = 0; i < 2; ++i) {
        int fl = fcntl(fds[i], F_GETFL, 0);
        if (fl >= 0) fcntl(fds[i], F_SETFL, fl | O_NONBLOCK);
    }
    __cajeta_net_wake_read_fd  = fds[0];
    __cajeta_net_wake_write_fd = fds[1];
    return 0;
#endif
}

// Close both wake fds. Called under the lifecycle mutex; idempotent.
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

// Lazy, idempotent, thread-safe init — the body NET-3.1's
// __cajeta_net_reactor_init delegates to. Latches `started` and returns 0 even
// when the best-effort wake pipe fails; later calls are a latched no-op.
int32_t __cajeta_net_reactor_lifecycle_init(void) {
    pthread_mutex_lock(&__cajeta_net_lifecycle_mutex);
    if (!__cajeta_net_lifecycle_started) {
        __cajeta_net_wake_pipe_open_locked();
        __cajeta_net_lifecycle_started = 1;
    }
    pthread_mutex_unlock(&__cajeta_net_lifecycle_mutex);
    return 0;
}

// Has the net reactor been initialized? Lets the runtime-teardown hook skip all
// of its work when no awaitable op ever ran.
int32_t __cajeta_net_reactor_started(void) {
    pthread_mutex_lock(&__cajeta_net_lifecycle_mutex);
    int s = __cajeta_net_lifecycle_started;
    pthread_mutex_unlock(&__cajeta_net_lifecycle_mutex);
    return (int32_t) s;
}

// Poke the shutdown wake pipe: one byte to the write end so a reactor blocked in
// a portable `select` over the read end returns promptly. Safe from any thread,
// and a no-op when no pipe is open.
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

// The wake pipe's read fd — what a portable waiter adds to its read fd_set — or
// -1 when no pipe is open. Drain it with __cajeta_net_reactor_wake_drain once
// the wake is observed.
int32_t __cajeta_net_reactor_wake_fd(void) {
    pthread_mutex_lock(&__cajeta_net_lifecycle_mutex);
    int rfd = __cajeta_net_wake_read_fd;
    pthread_mutex_unlock(&__cajeta_net_lifecycle_mutex);
    return (int32_t) rfd;
}

// Consume any pending wake bytes so the next select does not re-fire on the same
// byte. Called by the portable waiter after it observes the read end ready;
// non-blocking, and safe with no pipe open.
void __cajeta_net_reactor_wake_drain(void) {
    pthread_mutex_lock(&__cajeta_net_lifecycle_mutex);
    int rfd = __cajeta_net_wake_read_fd;
    pthread_mutex_unlock(&__cajeta_net_lifecycle_mutex);
    if (rfd < 0) return;
    char buf[64];
#if defined(_WIN32)
    // One recv takes whatever is queued: a wake writes a single byte per
    // shutdown, and MSG_DONTWAIT is not portable on Winsock.
    recv((SOCKET) rfd, buf, (int) sizeof(buf), 0);
#else
    ssize_t r;
    do { r = read(rfd, buf, sizeof(buf)); } while (r == sizeof(buf) ||
                                                   (r < 0 && errno == EINTR));
#endif
}

// Clean, idempotent teardown, called from __cajeta_task_shutdown once the
// carriers are joined: wake any portable waiter, clear the latch, close the wake
// pipe, drain NET-3.1's counter. NEVER closes the R9.4 epoll handle (not ours).
int32_t __cajeta_net_reactor_shutdown(void) {
    pthread_mutex_lock(&__cajeta_net_lifecycle_mutex);
    if (!__cajeta_net_lifecycle_started) {
        pthread_mutex_unlock(&__cajeta_net_lifecycle_mutex);
        return 0;
    }
    int wfd = __cajeta_net_wake_write_fd;
    pthread_mutex_unlock(&__cajeta_net_lifecycle_mutex);

    // Outside the lock, so the woken waiter can re-enter it to drain.
    if (wfd >= 0) {
        const char b = 1;
#if defined(_WIN32)
        send((SOCKET) wfd, &b, 1, 0);
#else
        ssize_t r; do { r = write(wfd, &b, 1); } while (r < 0 && errno == EINTR);
        (void) r;
#endif
    }

    pthread_mutex_lock(&__cajeta_net_lifecycle_mutex);
    __cajeta_net_lifecycle_started = 0;
    __cajeta_net_wake_pipe_close_locked();
    pthread_mutex_unlock(&__cajeta_net_lifecycle_mutex);

    // Outside our lock: the counter keeps its own single-threaded discipline.
    __cajeta_net_reactor_active_reset();
    return 0;
}
