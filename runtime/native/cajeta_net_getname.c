// cajeta.io.net NET-1.3 socket-name query intrinsics (`getsockname`/`getpeername`).
// Textually #included from cajeta_runtime.c AFTER cajeta_net_socket.c, whose fd
// typedefs and `cajeta_net_from_fd` widener this file reuses rather than redeclares.

// Write the LOCAL address `fd` is bound to into the caller's sockaddr scratch buffer
// `addr_out`, whose capacity arrives in `*addrlen_inout`, and set `*addrlen_inout` to
// the actual length. Returns 0, or -1 with the cause in `__cajeta_net_last_error()`.
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

// Write the REMOTE (peer) address `fd` is connected to, same out-parameter contract.
// An unconnected socket fails ENOTCONN/WSAENOTCONN. Returns 0 / -1.
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
