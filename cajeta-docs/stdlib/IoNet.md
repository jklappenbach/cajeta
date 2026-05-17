# `cajeta.io.net` — TCP / UDP / TLS / HTTP / WebSocket

Lands with the fiber reactor / server harness. Surface sketch.

Status: **designed, not implemented**. Tracked in Features.md.

## Shared abstractions

```cajeta
public interface Address { }
public final value class InetAddress4 implements Address { ... }
public final value class InetAddress6 implements Address { ... }
public final value class SocketAddress {
    Address host;
    int32 port;
}

public interface Socket extends InputStream, OutputStream {
    public SocketAddress localAddress();
    public SocketAddress remoteAddress();
    public void close();
}

public interface ServerSocket {
    public Socket accept();             // fiber-parks
    public SocketAddress localAddress();
    public void close();
}

public final class Selector { ... }     // reactor surface
```

Generic code (a TLS wrapper, an HTTP client, a proxy) programs
against `Socket` / `ServerSocket` without caring which protocol
backs it.

## `cajeta.io.net.tcp`

```cajeta
public final class TcpSocket implements Socket { ... }
public final class TcpServerSocket implements ServerSocket { ... }
public final class TcpListener { ... }   // listen/bind config
```

## `cajeta.io.net.udp`

```cajeta
public final class UdpSocket { ... }
public final value class DatagramPacket { ... }
```

## `cajeta.io.net.tls`

```cajeta
public final class TlsContext { ... }    // certs, ciphers
public final class TlsSocket implements Socket { ... }
public final value class Certificate { ... }
public final value class PrivateKey { ... }
```

## `cajeta.io.net.http`

Full design in `cajeta-docs/stdlib/Networking.md`. Surface: `HttpClient`,
`HttpRequest`, `HttpResponse`, `HttpServer`, `Route`, `ServerMode`
(FIBER_PER_CONNECTION / EVENT_DRIVEN / HYBRID).

## `cajeta.io.net.websocket`

Full design in `cajeta-docs/stdlib/Networking.md`. Surface:
`WebSocketClient`, `WebSocketServer`, `WebSocket`, `Frame`. Uses the
HTTP upgrade handshake.

## Open items

All of `cajeta.io.net` is unimplemented. Tracked in Features.md.
