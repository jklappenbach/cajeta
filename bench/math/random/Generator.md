# Generator

`cajeta.math.random.Generator` — pseudo-random generation.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `math/random-uniform` | 1e8 uniform float32 draws (`Generator` yields 24-bit uniforms; a fresh tensor per trial, there is no out-parameter form) | NumPy `Generator`, Rust `rand`, Go `math/rand/v2`, Java `RandomGenerator`, C++ `std::mt19937_64` | mean and variance |
| `math/random-normal` | 1e8 normal float32 draws | same | mean and variance |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

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
