# TLS

`cajeta-http` — handshake rate through `dev.cajeta.crypto`.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `http/tls-handshake-rate` | 1e4 fresh TLS 1.3 handshakes | Go `crypto/tls`, Rust `rustls`, Java JSSE, Node, Python `ssl` | all complete |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: http/tls-handshake-rate
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench tls-handshake-rate               # Cajeta
./bench.sh --bench tls-handshake-rate --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/http/`
- Library: `cajeta-http`
