# TcpListener

`cajeta.io.net.TcpListener` — accepting connections.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `io/tcp-accept-rate` | accept 1e5 short connections | same stacks as TcpStream | count |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: io/tcp-accept-rate
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench tcp-accept-rate               # Cajeta
./bench.sh --bench tcp-accept-rate --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [TcpListener](../../../docs/stdlib/io/net/TcpListener.md)
- Source: `runtime/src/cajeta/io/net/TcpListener.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/io/`
