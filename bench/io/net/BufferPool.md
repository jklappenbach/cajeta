# BufferPool

`cajeta.io.net.BufferPool` — pooled buffers for the reactor.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `io/bufferpool-acquire-release` | 1e7 acquire and release of 64 KB buffers | Netty `PooledByteBufAllocator`, Rust `bytes::BytesMut` reuse, Go `sync.Pool` | count |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: io/bufferpool-acquire-release
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench bufferpool-acquire-release               # Cajeta
./bench.sh --bench bufferpool-acquire-release --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [BufferPool](../../../docs/stdlib/io/net/BufferPool.md)
- Source: `runtime/src/cajeta/io/net/BufferPool.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/io/`
