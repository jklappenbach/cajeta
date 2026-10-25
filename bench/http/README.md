# cajeta-http

HTTP/1.1 and HTTP/2 serving, WebSocket, server-sent events, TLS, the client, and start-up, against Go, axum, Netty, Node and uvicorn.

| Page | |
|---|---|
| [Server](Server.md) | The TechEmpower plaintext and JSON shapes without a database, over HTTP/1.1 keep-alive and HTTP/2, at 64 and 512 connections. oha drives; wrk cross-checks one row |
| [WebSocket](WebSocket.md) | Echo throughput, plain and with permessage-deflate |
| [Server-sent events](Sse.md) | Fan-out to a thousand subscribers |
| [TLS](Tls.md) | Handshake rate through dev.cajeta.crypto |
| [Client](Client.md) | HttpClient throughput and connection reuse against a local server |
| [Start-up and footprint](Startup.md) | Time to the first served request and idle resident memory |

## Standing

Written after the first published run.
