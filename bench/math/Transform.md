# Transform

`cajeta.math.Transform` — affine transforms.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `math/transform-apply` | apply a 4 × 4 transform to 1e8 points | Eigen, `glam`, Go, `JOML`, NumPy | checksum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: math/transform-apply
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench transform-apply               # Cajeta
./bench.sh --bench transform-apply --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Transform](../../docs/stdlib/math/Transform.md)
- Source: `runtime/src/cajeta/math/Transform.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/math/`
