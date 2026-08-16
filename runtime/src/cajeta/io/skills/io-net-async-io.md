---
id: io-net-async-io
applies-to: [cajeta/io/net/AsyncReader, cajeta/io/net/AsyncWriter]
title: AsyncReader / AsyncWriter — buffered async byte streaming over a ByteChannel
description: The bufio-style buffered read/write pair the HTTP and WebSocket codecs stream through; turns would-block socket I/O into read/readExact/readUntil + chunk iteration and coalesced flushes.
---

# AsyncReader / AsyncWriter

The buffered streaming pair every HTTP/1.1 and WebSocket codec reads and writes
*through* (plan item NET-3.5). Each wraps a borrowed
[`ByteChannel`](../ByteChannel.cajeta) (a `TcpStream` for `http://`/`ws://`, a
`TlsStream` for `https://`/`wss://`) and bridges the gap between *whatever bytes the
socket has ready* and the *tokens* a codec wants — a CRLF line, an exact
`Content-Length` body, a fixed WS frame header. The Go `bufio` / Rust `BufReader`+
`BufWriter` shape. Every refill/flush parks the **fiber** (not the carrier) on the
reactor, so a reader/writer stalled on a slow peer yields its carrier.

**Use this when** you need token-level reads or coalesced writes over a socket. If
you only need the raw non-blocking primitive, call
[`ByteChannel.readAsync`/`writeAllAsync`](../ByteChannel.cajeta) directly — these add
buffering, nothing else. These do **not** parse HTTP/WS, frame messages, or
own/close the channel.

## Members & roles

- **`AsyncReader`** — owns a [`RingBuffer`](../RingBuffer.cajeta) staging ring + a
  scratch fill array; serves `read` / `readExact` / `readUntil` from the ring,
  refilling from the socket only when it runs dry. Also an
  [`AsyncIterator<int8[]>`](../../concurrent/AsyncIterator.cajeta) (`next()` yields
  chunks).
- **`AsyncWriter`** — owns a coalescing `int8[]`; `writeAll` / `writeByte` /
  `writeString` stage bytes, and only `flush()` (or an auto-flush when the buffer
  fills) puts them on the wire.

They are independent: pair one of each over the *same* channel to drive a connection.

## Ownership & lifecycle (the boundary rules)

- The channel is **borrowed** by both — neither owns nor closes it. The caller that
  opened the socket closes it (after a final `AsyncWriter.flush()`).
- Internal ring / scratch / coalescing buffers are **owned**, array-dropped on drop.
- **Single-fiber ownership**: one reader + one writer drive one connection — no
  internal locking. Do not share an instance across fibers.
- `readUntil` returns a **fresh owned `#int8[]`** (caller owns it). `next()` returns
  a `stack Optional<int8[]>` whose present value is a fresh owned array — the
  interface contract returns a borrow, so this is *not* `#`-marked.

## EOF / terminate-on-zero (the contract that ends every loop)

A socket read of `0` is an orderly peer close; the reader sets a sticky `eof` flag
and, once the ring drains, is terminally at end-of-stream:

- `read` returns `0` (same as the raw channel — copy loops end on `0`).
- `readExact` raises [`NetException`](../NetException.cajeta) if it can't fill the
  whole request (truncated frame/body).
- `readUntil` returns whatever it had, delimiter absent — distinguish "found" from
  "EOF" by inspecting the last byte or `atEnd()`.
- `next()` returns an **empty** `Optional` (terminal thereafter).

## Cross-class call sequence (HTTP-shaped)

1. `reader.readUntil((int8) 10, maxBytes)` per header line until a blank line.
2. `reader.readExact(body, 0, contentLength)` for the body (or iterate `next()` for
   chunked/unbounded streaming, looping while `opt.isPresent()`).
3. `writer.writeString(...)` / `writeAll(...)` to emit the response head + body.
4. `writer.flush()` at each message boundary — buffered bytes are **not** on the
   wire until flush. Forgetting the final flush is the classic buffered-writer bug.
5. Caller closes the channel.

## Key signatures

```cajeta
// reader
int32   read(int8[] dst, int32 off, int32 len)            // 0 == EOF
void    readExact(int8[] dst, int32 off, int32 len)       // raises NetException on short EOF
#int8[] readUntil(int8 delimiter, int32 maxBytes)         // fresh owned; maxBytes<=0 = unbounded
Optional<int8[]> next()                                    // empty == exhausted (terminal)
int32   readWithin(int8[] dst, int32 off, int32 len, int32 timeoutMs)  // NET-9.6 slowloris; raises TimedOutException
int32   stage(int8[] src, int32 len)  /  void markEof()  /  boolean atEnd()  /  int32 buffered()
// writer
void writeAll(int8[] data, int32 off, int32 len)  /  void writeByte(int8 b)  /  void writeString(String s)
void flush()                                              // no-op when nothing pending
int32 pending()
```

Constructors: `AsyncReader(ByteChannel)` / `AsyncReader(ByteChannel, int32 capacity)`
(and the same for `AsyncWriter`); default ring/buffer is `DEFAULT_BUFFER` = 8192. A
non-positive capacity is clamped to 1.

## Worked example (test-backed: NetAsyncStreamTests)

`stage` + `markEof` is the offline test/pre-staging seam — the drain logic is pure
over the ring + sticky `eof`, so you can exercise every path with no live socket.
(The codecs also use `stage` to push back pipelined bytes read past a boundary.)

```cajeta
import cajeta.lang.Optional;
import cajeta.io.net.TcpStream;
import cajeta.io.net.AsyncReader;
import cajeta.io.net.AsyncWriter;

TcpStream t = heap TcpStream(-1);          // borrowed channel (closed-fd sentinel)
AsyncReader r = heap AsyncReader(t, 64);

int8[] src = heap int8[5];
src[0]=(int8)65; src[1]=(int8)66; src[2]=(int8)67; src[3]=(int8)13; src[4]=(int8)10;
r.stage(src, 5);                           // pre-stage "ABC\r\n" ahead of the socket
int8[] line #= r.readUntil((int8) 10, 0);   // fresh owned array, LF included -> len 5

AsyncWriter w = heap AsyncWriter(t, 64);
w.writeString(line == null ? "" : "ok\r\n");
w.flush();                                 // nothing on the wire until here
// caller closes t (after the final flush) — the reader/writer never do.
```

## Gotchas

- **Final flush.** Buffered bytes stay invisible to the peer until `flush()`; codecs
  flush at each message boundary and once more before close.
- **`heap T[this.field]` mistype.** Allocating an array sized by a field or
  static-final mis-lowers the size read as a pointer (invalid-IR verify failure). Bind
  the size to a named `int32` local first — both ctors and `next()` already do.
- **Reuse trap.** Single-fiber only; do not share a reader/writer across fibers.
