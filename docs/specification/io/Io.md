# `cajeta.io` — Byte substrate + stream abstractions

Umbrella for everything that crosses the program / outside-world boundary. Direct
members are the shared abstractions — **buffers**, **views**, **streams** — used by
files, pipes, network, and subprocess alike; concrete I/O kinds live in nested
subpackages so a file-only program doesn't drag in a TLS stack.

Status: **designed, not implemented**. Tracked in specs/Features.md.

## Subpackages

- [`io/file`](file/File.md) — files, paths, directories, watchers.
- [`io/pipe`](Pipes.md) — anonymous + named pipes (the byte-level, process-crossing
  transport; for in-process streaming use `Stream<T>` below).
- network — see the **`cajeta.io.net`** transport stack
  ([`net/Networking.md`](net/Networking.md)). HTTP/WS/SSE are *not* stdlib — they
  live in the [cajeta-http](https://github.com/jklappenbach/cajeta-http) library.
- subprocess — see [`cajeta.process`](../lang/Process.md).

## `Buffer` + `BufferChain` — the byte substrate

```cajeta
public class Buffer {
    public Buffer(int64 capacity);
    public int64 capacity(); public int8[] data();
    public Buffer next(); public void linkNext(Buffer next);
}
public class BufferChain {
    public void append(Buffer b); public Buffer head(); public int64 totalSize();
}
```

`Buffer` wraps a single `int8[]`; `BufferChain` is the linked list used by the
harness (multiple-inherits `Stream<Buffer>` for traversal).

### Owned pooled buffers

For high-throughput I/O, buffers come from a **pool** and are **owned** — a
connection borrows one for the duration of a read/write and returns it; the
borrow-checker drops it deterministically (no GC pause, predictable latency). A
**completion-based** reactor backend (IOCP / io_uring) borrows the buffer *for the
in-flight operation*: it must outlive the op and not be touched until completion —
expressed as an ownership constraint on the pooled buffer. Readiness backends
(epoll / kqueue) don't have it. (See [`net/Networking.md`](net/Networking.md) § reactor.)

## Views — zero-copy structured reads

A **view** is a structured overlay over a buffer's bytes — **a borrow, not a copy**.
Reading a field reads the buffer directly; constructing the view allocates nothing.
Because a view is a borrow, the borrow-checker **guarantees it cannot outlive the
buffer it overlays** — zero-copy parsing that is safe *by construction* (no
manual refcount / `release()` footguns). See `Views.md` for the view-type model.

Serde therefore has two modes, picked at the use site:

| Mode | Shape | Lifetime | Use |
|---|---|---|---|
| **view** | `View<T>` — a borrow over the buffer | bounded to the buffer | hot path, forwarding, large payloads — zero allocation |
| **materialize** | owned `#T` — decoded/copied | independent; can outlive the buffer | when you keep or mutate it |

This is the data plane shared by network framing (HTTP/2, WebSocket, raw-TCP
framers all decode headers as views over a pooled buffer) and any other structured
byte source.

## `Stream<T>` — the streaming model

The unifying sequence abstraction across all of I/O — **fiber-backed, not
reactive-streams.** Because Cajeta fibers are cheap and a fiber can *block* on a
stream step without holding an OS thread, `Stream<T>` reads as ordinary synchronous
code with **no function coloring** (no `Mono`/`Flux`, no `suspend`/async split —
every operator is a normal function):

```cajeta
for (Order o : orders) { ... }                 // suspends the fiber, not the carrier
Stream<Tick> recent = ticks.filter(t -> t.live).map(parse).window(Duration.ofSeconds(1));
```

- **Backpressure** falls out of a bounded `Channel<T>` (`cajeta.concurrent`)
  underneath: a full channel parks the producer fiber. No request-`n` protocol.
- **Operators** compose: `map` / `filter` / `flatMap` / `window` / `merge` / `zip`
  / `fold`. Each is an ordinary function over the upstream stream.
- **Element ownership** is explicit: `Stream<#T>` *moves* owned elements through
  the pipeline; `Stream<View<T>>` yields **borrows valid only for that iteration
  step** (zero-copy streaming of large data; the borrow can't escape the step).
  This owned-vs-borrowed distinction is the linchpin of the model.
- **Relationship to neighbours:** `Stream<T>` is the high-level composable layer
  over `Channel<T>` (the raw fiber-to-fiber conduit) and pairs with byte-level
  `InputStream`/`OutputStream` (a byte stream is `Stream<View<bytes>>` after framing).

**Interaction shapes are stream topologies** — the reason this substrate unifies
networking's request/response, duplex, and push patterns: request/response is a
`Stream` of length 1, server-push/SSE/multicast is an outbound `Stream<T>`, a
duplex connection (WebSocket, raw TCP) is `(Stream<In>) -> Stream<Out>`. (Those
protocol bindings live in cajeta-http / cajeta.io.net; the stream *model* is here.)

## `InputStream` / `OutputStream` / `Reader` / `Writer`

Java-shaped byte/char abstractions over buffers; the same interfaces back files,
sockets, subprocess stdio, and in-memory streams, so generic code (compress,
parse) works over any source.

```cajeta
InputStream src = someFile.inputStream();
Buffer buf = heap Buffer(8192);
while (src.read(buf) > 0) { dst.write(buf); }
```

## Open items

All of `cajeta.io` (Buffer, BufferChain, views, `Stream<T>`, InputStream/
OutputStream/Reader/Writer) is unimplemented. Lands with the fiber reactor and the
first concrete I/O subpackage that needs it. Tracked in specs/Features.md.

## See also

- `cajeta.io.file` — the one built subpackage today
  ([`io/file/File.md`](file/File.md), [`io/file/Path.md`](file/Path.md)).
- **`cajeta.io.net`** — the network *transport* stack (a **peer package** of
  `cajeta.io`, not a child: sockets, DNS, TLS, URI, the cross-platform reactor).
  See [`net/Networking.md`](net/Networking.md). The HTTP/WebSocket/SSE *application*
  layer is the separate [cajeta-http](https://github.com/jklappenbach/cajeta-http)
  library over it.
- `cajeta.process` — subprocess management ([`Process.md`](../lang/Process.md)).
