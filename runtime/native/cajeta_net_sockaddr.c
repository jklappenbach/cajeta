// cajeta.net — NET-1.2 native `sockaddr` marshalling intrinsics.
//
// This translation unit is **#included once** at the bottom of
// `cajeta_runtime.c` (the same single-TU → bitcode → embed build path the
// NET-1.1 socket intrinsics ride; see the header of `cajeta_net_socket.c`).
// No CMake change to the bitcode-embed path is required.
//
// Scope of NET-1.2 (per plan/cajeta-net-plan.md): the two `sockaddr` ↔ Cajeta
// marshalling intrinsics the address types lower to —
//
//   __cajeta_net_sockaddr_pack   (family, octets, port)  -> sockaddr bytes
//   __cajeta_net_sockaddr_unpack (sockaddr bytes)         -> family, octets, port
//
// These sit between the pure-Cajeta `IpAddress`/`SocketAddress` types (which
// hold a family ordinal + a 16-byte network-order octet buffer + a host-order
// port) and the raw `struct sockaddr_in` / `struct sockaddr_in6` the NET-1.1
// socket calls (`bind`/`connect`/`sendto`/`accept`/`recvfrom`) consume and
// produce. The cajeta layer never sees a platform `AF_*` constant — that
// mapping lives here, behind the stable cajeta `AddressFamily` ordinal.
//
// **AddressFamily ordinal contract** (mirrors AddressFamily.cajeta):
//     0  = V4  -> AF_INET   / sockaddr_in
//     1  = V6  -> AF_INET6  / sockaddr_in6
// `AF_INET6` differs across OSes (10 Linux, 23 Windows, 30 macOS), which is
// exactly why the ordinal is portable and the constant is resolved here.
//
// **Byte order.** The octet buffer is in network byte order (the order the
// text form prints, most-significant-first), so packing the address bytes is
// a straight `memcpy` into `sin_addr` / `sin6_addr`. The port crosses the
// cajeta boundary in **host** order and is `htons`/`ntohs`-converted here.

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN 1
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#else
#  include <arpa/inet.h>
#  include <netinet/in.h>
#  include <sys/socket.h>
#endif

#include <stdint.h>
#include <string.h>

// Cajeta AddressFamily ordinals (must match AddressFamily.cajeta).
#define CAJETA_AF_V4 0
#define CAJETA_AF_V6 1

// ---------------------------------------------------------------------------
// Pack a cajeta address into a platform `sockaddr`.
//
//   family   : cajeta AddressFamily ordinal (0 = V4, 1 = V6).
//   octets   : network-order address bytes — 4 read for V4, 16 for V6.
//   port     : host-order port (0..65535).
//   out      : caller buffer that receives the sockaddr bytes.
//   out_cap  : capacity of `out` in bytes.
//
// Returns the number of bytes written (sizeof(sockaddr_in) for V4,
// sizeof(sockaddr_in6) for V6), or -1 on a bad argument / undersized buffer.
// The returned length is exactly the `addrlen` the NET-1.1 socket intrinsics
// (`__cajeta_net_bind`/`_connect`/`_sendto`) expect.
// ---------------------------------------------------------------------------
int32_t __cajeta_net_sockaddr_pack(int32_t family,
                                   const void* octets,
                                   int32_t port,
                                   void* out,
                                   int32_t out_cap) {
    if (!octets || !out || port < 0 || port > 65535) {
        return -1;
    }
    if (family == CAJETA_AF_V4) {
        if (out_cap < (int32_t) sizeof(struct sockaddr_in)) {
            return -1;
        }
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t) port);
        // sin_addr is a 32-bit network-order field; the 4 octets are already
        // network order, so a straight copy is correct.
        memcpy(&sa.sin_addr, octets, 4);
        memcpy(out, &sa, sizeof(sa));
        return (int32_t) sizeof(sa);
    } else if (family == CAJETA_AF_V6) {
        if (out_cap < (int32_t) sizeof(struct sockaddr_in6)) {
            return -1;
        }
        struct sockaddr_in6 sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin6_family = AF_INET6;
        sa.sin6_port = htons((uint16_t) port);
        // sin6_addr.s6_addr is a 16-byte network-order array.
        memcpy(&sa.sin6_addr, octets, 16);
        memcpy(out, &sa, sizeof(sa));
        return (int32_t) sizeof(sa);
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Unpack a platform `sockaddr` into the cajeta address pieces.
//
//   addr        : the sockaddr bytes (e.g. from accept / recvfrom / getsockname).
//   addrlen     : the valid length of `addr`.
//   octets_out  : caller buffer of >= 16 bytes; receives the network-order
//                 address bytes (4 written for V4, 16 for V6).
//   port_out    : receives the host-order port.
//
// Returns the cajeta AddressFamily ordinal (0 = V4, 1 = V6) on success, or -1
// for a NULL/short buffer or an unrecognized `sa_family`. The cajeta layer
// reconstructs an `IpAddress` from (returned family, octets_out) and a
// `SocketAddress` with `*port_out`.
// ---------------------------------------------------------------------------
int32_t __cajeta_net_sockaddr_unpack(const void* addr,
                                     int32_t addrlen,
                                     void* octets_out,
                                     int32_t* port_out) {
    if (!addr || !octets_out || !port_out) {
        return -1;
    }
    // Need at least the common header to read sa_family — `sockaddr` is the
    // shortest of the address structs and carries the family field at its
    // front, shared by sockaddr_in / sockaddr_in6.
    if (addrlen < (int32_t) sizeof(struct sockaddr)) {
        return -1;
    }
    // Read the family out of the common prefix. Both sockaddr_in and
    // sockaddr_in6 begin with the `sa_family` field at offset 0.
    const struct sockaddr* sa = (const struct sockaddr*) addr;
    if (sa->sa_family == AF_INET) {
        if (addrlen < (int32_t) sizeof(struct sockaddr_in)) {
            return -1;
        }
        const struct sockaddr_in* in4 = (const struct sockaddr_in*) addr;
        memcpy(octets_out, &in4->sin_addr, 4);
        *port_out = (int32_t) ntohs(in4->sin_port);
        return CAJETA_AF_V4;
    } else if (sa->sa_family == AF_INET6) {
        if (addrlen < (int32_t) sizeof(struct sockaddr_in6)) {
            return -1;
        }
        const struct sockaddr_in6* in6 = (const struct sockaddr_in6*) addr;
        memcpy(octets_out, &in6->sin6_addr, 16);
        *port_out = (int32_t) ntohs(in6->sin6_port);
        return CAJETA_AF_V6;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// Size helpers — let the cajeta layer size its sockaddr scratch buffers
// without hard-coding platform struct sizes (sockaddr_in6 padding varies).
// `__cajeta_net_sockaddr_storage_size()` is the max of the two, suitable for
// a one-size-fits-all `accept`/`recvfrom` out buffer.
// ---------------------------------------------------------------------------
int32_t __cajeta_net_sockaddr_v4_size(void) {
    return (int32_t) sizeof(struct sockaddr_in);
}

int32_t __cajeta_net_sockaddr_v6_size(void) {
    return (int32_t) sizeof(struct sockaddr_in6);
}

int32_t __cajeta_net_sockaddr_storage_size(void) {
    return (int32_t) sizeof(struct sockaddr_storage);
}
