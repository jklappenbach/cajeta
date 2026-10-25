# Start-up and footprint

`cajeta-http` — time to the first served request and idle resident memory.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `http/startup-time` | `exec` to first 200 response, 20 launches | Go `net/http`, Rust `axum`, Java Netty, Node.js `http`, Python `uvicorn` | response |
| `http/idle-rss` | resident set after start-up with no load | Go `net/http`, Rust `axum`, Java Netty, Node.js `http`, Python `uvicorn` | process alive |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: http/startup-time
metric: time
```

```bench
id: http/idle-rss
metric: memory
```

## Reproduce

```sh
cd bench
./bench.sh --bench startup-time               # Cajeta
./bench.sh --bench startup-time --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/http/`
- Library: `cajeta-http`
