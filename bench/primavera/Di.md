# Dependency injection at scale

`primavera` — a hundred components wired ten deep, the start-up cost of the graph itself.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `primavera/di-graph-100-startup` | 100 components, 10 deep, time to first injected call | Spring, Quarkus (ArC), Micronaut, ASP.NET Core DI, FastAPI `Depends` | component count |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: primavera/di-graph-100-startup
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench di-graph-100-startup               # Cajeta
./bench.sh --bench di-graph-100-startup --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `cajeta-primavera/bench/`
- Library: `cajeta-primavera`
