# cajeta-http

This section measures HTTP/1.1 and HTTP/2 serving, WebSocket, server-sent events, TLS, the client, and start-up, against Go, axum, Netty, Node and uvicorn.

| Page | |
|---|---|
| [Server](Server.md) | The TechEmpower plaintext and JSON shapes without a database, over HTTP/1.1 keep-alive and HTTP/2, at 64 and 512 connections. oha drives and wrk cross-checks one row |
| [WebSocket](WebSocket.md) | Echo throughput, plain and with permessage-deflate |
| [Server-sent events](Sse.md) | Fan-out to a thousand subscribers |
| [TLS](Tls.md) | Handshake rate through dev.cajeta.crypto |
| [Client](Client.md) | HttpClient throughput and connection reuse against a local server |
| [Start-up and footprint](Startup.md) | Time to the first served request and idle resident memory |

## Standing

Not measured yet: the first published run (proton, 2026-11-09) holds no rows for this library. Its bench runs under its own repository and is collected by `bench.sh publish` once it has run on a quiet box.
