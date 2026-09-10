// cajeta.io.net — NET-1.6/NET-14.1 typed socket-option intrinsics: every
// platform SOL_*/IPPROTO_*/SO_* constant stays in C. Textually #included by
// cajeta_runtime.c AFTER cajeta_net_socket.c, whose fd-ABI helpers it reuses.

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

// ---- internal helpers: the single int-valued setsockopt/getsockopt funnel --

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

// TCP_NODELAY (IPPROTO_TCP, SOCK_STREAM only) — disable Nagle so small writes
// go out immediately. Set returns 0/-1, get returns 1/0/-1.
int32_t __cajeta_net_set_nodelay(int32_t fd, int32_t on) {
    return cajeta_opt_set_bool(fd, IPPROTO_TCP, TCP_NODELAY, on);
}
int32_t __cajeta_net_get_nodelay(int32_t fd) {
    return cajeta_opt_get_bool(fd, IPPROTO_TCP, TCP_NODELAY);
}

// SO_KEEPALIVE — probe an idle connection so a dead peer is detected. The
// per-probe TCP_KEEPIDLE/INTVL/CNT tuning is non-portable and not exposed.
int32_t __cajeta_net_set_keepalive(int32_t fd, int32_t on) {
    return cajeta_opt_set_bool(fd, SOL_SOCKET, SO_KEEPALIVE, on);
}
int32_t __cajeta_net_get_keepalive(int32_t fd) {
    return cajeta_opt_get_bool(fd, SOL_SOCKET, SO_KEEPALIVE);
}

// SO_RCVBUF / SO_SNDBUF — kernel buffer sizes in bytes. The kernel may round,
// clamp or double the request, so the getter returns the EFFECTIVE size.
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

// SO_LINGER — `on`==0 closes immediately and drains in the background (the
// default); `on`!=0 makes close block up to `seconds` flushing, then RST. The
// one option whose payload is a struct, so it does not use the int helpers.
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

// Read SO_LINGER back into `*on_out` (1/0) and `*seconds_out`; either pointer
// may be NULL to discard that field. Returns 0 / -1.
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

// SO_BROADCAST — permit a SOCK_DGRAM socket to send to a broadcast address;
// without it the kernel rejects the sendto. Off by default.
int32_t __cajeta_net_set_broadcast(int32_t fd, int32_t on) {
    return cajeta_opt_set_bool(fd, SOL_SOCKET, SO_BROADCAST, on);
}
int32_t __cajeta_net_get_broadcast(int32_t fd) {
    return cajeta_opt_get_bool(fd, SOL_SOCKET, SO_BROADCAST);
}

// Unicast hop limit, 0..255: IP_TTL for IPv4, IPV6_UNICAST_HOPS for IPv6, so
// the caller passes the family it created the socket with as `is_v6`. Set
// returns 0/-1, get returns the read-back value or -1.
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

// IPV6_V6ONLY — on, an AF_INET6 socket takes IPv6 only; off, it also accepts
// IPv4-mapped peers. Platform defaults differ, so callers set it explicitly.
int32_t __cajeta_net_set_only_v6(int32_t fd, int32_t on) {
    return cajeta_opt_set_bool(fd, IPPROTO_IPV6, IPV6_V6ONLY, on);
}
int32_t __cajeta_net_get_only_v6(int32_t fd) {
    return cajeta_opt_get_bool(fd, IPPROTO_IPV6, IPV6_V6ONLY);
}

// ---- NET-14.1 UDP multicast options ----------------------------------------
// The family split is asymmetric because the kernels are: IPv4 names the
// interface by ADDRESS (ip_mreq, INADDR_ANY = default), IPv6 by INDEX
// (ipv6_mreq, 0); IPv4's MULTICAST_TTL/LOOP take a u_char where IPv6's take int.

// Octet parameters cross the @Native bridge as cajeta int8[] HEADERS —
// `{ i64 count, [N x i8] data }` — so the bytes live at offset 8. NULL stays NULL.
static const void* cajeta_octets_of(const void* hdr) {
    return hdr ? ((const uint8_t*) hdr) + 8 : (const void*) 0;
}

// Join/leave an IPv4 group: `group_hdr` holds 4 network-order bytes,
// `iface_hdr` the interface address or NULL for INADDR_ANY. Returns 0 / -1.
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
    }
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

// Join/leave an IPv6 group: `group_hdr` holds 16 network-order bytes,
// `iface_index` is the interface index, 0 for the kernel default. Returns 0/-1.
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

// IPv4 u_char-payload option set/get — the TTL/LOOP width quirk noted above.
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

// Multicast TTL / hop limit for OUTBOUND multicast, distinct from the unicast
// IP_TTL / IPV6_UNICAST_HOPS above. Default 1 = link-local, per the RFCs.
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

// Multicast loopback — whether this host's own group sends come back to local
// members (OS default: on).
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

// Outbound multicast interface, with the same v4-by-address / v6-by-index
// split as the membership calls.
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
// Writes the 4 network-order octets of the current outbound interface into the
// int8[] whose header is `iface_hdr_out` (0.0.0.0 = default). Returns 0 / -1.
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

// The socket's address family read off the kernel via getsockname (SO_DOMAIN
// is Linux-only): 1 for AF_INET6, 0 for AF_INET, -1 on error. Drives the
// is_v6 dispatch, since the Cajeta wrappers store only the fd.
int32_t __cajeta_net_sockname_is_v6(int32_t fd) {
    if (fd < 0) return -1;
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    cajeta_socklen_t len = (cajeta_socklen_t) sizeof(ss);
    if (getsockname(cajeta_net_from_fd(fd), (struct sockaddr*) &ss, &len)
            == CAJETA_SOCKET_ERROR) {
#if defined(_WIN32)
        // Winsock refuses getsockname on an UNBOUND socket with WSAEINVAL where
        // POSIX answers with a zeroed address; the family is fixed at socket()
        // time, so read it from SO_PROTOCOL_INFOW on exactly that case.
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
