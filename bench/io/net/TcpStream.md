# TcpStream

`cajeta.io.net.TcpStream` — a connected TCP socket. Echo latency over loopback is the per-request floor of every service; bulk throughput is the copy path.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `io/tcp-echo-latency` | 1e5 64-byte echo round trips over loopback; the row times the batch and the per-round-trip median is in the notes | C++ POSIX sockets, Rust std and tokio, Go net, Java NIO, Python asyncio | byte equality |
| `io/tcp-bulk-throughput` | stream 4 GB over loopback in 64 KB writes | same | byte count |

The peer runs in-process on another fiber; the competitor programs do the same on a thread.

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: io/tcp-echo-latency
metric: time
```

```bench
id: io/tcp-bulk-throughput
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench tcp-echo-latency               # Cajeta
./bench.sh --bench tcp-echo-latency --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [TcpStream](../../../docs/stdlib/io/net/TcpStream.md)
- Source: `runtime/src/cajeta/io/net/TcpStream.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/io/`
