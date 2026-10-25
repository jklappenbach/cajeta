# Server

`cajeta.io.net.Server` — the reactor-driven server. One thousand idle connections cost memory and wake-ups, not CPU; the row shows both.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `io/server-1k-idle` | hold 1e3 idle connections for 10 s; the row is resident memory and CPU time | Rust tokio, Go net, Java NIO, Python asyncio | connection count |
| `io/server-echo-throughput` | 64 clients echoing 1 KB messages for 10 s | same | message count |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: io/server-1k-idle
metric: memory
```

```bench
id: io/server-echo-throughput
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench server-1k-idle               # Cajeta
./bench.sh --bench server-1k-idle --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Server](../../../docs/stdlib/io/net/Server.md)
- Source: `runtime/src/cajeta/io/net/Server.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/io/`
