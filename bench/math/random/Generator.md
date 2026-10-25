# Generator

`cajeta.math.random.Generator` — pseudo-random generation.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `math/random-uniform` | 1e8 uniform float64 draws | NumPy `Generator`, Rust `rand`, Go `math/rand/v2`, Java `RandomGenerator`, C++ `std::mt19937_64` | mean and variance |
| `math/random-normal` | 1e8 normal draws | same | mean and variance |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: math/random-uniform
metric: throughput
```

```bench
id: math/random-normal
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench random-uniform               # Cajeta
./bench.sh --bench random-uniform --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Generator](../../../docs/stdlib/math/random/Generator.md)
- Source: `runtime/src/cajeta/math/random/Generator.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/math/`
