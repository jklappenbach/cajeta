# Rotation

`cajeta.math.Rotation` — rotations as quaternions and matrices.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `math/rotation-compose` | 1e8 quaternion compositions and vector rotations | C++ Eigen, Rust `glam`, Go, Java `JOML` | checksum |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: math/rotation-compose
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench rotation-compose               # Cajeta
./bench.sh --bench rotation-compose --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Rotation](../../docs/stdlib/math/Rotation.md)
- Source: `runtime/src/cajeta/math/Rotation.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/math/`
