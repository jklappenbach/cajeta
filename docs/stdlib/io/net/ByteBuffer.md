# ByteBuffer

`cajeta.io.net.ByteBuffer` — the pooled byte buffer with independent read and
write cursors. Three regions over one backing array: consumed `[0, readPos)`,
readable `[readPos, writePos)` and writable `[writePos, capacity)`. A parser
fills the writable tail from a channel, scans the readable region in place,
and advances the read cursor past what it consumed. When the readable region
drains, `compact` or `clear` reopens the writable region from the front. A
buffer is held by one fiber at a time, so it has no lock.

```cajeta
ByteBuffer b = heap ByteBuffer(4096);
int8[] head = heap int8[3];
head[0] = (int8) 71; head[1] = (int8) 69; head[2] = (int8) 84;
int32 w = b.write(head, 0, 3);
int8 first = b.at(b.readPosition());    // scan in place: 71, the G of GET
b.advanceRead(3);                       // consumed; readable() is 0 again
```

Over a connection the same buffer is filled and drained without a copy:

<!-- snippet: skip -->
```cajeta
int32 got = b.fillAsync(conn);          // socket bytes land in the writable tail
b.writeToAsync(conn);                   // the readable region, one write
```

## Methods

| Signature | |
|---|---|
| `ByteBuffer(int32 capacity)` | A buffer with `capacity` writable bytes and both cursors at the front |
| `int32 capacity()` / `int32 readable()` / `int32 writable()` | Sizes of the whole store, the readable region, the writable region |
| `int32 readPosition()` / `int32 writePosition()` | The two cursors |
| `int8 at(int32 i)` | The byte at absolute index `i`, unchecked |
| `int8[] array()` | The backing store, a borrow; index it with the cursors. It is replaced when `reserve` grows the buffer |
| `boolean put(int8 b)` | Append one byte; false when the writable region is full |
| `int32 write(int8[] src, int32 srcOff, int32 len)` | Copy into the writable region; returns the count that fit |
| `int32 read(int8[] dst, int32 dstOff, int32 len)` | Copy out of the readable region and consume it |
| `int32 fillAsync(ByteChannel ch)` | One channel read straight into the writable tail, parking the fiber; returns the count, 0 at EOF or when nothing is writable, in which case the channel is not touched |
| `int32 fillWithin(ByteChannel ch, int32 timeoutMs)` | The same under a deadline. Raises `TimedOutException` when it elapses |
| `void writeToAsync(ByteChannel ch)` | Send the readable region in one write and consume it; nothing readable means no write |
| `void advanceWrite(int32 n)` / `void advanceRead(int32 n)` | Move a cursor after filling or scanning in place, clamped to the region |
| `void compact()` | Slide the readable bytes to the front so the writable region reopens |
| `void clear()` | Both cursors to the front, content discarded, store kept |
| `void reserve(int32 minWritable)` | Compact, then grow the store only if that is not enough |

## See also

- [BufferPool](BufferPool.md) — where a connection's buffers come from
- Source: [`runtime/src/cajeta/io/net/ByteBuffer.cajeta`](../../../../runtime/src/cajeta/io/net/ByteBuffer.cajeta)
