# `cajeta.io` — Byte substrate + stream abstractions

`cajeta.io` is the umbrella for everything that crosses the boundary between
the program and the outside world. Its direct members are the shared
abstractions that files, pipes, the network and subprocesses all use. Those are
buffers, views and streams. The concrete I/O kinds live in nested subpackages,
so a file-only program does not pull in a TLS stack.

The package is designed and not yet implemented. It is tracked in
specs/Features.md.

## Subpackages

- [`io/file`](file/File.md) — files, paths, directories, watchers.
- [`io/pipe`](Pipes.md) — anonymous and named pipes. This is the byte-level
  transport that crosses a process boundary. For in-process streaming use
  `Stream<T>` below.
- network — see the `cajeta.io.net` transport stack
  ([`net/Networking.md`](net/Networking.md)). HTTP, WebSocket and SSE are not
  part of the stdlib. They live in the
  [cajeta-http](https://github.com/jklappenbach/cajeta-http) library.
- subprocess — see [`cajeta.process`](../process/Process-design.md).

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

`Buffer` wraps a single `int8[]`. `BufferChain` is the linked list the harness
uses, and it multiple-inherits `Stream<Buffer>` for traversal.

### Owned pooled buffers

For high-throughput I/O, buffers come from a pool and are owned. A connection
borrows one for the duration of a read or write and returns it, and the
borrow-checker drops it deterministically, so there is no GC pause and latency
stays predictable. A completion-based reactor backend (IOCP or io_uring)
borrows the buffer for the in-flight operation. The buffer must outlive the
operation and must not be touched until completion, and that requirement is
expressed as an ownership constraint on the pooled buffer. Readiness backends
(epoll and kqueue) do not have it. See
[`net/Networking.md`](net/Networking.md) § reactor.

## Views — zero-copy structured reads

A view is a structured overlay over a buffer's bytes, and it is a borrow rather
than a copy. Reading a field reads the buffer directly, and constructing the
view allocates nothing. Because a view is a borrow, the borrow-checker
guarantees it cannot outlive the buffer it overlays. You get zero-copy parsing
that is safe by construction, with no manual refcount and no `release()` call
to forget. See `Views.md` for the view-type model.

Serde therefore has two modes, picked at the use site:

| Mode | Shape | Lifetime | Use |
|---|---|---|---|
| **view** | `View<T>` — a borrow over the buffer | bounded to the buffer | hot path, forwarding, large payloads — zero allocation |
| **materialize** | owned `#T` — decoded/copied | independent; can outlive the buffer | when you keep or mutate it |

This is the data plane shared by network framing and any other structured byte
source. The HTTP/2, WebSocket and raw-TCP framers all decode headers as views
over a pooled buffer.

## `Stream<T>` — the streaming model

`Stream<T>` is the one sequence abstraction across all of I/O, and it is backed
by fibers rather than by reactive streams. Cajeta fibers are cheap, and a fiber
can block on a stream step without holding an OS thread. So `Stream<T>` reads
as ordinary synchronous code with no function coloring. There is no
`Mono`/`Flux` and no `suspend`/async split, and every operator is a normal
function:

```cajeta
for (Order o : orders) { ... }                 // suspends the fiber, not the carrier
Stream<Tick> recent = ticks.filter(t -> t.live).map(parse).window(Duration.ofSeconds(1));
```

- **Backpressure** falls out of a bounded `Channel<T>` (`cajeta.concurrent`)
  underneath. A full channel parks the producer fiber, and there is no
  request-`n` protocol.
- **Operators** compose. `map`, `filter`, `flatMap`, `window`, `merge`, `zip`
  and `fold` are each an ordinary function over the upstream stream.
- **Element ownership** is explicit, and it is spelled per call rather than in
  the type. A `Stream<T>` moves an owned element when the producer surrenders
  it at the store site (`s.emit(#x)`). A `Stream<View<T>>` yields borrows that
  are valid only for that iteration step, which gives zero-copy streaming of
  large data because the borrow cannot escape the step. This owned-versus-
  borrowed distinction is the linchpin of the model, and it is a property of
  the call and never of the stream's type.
- **Relationship to neighbours.** `Stream<T>` is the high-level composable
  layer over `Channel<T>`, the raw fiber-to-fiber conduit. It pairs with the
  byte-level `InputStream`/`OutputStream`, and a byte stream is
  `Stream<View<bytes>>` after framing.

Interaction shapes are stream topologies, and that is why this substrate
unifies networking's request/response, duplex and push patterns.
Request/response is a `Stream` of length 1. Server-push, SSE and multicast are
an outbound `Stream<T>`. A duplex connection such as WebSocket or raw TCP is
`(Stream<In>) -> Stream<Out>`. Those protocol bindings live in cajeta-http and
cajeta.io.net, and the stream model itself is here.

## `InputStream` / `OutputStream` / `Reader` / `Writer`

These are Java-shaped byte and char abstractions over buffers. The same
interfaces back files, sockets, subprocess stdio and in-memory streams, so
generic code such as a compressor or a parser works over any source.

```cajeta
InputStream src = someFile.inputStream();
Buffer buf = heap Buffer(8192);
while (src.read(buf) > 0) { dst.write(buf); }
```

## Open items

All of `cajeta.io` is unimplemented. That covers Buffer, BufferChain, views,
`Stream<T>` and InputStream/OutputStream/Reader/Writer. It lands with the fiber
reactor and the first concrete I/O subpackage that needs it, and it is tracked
in specs/Features.md.

## See also

- `cajeta.io.file` — the one built subpackage today
  ([`io/file/File.md`](file/File.md), [`io/file/Path.md`](file/Path.md)).
- `cajeta.io.net` — the network transport stack. It is a peer package of
  `cajeta.io` rather than a child, and it holds sockets, DNS, TLS, URI and the
  cross-platform reactor. See [`net/Networking.md`](net/Networking.md). The
  HTTP/WebSocket/SSE application layer is the separate
  [cajeta-http](https://github.com/jklappenbach/cajeta-http) library over it.
- `cajeta.process` — subprocess management ([`Process.md`](../process/Process-design.md)).
