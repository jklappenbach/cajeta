// cajeta.net — NET-2.1 native name-resolution intrinsics (`getaddrinfo`).
//
// This translation unit is **#included once** at the bottom of
// `cajeta_runtime.c`, *after* `cajeta_net_socket.c` and
// `cajeta_net_sockaddr.c` (the same single-TU → bitcode → embed build path the
// NET-1.1 socket + NET-1.2 sockaddr intrinsics ride; see the header of
// `cajeta_net_socket.c`). The post-socket ordering matters: this file calls the
// file-static `cajeta_net_ensure_init()` defined in `cajeta_net_socket.c` to
// guarantee `WSAStartup` ran before `getaddrinfo` on Windows. No CMake change
// to the bitcode-embed path is required.
//
// Scope of NET-2.1 (per plan/cajeta-net-plan.md):
//
//   __cajeta_net_getaddrinfo         (host, host_len, port, family) -> result handle
//   __cajeta_net_getaddrinfo_count   (handle)                       -> address count
//   __cajeta_net_getaddrinfo_error   ()                             -> normalized resolve errno
//   __cajeta_net_getaddrinfo_family  (handle, index)                -> cajeta family ordinal
//   __cajeta_net_getaddrinfo_port    (handle, index)                -> host-order port
//   __cajeta_net_getaddrinfo_octets  (handle, index, out)           -> octet bytes written
//   __cajeta_net_freeaddrinfo        (handle)                       -> release the result block
//
// `Dns.resolve` (NET-2.2) is the pure-Cajeta wrapper that drives these; it is
// intentionally NOT here — this file is the native syscall layer only.
//
// ## ABI: a result-handle + scalar accessors (no out-params)
//
// `getaddrinfo` returns a heap-owned linked list (`struct addrinfo*`). The
// cajeta `@Native` bridge can't yet return a freshly heap-allocated `int8[]`
// that registers with the per-thread live-set (the same constraint the SHA-256
// bridge documents), and an address list is variable-length, so a single
// "return the bytes" call is the wrong shape. The bridge also has no
// out-pointer (`pointer*` / `int32*`) form — every `@Native` parameter is a
// scalar, a `pointer`, or an `int8[]`. So the ABI is an **iterator over an
// opaque handle**, every call carrying only those bridgeable parameter kinds:
//
//   1. `__cajeta_net_getaddrinfo` resolves and stashes the parsed results in a
//      runtime-owned `struct cajeta_addr_result` block, **returning the opaque
//      handle as a `pointer`** (NULL on failure / zero results). The cajeta
//      side reads the count + error through the accessors below.
//   2. `__cajeta_net_getaddrinfo_family` / `_port` / `_octets` unpack the Nth
//      result into the exact `(family ordinal, network-order octets,
//      host-order port)` triple that `__cajeta_net_sockaddr_unpack` already
//      produces — so the NET-2.2 layer reconstructs each `IpAddress.fromOctets`
//      + `SocketAddress.of` with the *same* code path it uses for
//      `accept`/`recvfrom`. One unmarshalling contract across the subsystem.
//   3. `__cajeta_net_freeaddrinfo` releases the block (the cajeta `Dns` layer
//      calls this in a `finally`/drop once it has copied every entry out).
//
// The native block holds the addresses **pre-parsed** into the cajeta octet
// form (not the raw `struct addrinfo`), so the platform `getaddrinfo` list is
// freed immediately inside `__cajeta_net_getaddrinfo` — nothing platform-owned
// survives the call, which keeps the handle's lifetime trivially ours.
//
// ## AddressFamily ordinal contract (mirrors AddressFamily.cajeta)
//     0 = V4 -> AF_INET  / sockaddr_in
//     1 = V6 -> AF_INET6 / sockaddr_in6
// Same ordinal the sockaddr pack/unpack uses; the platform `AF_*` constant
// never crosses the cajeta boundary.
//
// ## Family filter (mirrors the NET-2.2 `AddressFamily` filter)
//    -1 = both (AF_UNSPEC)   0 = V4-only (AF_INET)   1 = V6-only (AF_INET6)
//
// ## Byte order
// Octets are emitted in **network** order (the order the text form prints);
// the port crosses the boundary in **host** order — identical to
// `__cajeta_net_sockaddr_unpack`.

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

// ---------------------------------------------------------------------------
// Normalized resolve-error ordinals.
//
// `getaddrinfo` reports failure through its own `EAI_*` return-code space —
// disjoint from the socket `errno`/`WSAGetLastError` space the NET-1.1 shim
// normalizes. We collapse the EAI_* codes into a small stable enum the cajeta
// `Dns` layer (NET-2.5) switches on to pick `UnknownHost` vs `ResolutionFailed`.
// Keep these ordinals append-only; never renumber.
// ---------------------------------------------------------------------------
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

// Thread-local last resolve error, mirroring the socket shim's
// `__cajeta_net_last_error()` design (the per-thread `errno` analogue). Set on
// every `__cajeta_net_getaddrinfo` call (to OK on success). `__thread` is the
// same TLS facility the rest of the runtime relies on.
static __thread int32_t g_cajeta_resolve_errno = CAJETA_RESOLVE_OK;

// Map a platform `getaddrinfo` EAI_* return code to the normalized ordinal.
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

// ---------------------------------------------------------------------------
// The runtime-owned, pre-parsed result block. One entry per resolved address,
// already collapsed to the cajeta `(family, octets, port)` triple so the
// platform `struct addrinfo` list can be freed before this call returns.
// ---------------------------------------------------------------------------
struct cajeta_addr_entry {
    int32_t family;       // cajeta ordinal: 0 = V4, 1 = V6
    int32_t port;         // host-order port
    uint8_t octets[16];   // network-order address bytes (4 used for V4)
};

struct cajeta_addr_result {
    int32_t count;
    struct cajeta_addr_entry* entries;
};

// ---------------------------------------------------------------------------
// Resolve `host[0..host_len)` + `port` into a runtime-owned result block.
//
//   host       : host bytes (NOT necessarily NUL-terminated — we copy + NUL).
//                A NULL host (host_len == 0) resolves the passive/loopback
//                wildcard (AI_PASSIVE-less default: loopback), matching libc.
//   host_len   : length of `host` in bytes.
//   port       : 0..65535 service port baked into every returned address.
//   family     : family filter — -1 both, 0 V4-only, 1 V6-only.
//
// Returns the opaque result-block **handle** (a non-NULL `pointer`) on success,
// or **NULL** on any failure *or* a zero-address resolve. On NULL,
// `__cajeta_net_getaddrinfo_error()` carries the normalized ordinal (it is set
// to `OK` only when a non-NULL handle is returned). The handle's address count
// is read via `__cajeta_net_getaddrinfo_count`; the cajeta side must release it
// with `__cajeta_net_freeaddrinfo` (NULL-safe, so an unconditional drop call is
// fine).
// ---------------------------------------------------------------------------
void* __cajeta_net_getaddrinfo(const void* host, int32_t host_len,
                               int32_t port, int32_t family) {
    if (host_len < 0 || port < 0 || port > 65535) {
        g_cajeta_resolve_errno = CAJETA_RESOLVE_BADFLAGS;
        return NULL;
    }
    // Windows needs WSAStartup before getaddrinfo; reuse the socket shim's
    // idempotent once-guard (this TU is #included after cajeta_net_socket.c,
    // so the file-static is in scope).
    if (!cajeta_net_ensure_init()) {
        g_cajeta_resolve_errno = CAJETA_RESOLVE_SYSTEM;
        return NULL;
    }

    // NUL-terminate the host into a small stack buffer (DNS labels cap the
    // total name at 253 chars; 256 is safely above any legal hostname).
    char hostbuf[256];
    const char* hostarg = NULL;
    if (host_len > 0) {
        if (host_len > (int32_t) sizeof(hostbuf) - 1) {
            g_cajeta_resolve_errno = CAJETA_RESOLVE_NONAME;  // can't be a real name
            return NULL;
        }
        // `host` is a cajeta int8[] header — { i64 count, [N x i8] data }; the
        // host bytes start at +8 (the @Native ABI passes the header, as
        // __cajeta_sha256_update etc. expect).
        memcpy(hostbuf, (const uint8_t*) host + 8, (size_t) host_len);
        hostbuf[host_len] = '\0';
        hostarg = hostbuf;
    }

    // Service as a decimal string; AI_NUMERICSERV keeps getaddrinfo from
    // touching /etc/services. The port is baked into the returned sockaddrs.
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
    // We resolve for connect-style use: stream sockets, numeric service. The
    // socktype filter collapses the otherwise-duplicated SOCK_STREAM /
    // SOCK_DGRAM / SOCK_RAW triples into one address per (family, ip).
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_NUMERICSERV;

    struct addrinfo* res = NULL;
    int rc = getaddrinfo(hostarg, servbuf, &hints, &res);
    if (rc != 0) {
        g_cajeta_resolve_errno = cajeta_map_eai(rc);
        return NULL;
    }

    // Count first so we can size one contiguous entry array.
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

// ---------------------------------------------------------------------------
// Per-entry accessors. The `(family, port, octets)` triple is read by three
// scalar/array calls rather than one out-param call, because every `@Native`
// bridge parameter is a scalar, a `pointer`, or an `int8[]` — there is no
// out-pointer form. Together they reproduce the exact unmarshalling contract of
// `__cajeta_net_sockaddr_unpack`, so the NET-2.2 layer feeds the result into
// the identical `IpAddress.fromOctets` + `SocketAddress.of` path.
//
// All three bounds-check (`NULL` handle / out-of-range index) and return a
// benign sentinel rather than faulting, since the cajeta side iterates
// `0 .. count-1` and a defensive sentinel beats UB on a caller bug.
// ---------------------------------------------------------------------------

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

// Copy the `index`-th address's network-order octets into `octets_out`
// (caller buffer of >= 16 bytes). Writes 4 bytes for a V4 address, 16 for V6,
// and returns the number written (4 or 16), or -1 for a NULL handle / NULL
// buffer / out-of-range index. (The block zero-fills the V4 tail, so a caller
// that always reads 16 is also safe.)
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
    // `octets_out` is a cajeta int8[] header; write the octets into its data
    // region at +8 (same @Native header ABI as the host arg above).
    memcpy((uint8_t*) octets_out + 8, e->octets, (size_t) n);
    return n;
}

// ---------------------------------------------------------------------------
// Release a result block. NULL-safe (a zero-result resolve hands back a NULL
// handle, so the cajeta drop path can call this unconditionally).
// ---------------------------------------------------------------------------
void __cajeta_net_freeaddrinfo(void* handle) {
    if (!handle) {
        return;
    }
    struct cajeta_addr_result* block = (struct cajeta_addr_result*) handle;
    free(block->entries);
    free(block);
}

// ---------------------------------------------------------------------------
// The normalized `cajeta_resolve_err` ordinal of the last
// `__cajeta_net_getaddrinfo` call on this thread (thread-local, like the
// socket shim's `__cajeta_net_last_error`). The NET-2.5 `Dns` layer switches
// on this to raise `UnknownHost` (NONAME/NODATA) vs `ResolutionFailed`.
// ---------------------------------------------------------------------------
int32_t __cajeta_net_getaddrinfo_error(void) {
    return g_cajeta_resolve_errno;
}

// Result count of a handle (convenience; the cajeta side already has the count
// from the resolve return value, but this keeps the handle self-describing for
// the entry-iteration loop without re-threading the count through every call).
int32_t __cajeta_net_getaddrinfo_count(void* handle) {
    if (!handle) {
        return 0;
    }
    return ((struct cajeta_addr_result*) handle)->count;
}
