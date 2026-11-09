# Components

`cajeta.aot` is the compile-time dependency-injection graph. Resolving a hundred components at start-up is the cost a service pays before its first request. The competitors resolve the same graph at runtime.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `aot/di-graph-100-startup` | start a process with a 100-component graph, 10 deep, generated for every framework from one description; the row is time to the first injected call | Spring (Java), Dagger (compile-time, Java), Guice, Go `wire`; Rust `shaku` skips (its compile time grows exponentially with depth and did not finish) | component count |

## Results

Each benchmark has one chart, drawn for the machine selected at the top of the page. Languages are the bars, best first. Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: aot/di-graph-100-startup
metric: time
```

## Standing

Starting a 100-component graph takes 1.6 ms, 2.7x Go's wire, which generates the wiring at build time. Dagger takes 20x longer than Cajeta, Guice 100x and Spring 170x. wire generates its wiring at build time and Cajeta resolves the graph at start-up; a generated-wiring mode is not planned.

## Reproduce

```sh
cd bench
./bench.sh --bench di-graph-100-startup               # Cajeta
./bench.sh --bench di-graph-100-startup --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver. [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Components](../../docs/stdlib/aot/Components.md)
- Source: `runtime/src/cajeta/aot/`
- Benchmark source: `bench/src/main/cajeta/bench/aot/`
