// cajeta.io.net — NET-1.6 native socket-option surface.
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

// ===========================================================================
// NET-14.1 — UDP multicast option intrinsics.
//
// Same doctrine as everything above: every platform constant (IP_ADD_MEMBERSHIP
// vs IPV6_JOIN_GROUP naming, the u_char-vs-int payload width quirk, mreq struct
// shapes) stays in C; the Cajeta surface passes only the AddressFamily-agnostic
// pieces — network-order octets, an interface index, portable ints.
//
// Family split, deliberately asymmetric because the kernels are:
//   - IPv4 selects the interface by ADDRESS (`struct ip_mreq.imr_interface`,
//     INADDR_ANY = kernel default). `ip_mreqn`'s by-index form is Linux-only,
//     so it is not offered.
//   - IPv6 selects the interface by INDEX (`struct ipv6_mreq.ipv6mr_interface`,
//     0 = kernel default). v6 interfaces have no single address to name.
//
// Payload-width quirk: IPv4's IP_MULTICAST_TTL and IP_MULTICAST_LOOP take a
// `u_char` on the BSDs/macOS (an `int` payload fails EINVAL there); Linux
// accepts either; Winsock wants a DWORD. IPv6's equivalents take an int
// everywhere. The `#if` lives here, once.
// ===========================================================================

// Octet parameters cross the @Native bridge as cajeta int8[] HEADERS —
// `{ i64 count, [N x i8] data }` — so the address bytes live at offset 8
// (the `__cajeta_sha1_update` convention). NULL stays NULL.
static const void* cajeta_octets_of(const void* hdr) {
    return hdr ? ((const uint8_t*) hdr) + 8 : (const void*) 0;
}

// Join/leave an IPv4 group. `group_hdr` = int8[] header holding 4
// network-order bytes; `iface_hdr` likewise for the interface address, or
// NULL for the kernel default (INADDR_ANY). Returns 0 / -1.
static int32_t cajeta_mcast_v4(int32_t fd, int optname,
                               const void* group_hdr,
                               const void* iface_hdr) {
    const void* group_octets = cajeta_octets_of(group_hdr);
    const void* iface_octets = cajeta_octets_of(iface_hdr);
    if (fd < 0 || !group_octets) return -1;
    struct ip_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    memcpy(&mreq.imr_multiaddr, group_octets, 4);
    if (iface_octets) {
        memcpy(&mreq.imr_interface, iface_octets, 4);
    }                                   // else zeroed = INADDR_ANY
    int r = setsockopt(cajeta_net_from_fd(fd), IPPROTO_IP, optname,
#if defined(_WIN32)
                       (const char*) &mreq,
#else
                       &mreq,
#endif
                       (cajeta_socklen_t) sizeof(mreq));
    return r == CAJETA_SOCKET_ERROR ? -1 : 0;
}

int32_t __cajeta_net_mcast_join_v4(int32_t fd, const void* group_hdr,
                                   const void* iface_hdr) {
    return cajeta_mcast_v4(fd, IP_ADD_MEMBERSHIP, group_hdr, iface_hdr);
}
int32_t __cajeta_net_mcast_leave_v4(int32_t fd, const void* group_hdr,
                                    const void* iface_hdr) {
    return cajeta_mcast_v4(fd, IP_DROP_MEMBERSHIP, group_hdr, iface_hdr);
}

// Join/leave an IPv6 group. `group_hdr` = int8[] header holding 16
// network-order bytes; `iface_index` = interface index, 0 for the kernel
// default. Returns 0 / -1.
static int32_t cajeta_mcast_v6(int32_t fd, int optname,
                               const void* group_hdr,
                               int32_t iface_index) {
    const void* group_octets = cajeta_octets_of(group_hdr);
    if (fd < 0 || !group_octets || iface_index < 0) return -1;
    struct ipv6_mreq mreq;
    memset(&mreq, 0, sizeof(mreq));
    memcpy(&mreq.ipv6mr_multiaddr, group_octets, 16);
    mreq.ipv6mr_interface = (unsigned int) iface_index;
    int r = setsockopt(cajeta_net_from_fd(fd), IPPROTO_IPV6, optname,
#if defined(_WIN32)
                       (const char*) &mreq,
#else
                       &mreq,
#endif
                       (cajeta_socklen_t) sizeof(mreq));
    return r == CAJETA_SOCKET_ERROR ? -1 : 0;
}

int32_t __cajeta_net_mcast_join_v6(int32_t fd, const void* group_hdr,
                                   int32_t iface_index) {
    return cajeta_mcast_v6(fd, IPV6_JOIN_GROUP, group_hdr, iface_index);
}
int32_t __cajeta_net_mcast_leave_v6(int32_t fd, const void* group_hdr,
                                    int32_t iface_index) {
    return cajeta_mcast_v6(fd, IPV6_LEAVE_GROUP, group_hdr, iface_index);
}

// IPv4 u_char-payload option set/get (the TTL/LOOP width quirk — see header).
static int32_t cajeta_mcast_set_v4_uchar(int32_t fd, int optname, int32_t value) {
    if (fd < 0) return -1;
#if defined(_WIN32)
    return cajeta_opt_set_int(fd, IPPROTO_IP, optname, (int) value);
#else
    unsigned char v = (unsigned char) value;
    int r = setsockopt(cajeta_net_from_fd(fd), IPPROTO_IP, optname,
                       &v, (cajeta_socklen_t) sizeof(v));
    return r == CAJETA_SOCKET_ERROR ? -1 : 0;
#endif
}
static int32_t cajeta_mcast_get_v4_uchar(int32_t fd, int optname) {
    if (fd < 0) return -1;
#if defined(_WIN32)
    int v = 0;
    if (cajeta_opt_get_int(fd, IPPROTO_IP, optname, &v) != 0) return -1;
    return (int32_t) v;
#else
    unsigned char v = 0;
    cajeta_socklen_t len = (cajeta_socklen_t) sizeof(v);
    int r = getsockopt(cajeta_net_from_fd(fd), IPPROTO_IP, optname, &v, &len);
    return r == CAJETA_SOCKET_ERROR ? -1 : (int32_t) v;
#endif
}

// Multicast TTL / hop limit for OUTBOUND multicast (distinct from the unicast
// IP_TTL / IPV6_UNICAST_HOPS above). Default 1 = link-local, per the RFCs.
int32_t __cajeta_net_set_mcast_ttl(int32_t fd, int32_t is_v6, int32_t ttl) {
    if (ttl < 0 || ttl > 255) return -1;
    if (is_v6) {
        return cajeta_opt_set_int(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, (int) ttl);
    }
    return cajeta_mcast_set_v4_uchar(fd, IP_MULTICAST_TTL, ttl);
}
int32_t __cajeta_net_get_mcast_ttl(int32_t fd, int32_t is_v6) {
    if (is_v6) {
        int v = 0;
        if (cajeta_opt_get_int(fd, IPPROTO_IPV6, IPV6_MULTICAST_HOPS, &v) != 0) return -1;
        return (int32_t) v;
    }
    return cajeta_mcast_get_v4_uchar(fd, IP_MULTICAST_TTL);
}

// Multicast loopback — whether this host's own group sends are delivered back
// to local members (OS default: on).
int32_t __cajeta_net_set_mcast_loop(int32_t fd, int32_t is_v6, int32_t on) {
    if (is_v6) {
        return cajeta_opt_set_bool(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP, on);
    }
    return cajeta_mcast_set_v4_uchar(fd, IP_MULTICAST_LOOP, on ? 1 : 0);
}
int32_t __cajeta_net_get_mcast_loop(int32_t fd, int32_t is_v6) {
    if (is_v6) {
        return cajeta_opt_get_bool(fd, IPPROTO_IPV6, IPV6_MULTICAST_LOOP);
    }
    int32_t v = cajeta_mcast_get_v4_uchar(fd, IP_MULTICAST_LOOP);
    return v < 0 ? -1 : (v != 0 ? 1 : 0);
}

// Outbound multicast interface. Same v4-by-address / v6-by-index split as the
// membership calls.
int32_t __cajeta_net_set_mcast_if_v4(int32_t fd, const void* iface_hdr) {
    const void* iface_octets = cajeta_octets_of(iface_hdr);
    if (fd < 0 || !iface_octets) return -1;
    struct in_addr addr;
    memset(&addr, 0, sizeof(addr));
    memcpy(&addr, iface_octets, 4);
    int r = setsockopt(cajeta_net_from_fd(fd), IPPROTO_IP, IP_MULTICAST_IF,
#if defined(_WIN32)
                       (const char*) &addr,
#else
                       &addr,
#endif
                       (cajeta_socklen_t) sizeof(addr));
    return r == CAJETA_SOCKET_ERROR ? -1 : 0;
}
// Writes the 4 network-order octets of the current outbound interface into
// the int8[] whose header is `iface_hdr_out` (0.0.0.0 = kernel default).
// Returns 0 / -1.
int32_t __cajeta_net_get_mcast_if_v4(int32_t fd, void* iface_hdr_out) {
    void* iface_octets_out = iface_hdr_out ? ((uint8_t*) iface_hdr_out) + 8
                                           : (void*) 0;
    if (fd < 0 || !iface_octets_out) return -1;
    struct in_addr addr;
    memset(&addr, 0, sizeof(addr));
    cajeta_socklen_t len = (cajeta_socklen_t) sizeof(addr);
    int r = getsockopt(cajeta_net_from_fd(fd), IPPROTO_IP, IP_MULTICAST_IF,
#if defined(_WIN32)
                       (char*) &addr,
#else
                       &addr,
#endif
                       &len);
    if (r == CAJETA_SOCKET_ERROR) return -1;
    memcpy(iface_octets_out, &addr, 4);
    return 0;
}
int32_t __cajeta_net_set_mcast_if_v6(int32_t fd, int32_t iface_index) {
    if (iface_index < 0) return -1;
    return cajeta_opt_set_int(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, (int) iface_index);
}
int32_t __cajeta_net_get_mcast_if_v6(int32_t fd) {
    int v = 0;
    if (cajeta_opt_get_int(fd, IPPROTO_IPV6, IPV6_MULTICAST_IF, &v) != 0) return -1;
    return (int32_t) v;
}

// The socket's address family, read portably off the kernel via getsockname
// (SO_DOMAIN is Linux-only). Returns 1 for AF_INET6, 0 for AF_INET, -1 on
// error. The Cajeta option surface uses this for the is_v6 dispatch instead of
// trusting wrapper-side state that does not exist (UdpSocket stores only fd).
int32_t __cajeta_net_sockname_is_v6(int32_t fd) {
    if (fd < 0) return -1;
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    cajeta_socklen_t len = (cajeta_socklen_t) sizeof(ss);
    if (getsockname(cajeta_net_from_fd(fd), (struct sockaddr*) &ss, &len)
            == CAJETA_SOCKET_ERROR) {
#if defined(_WIN32)
        // POSIX answers getsockname on an UNBOUND socket with a zeroed address
        // of the right family; Winsock refuses it with WSAEINVAL, which made
        // this probe return -1 for every fresh UDP socket on Windows
        // (NetMulticastOptionsTests.roundTripAndErrors, release full sweep
        // 2026-09-06). The family is still knowable without binding: it is
        // fixed at socket() time and Winsock exposes it through
        // SO_PROTOCOL_INFO's iAddressFamily. Fall back to that on exactly the
        // unbound case so a genuine bad-fd error still reports -1.
        if (WSAGetLastError() == WSAEINVAL) {
            WSAPROTOCOL_INFOW info;
            int ilen = (int) sizeof(info);
            memset(&info, 0, sizeof(info));
            if (getsockopt(cajeta_net_from_fd(fd), SOL_SOCKET, SO_PROTOCOL_INFOW,
                           (char*) &info, &ilen) == 0) {
                if (info.iAddressFamily == AF_INET6) return 1;
                if (info.iAddressFamily == AF_INET)  return 0;
            }
        }
#endif
        return -1;
    }
    if (ss.ss_family == AF_INET6) return 1;
    if (ss.ss_family == AF_INET)  return 0;
    return -1;
}
