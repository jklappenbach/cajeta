# Ray

`cajeta.math.Ray` — ray intersection, the inner loop of a tracer.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `math/ray-sphere` | 1e8 ray-sphere tests | C++, Rust `glam`, Go, Java `JOML` | hit count |
| `math/ray-aabb` | 1e8 ray-box tests | same | hit count |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: math/ray-sphere
metric: throughput
```

```bench
id: math/ray-aabb
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench ray-sphere               # Cajeta
./bench.sh --bench ray-sphere --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Ray](../../docs/stdlib/math/Ray.md)
- Source: `runtime/src/cajeta/math/Ray.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/math/`
