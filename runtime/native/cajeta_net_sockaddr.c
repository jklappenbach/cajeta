// cajeta.io.net — native `sockaddr` marshalling, #included once at the bottom
// of `cajeta_runtime.c`. The platform `AF_*` constants (AF_INET6 is 10 on
// Linux, 23 on Windows, 30 on macOS) are resolved HERE and nowhere above.

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

// Packs a cajeta address (family ordinal, NETWORK-order `octets`, HOST-order
// `port`) into `out`. Returns the bytes written, which is exactly the `addrlen`
// the socket intrinsics expect, or -1 on a bad argument or short buffer.
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
        // sin_addr is network order, as the octets already are.
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
        memcpy(&sa.sin6_addr, octets, 16);
        memcpy(out, &sa, sizeof(sa));
        return (int32_t) sizeof(sa);
    }
    return -1;
}

// Unpacks `addr` into `octets_out` (>= 16 bytes, network order) and `port_out`
// (host order). Returns the cajeta AddressFamily ordinal, or -1 for a null or
// short buffer or an unrecognized `sa_family`.
int32_t __cajeta_net_sockaddr_unpack(const void* addr,
                                     int32_t addrlen,
                                     void* octets_out,
                                     int32_t* port_out) {
    if (!addr || !octets_out || !port_out) {
        return -1;
    }
    // Both sockaddr_in and sockaddr_in6 begin with `sa_family`, and plain
    // `sockaddr` is the shortest struct carrying it.
    if (addrlen < (int32_t) sizeof(struct sockaddr)) {
        return -1;
    }
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

// Size helpers, so the cajeta layer never hard-codes a platform struct size
// (sockaddr_in6 padding varies). `storage_size` fits either family, which is
// what a one-size `accept` / `recvfrom` out buffer needs.
int32_t __cajeta_net_sockaddr_v4_size(void) {
    return (int32_t) sizeof(struct sockaddr_in);
}

int32_t __cajeta_net_sockaddr_v6_size(void) {
    return (int32_t) sizeof(struct sockaddr_in6);
}

int32_t __cajeta_net_sockaddr_storage_size(void) {
    return (int32_t) sizeof(struct sockaddr_storage);
}
