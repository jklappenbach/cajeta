# Net.md — retired (split into transport + HTTP library)

> This document has been **split and retired** (2026-06). Its content moved to
> two homes along the transport / application-protocol boundary:
>
> - **Transport** (sockets, TCP/UDP/multicast, the cross-platform reactor, TLS,
>   URI, DNS, addresses, framing, error model, native intrinsics) is the stdlib
>   `cajeta.io.net` layer →
>   **[`specification/io/net/Networking.md`](specification/io/net/Networking.md)**.
>
> - **HTTP/1.1·2·3, WebSocket, SSE** (an *application* protocol over transport —
>   HTTP/3 even rides QUIC/UDP) moved **out of stdlib** into the
>   **[cajeta-http](https://github.com/jklappenbach/cajeta-http)** library; see its
>   `docs/http-spec.md`. A program that only speaks raw TCP/UDP never links HTTP.

The byte/stream substrate those build on (buffers, zero-copy views, `Stream<T>`)
is in [`specification/io/Io.md`](specification/io/Io.md). The phased transport
build order is `agents/cajeta/net/cajeta-net-plan.md`.

Update any links pointing here to the appropriate target above.
