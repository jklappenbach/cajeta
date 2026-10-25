# WebSocket

`cajeta-http` — echo throughput, plain and with permessage-deflate.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `http/ws-echo` | 64 clients echoing 1 KB frames for 10 s | Go `gorilla/websocket`, Rust `tokio-tungstenite`, Java Netty, Node `ws`, Python `websockets` | frame count and equality |
| `http/ws-echo-deflate` | the same with permessage-deflate | same | equality |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: http/ws-echo
metric: throughput
```

```bench
id: http/ws-echo-deflate
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench ws-echo               # Cajeta
./bench.sh --bench ws-echo --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `cajeta-http/bench/`
- Library: `cajeta-http`
