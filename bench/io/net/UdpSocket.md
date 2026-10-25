# UdpSocket

`cajeta.io.net.UdpSocket` — datagrams.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `io/udp-send-rate` | send 1e6 512-byte datagrams over loopback and count receipts | C++, Rust, Go, Java, Python | received count |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: io/udp-send-rate
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench udp-send-rate               # Cajeta
./bench.sh --bench udp-send-rate --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [UdpSocket](../../../docs/stdlib/io/net/UdpSocket.md)
- Source: `runtime/src/cajeta/io/net/UdpSocket.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/io/`
