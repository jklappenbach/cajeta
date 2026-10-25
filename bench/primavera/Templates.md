# Templated page

`primavera` — an HTML page rendered through cajeta-jinja from injected data.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `primavera/templated-page` | `GET /fortunes`-shaped page of 12 rows | Thymeleaf under Spring, Qute under Quarkus, Razor under ASP.NET, Jinja2 under FastAPI | zero errors, markup equal |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: primavera/templated-page
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench templated-page               # Cajeta
./bench.sh --bench templated-page --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/primavera/`
- Library: `cajeta-primavera`
