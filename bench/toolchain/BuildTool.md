# Build tool

The cost of `cajeta build` when nothing changed.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `toolchain/build-noop` | 1e2 no-op builds | Gradle and Maven no-op, cargo build no-op, go build no-op, ninja no-op | exit 0 |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: toolchain/build-noop
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench build-noop               # Cajeta
./bench.sh --bench build-noop --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/toolchain/`
