// cajeta.io.net — NET-2.1 native name resolution (`getaddrinfo`), exposed as an
// opaque result handle plus scalar accessors. #included once at the bottom of
// cajeta_runtime.c AFTER cajeta_net_socket.c, whose ensure_init() it calls.

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
#  include <netdb.h>
#endif

#include <stdint.h>
#include <stdio.h>    // snprintf
#include <stdlib.h>
#include <string.h>

// Cajeta AddressFamily ordinals (must match AddressFamily.cajeta).
#ifndef CAJETA_AF_V4
#  define CAJETA_AF_V4 0
#endif
#ifndef CAJETA_AF_V6
#  define CAJETA_AF_V6 1
#endif

// Normalized resolve-error ordinals: `getaddrinfo` fails in its own EAI_* space,
// disjoint from the socket errno space, and the cajeta Dns layer switches on
// these. Append-only — never renumber.
enum cajeta_resolve_err {
    CAJETA_RESOLVE_OK          = 0,   // resolved (>= 1 address)
    CAJETA_RESOLVE_NONAME      = 1,   // host not found / no such name  -> UnknownHost
    CAJETA_RESOLVE_NODATA      = 2,   // name valid but no address of the requested family
    CAJETA_RESOLVE_AGAIN       = 3,   // transient failure, retry may succeed
    CAJETA_RESOLVE_FAIL        = 4,   // non-recoverable failure
    CAJETA_RESOLVE_FAMILY      = 5,   // requested family unsupported
    CAJETA_RESOLVE_MEMORY      = 6,   // out of memory
    CAJETA_RESOLVE_SYSTEM      = 7,   // system error (errno set)
    CAJETA_RESOLVE_BADFLAGS    = 8,   // invalid hints/flags
    CAJETA_RESOLVE_SERVICE     = 9,   // service/port not supported
    CAJETA_RESOLVE_OTHER       = 99   // any unmapped EAI_* code
};

// Thread-local last resolve error, set by every __cajeta_net_getaddrinfo call
// (to OK on success) — the per-thread `errno` analogue for resolution.
static __thread int32_t g_cajeta_resolve_errno = CAJETA_RESOLVE_OK;

// Map a platform `getaddrinfo` EAI_* return code to a cajeta_resolve_err.
static int32_t cajeta_map_eai(int eai) {
#if defined(_WIN32)
    // Winsock maps EAI_* onto WSA* codes (EAI_NONAME == WSAHOST_NOT_FOUND, etc.).
    switch (eai) {
        case WSAHOST_NOT_FOUND: return CAJETA_RESOLVE_NONAME;
        case WSANO_DATA:        return CAJETA_RESOLVE_NODATA;
        case WSATRY_AGAIN:      return CAJETA_RESOLVE_AGAIN;
        case WSANO_RECOVERY:    return CAJETA_RESOLVE_FAIL;
        case WSAEAFNOSUPPORT:   return CAJETA_RESOLVE_FAMILY;
        case WSA_NOT_ENOUGH_MEMORY: return CAJETA_RESOLVE_MEMORY;
        case WSAEINVAL:         return CAJETA_RESOLVE_BADFLAGS;
        case WSATYPE_NOT_FOUND: return CAJETA_RESOLVE_SERVICE;
        default:                return CAJETA_RESOLVE_OTHER;
    }
#else
    switch (eai) {
        case EAI_NONAME:   return CAJETA_RESOLVE_NONAME;
#  ifdef EAI_NODATA
        case EAI_NODATA:   return CAJETA_RESOLVE_NODATA;
#  endif
        case EAI_AGAIN:    return CAJETA_RESOLVE_AGAIN;
        case EAI_FAIL:     return CAJETA_RESOLVE_FAIL;
        case EAI_FAMILY:   return CAJETA_RESOLVE_FAMILY;
        case EAI_MEMORY:   return CAJETA_RESOLVE_MEMORY;
        case EAI_SYSTEM:   return CAJETA_RESOLVE_SYSTEM;
        case EAI_BADFLAGS: return CAJETA_RESOLVE_BADFLAGS;
        case EAI_SERVICE:  return CAJETA_RESOLVE_SERVICE;
        default:           return CAJETA_RESOLVE_OTHER;
    }
#endif
}

// The runtime-owned result block: one entry per resolved address, pre-parsed to
// the cajeta triple so the platform addrinfo list is freed before we return.
struct cajeta_addr_entry {
    int32_t family;       // cajeta ordinal: 0 = V4, 1 = V6
    int32_t port;         // host-order port
    uint8_t octets[16];   // network-order address bytes (4 used for V4)
};

struct cajeta_addr_result {
    int32_t count;
    struct cajeta_addr_entry* entries;
};

// Resolve `host[0..host_len)` (an int8[] header, empty = loopback) and `port`
// under a family filter (-1 both, 0 V4, 1 V6) into an opaque handle the caller
// frees with __cajeta_net_freeaddrinfo. NULL on failure or zero addresses.
void* __cajeta_net_getaddrinfo(const void* host, int32_t host_len,
                               int32_t port, int32_t family) {
    if (host_len < 0 || port < 0 || port > 65535) {
        g_cajeta_resolve_errno = CAJETA_RESOLVE_BADFLAGS;
        return NULL;
    }
    // Windows needs WSAStartup before getaddrinfo; the once-guard is idempotent.
    if (!cajeta_net_ensure_init()) {
        g_cajeta_resolve_errno = CAJETA_RESOLVE_SYSTEM;
        return NULL;
    }

    // DNS caps a name at 253 chars, so 256 holds any legal hostname plus NUL.
    char hostbuf[256];
    const char* hostarg = NULL;
    if (host_len > 0) {
        if (host_len > (int32_t) sizeof(hostbuf) - 1) {
            g_cajeta_resolve_errno = CAJETA_RESOLVE_NONAME;  // can't be a real name
            return NULL;
        }
        // The @Native ABI passes an int8[] header { i64 count, [N x i8] data },
        // so the host bytes start at +8.
        memcpy(hostbuf, (const uint8_t*) host + 8, (size_t) host_len);
        hostbuf[host_len] = '\0';
        hostarg = hostbuf;
    }

    // A decimal service string; AI_NUMERICSERV keeps us out of /etc/services.
    char servbuf[8];
    snprintf(servbuf, sizeof(servbuf), "%d", port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    if (family == CAJETA_AF_V4) {
        hints.ai_family = AF_INET;
    } else if (family == CAJETA_AF_V6) {
        hints.ai_family = AF_INET6;
    } else {
        hints.ai_family = AF_UNSPEC;   // both
    }
    // The socktype filter collapses the otherwise-duplicated STREAM/DGRAM/RAW
    // triples into one address per (family, ip).
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICSERV;

    struct addrinfo* res = NULL;
    int rc = getaddrinfo(hostarg, servbuf, &hints, &res);
    if (rc != 0) {
        g_cajeta_resolve_errno = cajeta_map_eai(rc);
        return NULL;
    }

    int32_t n = 0;
    for (struct addrinfo* ai = res; ai != NULL; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET || ai->ai_family == AF_INET6) {
            n++;
        }
    }
    if (n == 0) {
        freeaddrinfo(res);
        g_cajeta_resolve_errno = CAJETA_RESOLVE_NODATA;
        return NULL;
    }

    struct cajeta_addr_result* block =
        (struct cajeta_addr_result*) malloc(sizeof(struct cajeta_addr_result));
    struct cajeta_addr_entry* entries =
        (struct cajeta_addr_entry*) calloc((size_t) n, sizeof(struct cajeta_addr_entry));
    if (!block || !entries) {
        free(block);
        free(entries);
        freeaddrinfo(res);
        g_cajeta_resolve_errno = CAJETA_RESOLVE_MEMORY;
        return NULL;
    }

    int32_t idx = 0;
    for (struct addrinfo* ai = res; ai != NULL && idx < n; ai = ai->ai_next) {
        if (ai->ai_family == AF_INET) {
            const struct sockaddr_in* in4 = (const struct sockaddr_in*) ai->ai_addr;
            entries[idx].family = CAJETA_AF_V4;
            entries[idx].port = port;   // host order, as requested
            memcpy(entries[idx].octets, &in4->sin_addr, 4);
            idx++;
        } else if (ai->ai_family == AF_INET6) {
            const struct sockaddr_in6* in6 = (const struct sockaddr_in6*) ai->ai_addr;
            entries[idx].family = CAJETA_AF_V6;
            entries[idx].port = port;
            memcpy(entries[idx].octets, &in6->sin6_addr, 16);
            idx++;
        }
    }
    freeaddrinfo(res);

    block->count = idx;
    block->entries = entries;
    g_cajeta_resolve_errno = CAJETA_RESOLVE_OK;
    return block;
}

// Per-entry accessors: three calls rather than one out-param call, because every
// @Native parameter is a scalar, a `pointer` or an `int8[]`. All bounds-check
// and return a sentinel rather than faulting on a caller bug.

// The cajeta AddressFamily ordinal (0 = V4, 1 = V6) of the `index`-th address,
// or -1 for a NULL handle / out-of-range index.
int32_t __cajeta_net_getaddrinfo_family(void* handle, int32_t index) {
    if (!handle || index < 0) {
        return -1;
    }
    struct cajeta_addr_result* block = (struct cajeta_addr_result*) handle;
    if (index >= block->count) {
        return -1;
    }
    return block->entries[index].family;
}

// The host-order port of the `index`-th address (the port baked in at resolve
// time), or -1 for a NULL handle / out-of-range index.
int32_t __cajeta_net_getaddrinfo_port(void* handle, int32_t index) {
    if (!handle || index < 0) {
        return -1;
    }
    struct cajeta_addr_result* block = (struct cajeta_addr_result*) handle;
    if (index >= block->count) {
        return -1;
    }
    return block->entries[index].port;
}

// Copy the `index`-th address's network-order octets into `octets_out` (an
// int8[] of >= 16 bytes), returning the count written — 4 for V4, 16 for V6 —
// or -1 for a NULL handle / NULL buffer / out-of-range index.
int32_t __cajeta_net_getaddrinfo_octets(void* handle, int32_t index,
                                        void* octets_out) {
    if (!handle || !octets_out || index < 0) {
        return -1;
    }
    struct cajeta_addr_result* block = (struct cajeta_addr_result*) handle;
    if (index >= block->count) {
        return -1;
    }
    struct cajeta_addr_entry* e = &block->entries[index];
    int32_t n = (e->family == CAJETA_AF_V4) ? 4 : 16;
    // int8[] header again: the data region starts at +8.
    memcpy((uint8_t*) octets_out + 8, e->octets, (size_t) n);
    return n;
}

// Release a result block. NULL-safe, so the cajeta drop path can call it
// unconditionally after a zero-result resolve.
void __cajeta_net_freeaddrinfo(void* handle) {
    if (!handle) {
        return;
    }
    struct cajeta_addr_result* block = (struct cajeta_addr_result*) handle;
    free(block->entries);
    free(block);
}

// The cajeta_resolve_err ordinal of this thread's last __cajeta_net_getaddrinfo
// call; the Dns layer switches on it to pick UnknownHost vs ResolutionFailed.
int32_t __cajeta_net_getaddrinfo_error(void) {
    return g_cajeta_resolve_errno;
}

// Address count of a handle; 0 for NULL, so the iteration loop is self-bounding.
int32_t __cajeta_net_getaddrinfo_count(void* handle) {
    if (!handle) {
        return 0;
    }
    return ((struct cajeta_addr_result*) handle)->count;
}
