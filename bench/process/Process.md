# Process

`cajeta.process.Process` — a running child and its pipes.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `process/process-pipe-100mb` | pipe 100 MB through `cat` and read it back | same | checksum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: process/process-pipe-100mb
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench process-pipe-100mb               # Cajeta
./bench.sh --bench process-pipe-100mb --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Process](../../docs/stdlib/process/Process.md)
- Source: `runtime/src/cajeta/process/Process.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/process/`
