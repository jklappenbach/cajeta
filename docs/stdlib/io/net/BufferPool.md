# BufferPool

`cajeta.io.net.BufferPool` — a bounded free list of same-sized
[ByteBuffer](ByteBuffer.md)s. A server acquires one input and one output
buffer per connection at accept and releases both at close, so a warm server
allocates nothing per request. `allocations()` counts fresh buffers ever
made, which is the gauge a steady-state test asserts flat. Idle buffers past
`maxIdle` are dropped on release rather than kept.

```cajeta
BufferPool pool = heap BufferPool(65536, 64);
ByteBuffer in #= pool.acquire();
ByteBuffer out #= pool.acquire();
pool.release(#in);
pool.release(#out);
```

## Methods

| Signature | |
|---|---|
| `BufferPool(int32 slabSize, int32 maxIdle)` | Every buffer is `slabSize` bytes; at most `maxIdle` sit idle |
| `#ByteBuffer acquire()` | An idle buffer cleared, or a fresh one counted in `allocations()` |
| `void release(#ByteBuffer buf)` | Take the buffer back; kept idle up to `maxIdle`, dropped past it, and a buffer of another size is never kept |
| `int32 bufferSize()` / `int32 idle()` / `int32 maxIdleCount()` | The slab size, the idle count, the idle cap |
| `int32 allocations()` | Fresh buffers made since construction |

## See also

- [ByteBuffer](ByteBuffer.md) — the unit it hands out
- Source: [`runtime/src/cajeta/io/net/BufferPool.cajeta`](../../../../runtime/src/cajeta/io/net/BufferPool.cajeta)
