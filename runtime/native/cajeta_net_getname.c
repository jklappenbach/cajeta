// cajeta.net — NET-1.3 native socket-name query intrinsics
// (`getsockname` / `getpeername`).
//
// This translation unit is **#included once** at the bottom of
// `cajeta_runtime.c` (the same single-TU → bitcode → embed build path the
// NET-1.1 socket intrinsics ride; see the header of `cajeta_net_socket.c`).
// No CMake change to the bitcode-embed path is required — only the one
// `#include "cajeta_net_getname.c"` line alongside the existing
// `#include "cajeta_net_socket.c"` / `cajeta_net_sockaddr.c` lines.
//
// Scope: the two address-query syscalls `TcpStream.localAddress()` /
// `peerAddress()` (and `TcpListener.localAddress()`, `UdpSocket.localAddress()`)
// lower to. NET-1.1 deliberately stopped at the transfer primitives and left
// these for NET-1.3 — see the comment in `test/expression/NetSocketTests.cpp`
// (`boundPort` "calls the platform fn since the intrinsic for it is NET-1.3").
//
// **ABI.** Identical shape to the `addr_out`/`addrlen_inout` out-parameter
// convention `__cajeta_net_accept` / `__cajeta_net_recvfrom` already use: the
// caller passes a sockaddr scratch buffer sized via
// `__cajeta_net_sockaddr_storage_size()` and its capacity; on success the
// kernel writes the address bytes and the actual length is returned through
// `*addrlen_inout`. The Cajeta layer then unmarshals via
// `__cajeta_net_sockaddr_unpack` (NET-1.2) into an `IpAddress` + port. Returns
// 0 on success or -1 on failure (the caller reads `__cajeta_net_last_error()`).
//
// No new platform abstraction is introduced: both calls exist verbatim in
// BSD sockets and Winsock with the same `(SOCKET, sockaddr*, socklen_t*)`
// signature; only the handle width + the `socklen_t` spelling differ, which
// the shared typedefs in `cajeta_net_socket.c` already normalize. Because this
// file is textually included **after** `cajeta_net_socket.c`, those typedefs
// (`cajeta_native_socket_t`, `cajeta_socklen_t`, `CAJETA_SOCKET_ERROR`) and the
// `cajeta_net_from_fd` widener are already in scope — we reuse them rather than
// re-declare, keeping one definition of the fd ABI.

// `getsockname` — write the **local** address `fd` is bound to into
// `addr_out[0..*addrlen_inout)` (a caller sockaddr scratch buffer) and update
// `*addrlen_inout` to the actual length. For a connected socket this is the
// local endpoint (the ephemeral port the kernel assigned on connect/bind);
// `TcpStream.localAddress()` and `TcpListener.localAddress()` lower here.
// Returns 0 / -1.
int32_t __cajeta_net_getsockname(int32_t fd, void* addr_out, int32_t* addrlen_inout) {
    if (fd < 0 || !addr_out || !addrlen_inout || *addrlen_inout <= 0) {
        return -1;
    }
    cajeta_socklen_t len = (cajeta_socklen_t) *addrlen_inout;
    int r = getsockname(cajeta_net_from_fd(fd),
                        (struct sockaddr*) addr_out, &len);
    if (r == CAJETA_SOCKET_ERROR) {
        return -1;
    }
    *addrlen_inout = (int32_t) len;
    return 0;
}

// `getpeername` — write the **remote** (peer) address `fd` is connected to
// into `addr_out[0..*addrlen_inout)` and update `*addrlen_inout`.
// `TcpStream.peerAddress()` lowers here. On an unconnected socket the platform
// fails with ENOTCONN/WSAENOTCONN, which surfaces as -1 + a mapped errno.
// Returns 0 / -1.
int32_t __cajeta_net_getpeername(int32_t fd, void* addr_out, int32_t* addrlen_inout) {
    if (fd < 0 || !addr_out || !addrlen_inout || *addrlen_inout <= 0) {
        return -1;
    }
    cajeta_socklen_t len = (cajeta_socklen_t) *addrlen_inout;
    int r = getpeername(cajeta_net_from_fd(fd),
                        (struct sockaddr*) addr_out, &len);
    if (r == CAJETA_SOCKET_ERROR) {
        return -1;
    }
    *addrlen_inout = (int32_t) len;
    return 0;
}
