---
id: io-net-buffers
applies-to: [cajeta/io/net/ByteBuffer, cajeta/io/net/RingBuffer, cajeta/io/net/BufferPool]
title: Net buffer staging — ByteBuffer + RingBuffer + BufferPool
description: Pure-logic byte staging for incremental parsers — cursor buffer, watermark backpressure ring, and a bounded reuse pool.
---

# Net buffer staging

Three cooperating **pure-logic** byte-staging types in `cajeta.io.net` — no
syscalls, no native bridge, no locking (each is owned by a single fiber per
connection). They sit *between* a socket read and an incremental parser: a read
fills writable space, the parser consumes readable bytes, drained space is
reclaimed. Pick by job:

| Need | Use |
| --- | --- |
| Stage bytes for a parser with separate read/write cursors, `compact`/grow | `ByteBuffer` |
| Bound memory + signal producer to pause/resume (backpressure) | `RingBuffer` |
| Reuse `ByteBuffer`s across connections without alloc churn | `BufferPool` |

These do **not** do I/O. Nothing here reads or writes a socket — the reactor
does that and then drives these via `advanceWrite`/`write`. For the GPU/SWAR
leaf byte buffer see `cajeta/io/Buffer` (a different package, unrelated).

## Roles & object graph

- **`ByteBuffer`** — the unit of staging and the unit the pool vends. Owns an
  `int8[] data`; three regions over it: `[0,readPos)` consumed, `[readPos,
  writePos)` readable, `[writePos,cap)` writable.
- **`BufferPool`** — a free-list (LIFO stack) of cleared `ByteBuffer`s of one
  fixed `slabSize`. Creates them on `acquire`, owns idle ones between uses.
- **`RingBuffer`** — independent circular buffer; **not** vended by the pool and
  does not contain `ByteBuffer`s. Used where backpressure signalling matters.

`BufferPool` owns/creates `ByteBuffer`; `RingBuffer` is standalone. There is no
type that wraps all three — the reactor wires them.

## Ownership & lifecycle (the part that bites)

- All three own an `int8[]` reclaimed by normal array-drop when the object
  drops. No `close()`/`dispose()` — drop-on-scope only.
- **`BufferPool.acquire()` returns `#ByteBuffer`** — ownership transfers to the
  caller; the pool nulls its slot so it no longer aliases the buffer.
- **`BufferPool.release(#ByteBuffer buf)` takes ownership** — you MUST pass `#b`.
  Without the `#`, your owned reference drops at scope exit while the pool still
  aliases the retained buffer → dangling slot → use-after-free on the next
  `acquire`. A `null` argument is ignored.
- `release` only retains a buffer when there is idle room (`idle() < maxIdle`)
  **and** it is still slab-sized; an oversized buffer (one a parser grew via
  `reserve`) and any overflow buffer are **dropped**, not retained — that is how
  the idle set stays bounded.

## Worked example — pooled staging loop (mirrors BufferPoolTests.reuseStaysBounded)

```cajeta
import cajeta.io.net.BufferPool;
import cajeta.io.net.ByteBuffer;

BufferPool p = heap BufferPool(256, 4);    // 256-byte slabs, keep <=4 idle
int32 i = 0;
while (i < 100) {
    ByteBuffer b #= p.acquire();            // #-returned: b is now sole owner
    b.advanceWrite(10);                     // pretend a 10-byte socket read landed
    // ... parse out of b's readable region, advanceRead past consumed ...
    p.release(#b);                          // # MANDATORY — transfers ownership back
    i = i + 1;
}
// p.allocations() == 1 : only the first acquire allocated; the rest reused.
```

`allocations()` is the boundedness contract: across N **sequential**
acquire/release cycles it stays flat (`<= maxIdle + peakConcurrent`), never
growing with N. Concurrent holds each allocate, so the *live* count can exceed
`maxIdle`; the cap bounds only the *idle* set.

## ByteBuffer — cursor protocol

`heap ByteBuffer(capacity)` (capacity must be > 0). Cursors start at front.
The parser cycle:

1. Fill writable space: either `write(src, srcOff, len)` (copies, returns
   `min(len, writable())`) or `advanceWrite(n)` after a raw fill (clamped to
   writable room — can never pass `cap`).
2. Read: `readable()` count, `at(i)` for absolute-index peeks (no bounds check —
   caller's responsibility), `read(dst, dstOff, len)` to copy out + advance.
3. `advanceRead(n)` past consumed bytes (clamped to readable).
4. When the writable tail runs low but readable isn't drained, `compact()`
   slides unread bytes to front (no-op if `readPos == 0`).
5. `clear()` resets both cursors keeping the array (the reclaim step pooling
   relies on). `reserve(minWritable)` grows the array (compact-then-double) only
   for oversized frames; steady state never grows.

`put`/`write` return `false`/a short count when full — they never grow on their
own; backpressure is the ring's/pool's job.

## RingBuffer — watermark backpressure

`heap RingBuffer(capacity, low, high)`; watermarks are clamped to a sane band
(`high`→`[1,capacity]`, `low`→`[0,high]`). Fixed capacity — **never grows**.

- `write`/`read` wrap modulo cap and return short counts at the boundary.
  `peek(offset)` looks ahead without consuming (caller checks `offset < size()`);
  `skip(n)` consumes without copying.
- Backpressure is hysteresis, reported not enforced: `isOverHighWatermark()`
  (`size >= high`) tells the reactor to pause the socket read registration;
  `isUnderLowWatermark()` (`size <= low`) tells it to resume. **The caller tracks
  the paused/resumed edge itself** — the ring only reports levels.

See `cajeta/io/net/ByteBuffer`, `cajeta/io/net/BufferPool`,
`cajeta/io/net/RingBuffer` for per-method detail.
