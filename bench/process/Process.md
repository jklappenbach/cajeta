# Process

`cajeta.process.Process` is a running child and its pipes.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `process/process-pipe-100mb` | pipe 100 MB through `cat` and read it back | same | checksum |

## Results

Each benchmark has one chart, drawn for the machine selected at the top of the page. Languages are the bars, best first. Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: process/process-pipe-100mb
metric: throughput
```

## Standing

Piping 100 MB through a child is a win at 0.47x of Rust's std::process, with posix_spawn 2.7x, Go 3x and Java 6x slower than Cajeta.

## Reproduce

```sh
cd bench
./bench.sh --bench process-pipe-100mb               # Cajeta
./bench.sh --bench process-pipe-100mb --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver. [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Process](../../docs/stdlib/process/Process.md)
- Source: `runtime/src/cajeta/process/Process.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/process/`
