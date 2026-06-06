// cajeta.net — NET-1.6 native socket-option surface.
//
// This translation unit is **#included once** at the bottom of
// `cajeta_runtime.c` (the same single-TU -> bitcode -> embed build path the
// NET-1.1 socket intrinsics ride; see the header of `cajeta_net_socket.c`).
// It MUST be included AFTER `cajeta_net_socket.c` because it reuses that
// file's fd-ABI helpers (`cajeta_net_from_fd`, `cajeta_socklen_t`,
// `CAJETA_SOCKET_ERROR`). No CMake change to the bitcode-embed path is
// required — only the one `#include "cajeta_net_socket_options.c"` line in
// `cajeta_runtime.c` alongside the existing net `#include`s.
//
// **Scope of NET-1.6** (per plan/cajeta-net-plan.md): the *typed* socket
// option surface — `setNoDelay` (TCP_NODELAY), `setKeepAlive`
// (SO_KEEPALIVE), `setReuseAddress`/`setReusePort` (SO_REUSEADDR/SO_REUSEPORT,
// shipped natively by NET-1.4 in `cajeta_net_listener.c` and *reused* here, not
// re-implemented), `setRecvBufferSize`/`setSendBufferSize`
// (SO_RCVBUF/SO_SNDBUF), `setLinger` (SO_LINGER, `struct linger`),
// `setBroadcast` (SO_BROADCAST, UDP), `setTtl` (IP_TTL / IPV6_UNICAST_HOPS),
// and IPv6 `setOnlyV6` (IPV6_V6ONLY). Each has a getter.
//
// **Why typed intrinsics, not raw `setsockopt` ints from Cajeta.** The whole
// point (per NET-1.4's note that "the typed, general option surface ... is
// NET-1.6; it will systematize the constant table") is to keep every platform
// `#if` — `SOL_SOCKET` / `IPPROTO_TCP` / `IPPROTO_IPV6` / `TCP_NODELAY` /
// `SO_*` / `IP_TTL` / `IPV6_*` — entirely in C, exactly as NET-1.2 keeps the
// `AF_*` family constants behind `sockaddr_pack`. The Cajeta `SocketOptions`
// surface passes only portable booleans / ints / (seconds for linger); it
// never sees a platform option constant. This mirrors the NET-1.4
// `__cajeta_net_set_reuseaddr` precedent, one option per pair.
//
// **ABI conventions** (identical to the rest of the net layer):
//   - boolean options: `int32_t __cajeta_net_set_<opt>(int32_t fd, int32_t on)`
//     returns 0 on success / -1 on failure; the matching getter returns 1 if
//     enabled, 0 if disabled, -1 on a getsockopt error.
//   - int-valued options (buffer sizes, TTL): the setter takes the value, the
//     getter returns the read-back value (>= 0) or -1 on error. The kernel may
//     round/clamp a requested buffer size, so the getter is the source of
//     truth (the set->get test asserts the kernel honored the *intent*, e.g.
//     grew the buffer, not byte-equality — see NetOptionsTests).
//   - linger: setter takes `(on, seconds)`; on==0 disables (the seconds are
//     ignored), on!=0 enables with the given linger timeout. The getter writes
//     `*on_out` / `*seconds_out` and returns 0 / -1.
//
// After any -1 the Cajeta layer reads `__cajeta_net_last_error()` (NET-1.1)
// and raises the mapped `NetException` subtype, exactly as the transfer
// primitives do.
//
// No async here — these are plain `setsockopt`/`getsockopt` calls, valid in
// both blocking and non-blocking mode; the reactor (Phase 3) sets the same
// options on its non-blocking sockets through these same symbols.

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>      // IPV6_V6ONLY, IPPROTO_IPV6, IPV6_UNICAST_HOPS
#else
#  include <netinet/in.h>    // IPPROTO_TCP, IPPROTO_IP, IPPROTO_IPV6, IP_TTL
#  include <netinet/tcp.h>   // TCP_NODELAY
#  include <sys/socket.h>    // SOL_SOCKET, SO_*, struct linger
#endif

#include <stdint.h>
#include <string.h>

// Reuses the int32 <-> native-handle narrowing + the CAJETA_SOCKET_ERROR /
// cajeta_socklen_t typedefs from cajeta_net_socket.c (textually #included
// earlier in cajeta_runtime.c). Keeping these in a separate file is a *review*
// boundary, not a *compilation* boundary.

// ---------------------------------------------------------------------------
// Internal helpers: a generic int-valued setsockopt/getsockopt at a given
// (level, optname). All the typed booleans + int options funnel through these
// so the platform char*-cast `#if` is written exactly once.
// ---------------------------------------------------------------------------

// Set an `int`-valued option. Returns 0 / -1.
static int32_t cajeta_opt_set_int(int32_t fd, int level, int optname, int value) {
    if (fd < 0) return -1;
    int v = value;
    int r = setsockopt(cajeta_net_from_fd(fd), level, optname,
#if defined(_WIN32)
                       (const char*) &v,
#else
                       &v,
#endif
                       (cajeta_socklen_t) sizeof(v));
    return r == CAJETA_SOCKET_ERROR ? -1 : 0;
}

// Read an `int`-valued option into `*out`. Returns 0 / -1.
static int32_t cajeta_opt_get_int(int32_t fd, int level, int optname, int* out) {
    if (fd < 0 || !out) return -1;
    int v = 0;
    cajeta_socklen_t len = (cajeta_socklen_t) sizeof(v);
    int r = getsockopt(cajeta_net_from_fd(fd), level, optname,
#if defined(_WIN32)
                       (char*) &v,
#else
                       &v,
#endif
                       &len);
    if (r == CAJETA_SOCKET_ERROR) return -1;
    *out = v;
    return 0;
}

// Boolean setter wrapper: forces the payload to a clean 0/1 int.
static int32_t cajeta_opt_set_bool(int32_t fd, int level, int optname, int32_t on) {
    return cajeta_opt_set_int(fd, level, optname, on ? 1 : 0);
}

// Boolean getter wrapper: returns 1 / 0 / -1.
static int32_t cajeta_opt_get_bool(int32_t fd, int level, int optname) {
    int v = 0;
    if (cajeta_opt_get_int(fd, level, optname, &v) != 0) return -1;
    return v != 0 ? 1 : 0;
}

// ---------------------------------------------------------------------------
// TCP_NODELAY — disable Nagle's algorithm so small writes go out immediately
// (the canonical latency-vs-throughput knob for request/response protocols).
// Lives at IPPROTO_TCP; valid only on a SOCK_STREAM socket.
// ---------------------------------------------------------------------------
int32_t __cajeta_net_set_nodelay(int32_t fd, int32_t on) {
    return cajeta_opt_set_bool(fd, IPPROTO_TCP, TCP_NODELAY, on);
}
int32_t __cajeta_net_get_nodelay(int32_t fd) {
    return cajeta_opt_get_bool(fd, IPPROTO_TCP, TCP_NODELAY);
}

// ---------------------------------------------------------------------------
// SO_KEEPALIVE — enable TCP keepalive probes on an idle connection so a dead
// peer is eventually detected. SOL_SOCKET level. (The per-probe tuning
// constants TCP_KEEPIDLE/INTVL/CNT are deliberately out of NET-1.6 scope —
// they are non-portable in name; only the on/off switch is portable.)
// ---------------------------------------------------------------------------
int32_t __cajeta_net_set_keepalive(int32_t fd, int32_t on) {
    return cajeta_opt_set_bool(fd, SOL_SOCKET, SO_KEEPALIVE, on);
}
int32_t __cajeta_net_get_keepalive(int32_t fd) {
    return cajeta_opt_get_bool(fd, SOL_SOCKET, SO_KEEPALIVE);
}

// ---------------------------------------------------------------------------
// SO_RCVBUF / SO_SNDBUF — kernel socket buffer sizes (bytes). The kernel may
// round up, clamp, or (on Linux) double the requested value for bookkeeping;
// the getter returns the *effective* size, so callers read back rather than
// assume byte-equality. SOL_SOCKET level.
// ---------------------------------------------------------------------------
int32_t __cajeta_net_set_recvbuf(int32_t fd, int32_t bytes) {
    if (bytes < 0) return -1;
    return cajeta_opt_set_int(fd, SOL_SOCKET, SO_RCVBUF, (int) bytes);
}
int32_t __cajeta_net_get_recvbuf(int32_t fd) {
    int v = 0;
    if (cajeta_opt_get_int(fd, SOL_SOCKET, SO_RCVBUF, &v) != 0) return -1;
    return (int32_t) v;
}
int32_t __cajeta_net_set_sendbuf(int32_t fd, int32_t bytes) {
    if (bytes < 0) return -1;
    return cajeta_opt_set_int(fd, SOL_SOCKET, SO_SNDBUF, (int) bytes);
}
int32_t __cajeta_net_get_sendbuf(int32_t fd) {
    int v = 0;
    if (cajeta_opt_get_int(fd, SOL_SOCKET, SO_SNDBUF, &v) != 0) return -1;
    return (int32_t) v;
}

// ---------------------------------------------------------------------------
// SO_LINGER — control what close() does when unsent data remains. `on`==0
// disables linger (close returns immediately, the kernel drains in the
// background — the default). `on`!=0 enables it with `seconds`: close blocks
// up to `seconds` trying to flush, and on timeout the connection is reset
// (RST) rather than a graceful FIN. Carried as `struct linger`, the one
// option whose payload is a struct, not an int — hence its own helper.
//
// `struct linger` is identical on POSIX and Winsock: `{ u_short l_onoff;
// u_short l_linger; }` (Winsock) / `{ int l_onoff; int l_linger; }` (POSIX).
// We populate the fields portably via the struct members so the field widths
// are whatever the platform declares.
// ---------------------------------------------------------------------------
int32_t __cajeta_net_set_linger(int32_t fd, int32_t on, int32_t seconds) {
    if (fd < 0) return -1;
    if (seconds < 0) seconds = 0;
    struct linger lg;
    memset(&lg, 0, sizeof(lg));
    lg.l_onoff  = (unsigned short) (on ? 1 : 0);
    lg.l_linger = (unsigned short) (on ? seconds : 0);
    int r = setsockopt(cajeta_net_from_fd(fd), SOL_SOCKET, SO_LINGER,
#if defined(_WIN32)
                       (const char*) &lg,
#else
                       &lg,
#endif
                       (cajeta_socklen_t) sizeof(lg));
    return r == CAJETA_SOCKET_ERROR ? -1 : 0;
}

// Read SO_LINGER back: writes 1/0 to `*on_out` and the linger seconds to
// `*seconds_out`. Either out pointer may be NULL to discard that field.
// Returns 0 / -1.
int32_t __cajeta_net_get_linger(int32_t fd, int32_t* on_out, int32_t* seconds_out) {
    if (fd < 0) return -1;
    struct linger lg;
    memset(&lg, 0, sizeof(lg));
    cajeta_socklen_t len = (cajeta_socklen_t) sizeof(lg);
    int r = getsockopt(cajeta_net_from_fd(fd), SOL_SOCKET, SO_LINGER,
#if defined(_WIN32)
                       (char*) &lg,
#else
                       &lg,
#endif
                       &len);
    if (r == CAJETA_SOCKET_ERROR) return -1;
    if (on_out)      *on_out      = lg.l_onoff != 0 ? 1 : 0;
    if (seconds_out) *seconds_out = (int32_t) lg.l_linger;
    return 0;
}

// ---------------------------------------------------------------------------
// SO_BROADCAST — permit a UDP socket to send to a broadcast address. SOL_SOCKET
// level; meaningful only on SOCK_DGRAM. Off by default; the kernel rejects a
// broadcast sendto without it.
// ---------------------------------------------------------------------------
int32_t __cajeta_net_set_broadcast(int32_t fd, int32_t on) {
    return cajeta_opt_set_bool(fd, SOL_SOCKET, SO_BROADCAST, on);
}
int32_t __cajeta_net_get_broadcast(int32_t fd) {
    return cajeta_opt_get_bool(fd, SOL_SOCKET, SO_BROADCAST);
}

// ---------------------------------------------------------------------------
// TTL / hop limit — the unicast hop limit on outbound packets. For an IPv4
// socket this is IP_TTL at IPPROTO_IP; for an IPv6 socket it is
// IPV6_UNICAST_HOPS at IPPROTO_IPV6 (the two families name it differently).
// The cajeta layer knows the socket's family (it created the socket from the
// SocketAddress family), so it passes `is_v6` to pick the right pair — keeping
// the IPPROTO_* / option-name `#if`s here in C.
//
// `ttl` is 0..255. Returns 0 / -1 (set) and the read-back value / -1 (get).
// ---------------------------------------------------------------------------
int32_t __cajeta_net_set_ttl(int32_t fd, int32_t is_v6, int32_t ttl) {
    if (ttl < 0 || ttl > 255) return -1;
    if (is_v6) {
        return cajeta_opt_set_int(fd, IPPROTO_IPV6, IPV6_UNICAST_HOPS, (int) ttl);
    }
    return cajeta_opt_set_int(fd, IPPROTO_IP, IP_TTL, (int) ttl);
}
int32_t __cajeta_net_get_ttl(int32_t fd, int32_t is_v6) {
    int v = 0;
    int level   = is_v6 ? IPPROTO_IPV6 : IPPROTO_IP;
    int optname = is_v6 ? IPV6_UNICAST_HOPS : IP_TTL;
    if (cajeta_opt_get_int(fd, level, optname, &v) != 0) return -1;
    return (int32_t) v;
}

// ---------------------------------------------------------------------------
// IPV6_V6ONLY — when on, an AF_INET6 socket accepts IPv6 traffic only; when
// off, it also accepts IPv4-mapped (`::ffff:a.b.c.d`) connections (dual-stack).
// IPPROTO_IPV6 level; valid only on an IPv6 socket. Platform defaults differ
// (Linux off, the BSDs/Windows on), so callers that care set it explicitly —
// which is the entire reason this option is exposed.
// ---------------------------------------------------------------------------
int32_t __cajeta_net_set_only_v6(int32_t fd, int32_t on) {
    return cajeta_opt_set_bool(fd, IPPROTO_IPV6, IPV6_V6ONLY, on);
}
int32_t __cajeta_net_get_only_v6(int32_t fd) {
    return cajeta_opt_get_bool(fd, IPPROTO_IPV6, IPV6_V6ONLY);
}
