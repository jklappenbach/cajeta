// cajeta.io.net — NET-1.7 non-blocking mode plus the non-throwing WouldBlock
// and connect-in-progress classifiers. #included once at the bottom of
// cajeta_runtime.c, AFTER cajeta_net_socket.c, whose file-statics it reuses.

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

// 1 iff the last socket error was EAGAIN/EWOULDBLOCK (POSIX) or WSAEWOULDBLOCK
// (Winsock), else 0. Call IMMEDIATELY after an I/O intrinsic's -1 sentinel, on
// the same thread: the next syscall clobbers the platform error.
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

// 1 iff a non-blocking connect is in flight (POSIX EINPROGRESS; Winsock reuses
// WSAEWOULDBLOCK). Not a failure: the caller waits for writability and reads
// SO_ERROR, which is what tells this apart from a hard connect failure.
int32_t __cajeta_net_is_in_progress(void) {
    int e = cajeta_net_raw_errno();
#if defined(_WIN32)
    // The WSAEWOULDBLOCK reading holds only when the last intrinsic WAS the
    // connect; from recv/send/accept the same code means "would block", and
    // calling that an in-flight connect is a misclassification.
    if (e == WSAEWOULDBLOCK) return cajeta_net_last_op_was_connect() ? 1 : 0;
    return (e == WSAEINPROGRESS || e == WSAEALREADY) ? 1 : 0;
#else
    return (e == EINPROGRESS) ? 1 : 0;
#endif
}

// Windows-only shadow of the last non-blocking value SET per fd, because
// FIONBIO is write-only and Winsock has no query. An open-addressed table under
// one mutex; POSIX has a real fcntl query and never touches this.
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

// 0/1 for a known `fd`, -1 when the mode was never set on it (the OS default,
// blocking, is what the public entry point reports for a miss).
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

// The toggle `setNonBlocking` lowers to: the NET-1.1 setter plus, on Windows,
// the shadow update that makes the effect queryable. Returns 0 or -1. It is a
// separate name so the NET-1.1 primitive its callers use stays unchanged.
int32_t __cajeta_net_set_nonblocking_tracked(int32_t fd, int32_t nonblocking) {
    int32_t r = __cajeta_net_set_nonblocking(fd, nonblocking);
#if defined(_WIN32)
    if (r == 0) cajeta_nb_shadow_put(fd, nonblocking ? 1 : 0);
#endif
    return r;
}

// Read the non-blocking bit back: 1 non-blocking, 0 blocking, -1 on error.
// POSIX asks fcntl(F_GETFL), which is authoritative whoever set the flag;
// Windows consults the shadow, and a miss reads as the OS default, blocking.
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
