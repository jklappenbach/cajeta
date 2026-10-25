# Poly

`cajeta.math.poly.Poly` — polynomial evaluation and fitting.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `math/poly-eval` | evaluate a degree-8 polynomial at 1e8 points | NumPy `polyval`, Rust, Go, Java, C++ Horner | checksum |
| `math/poly-fit` | least-squares fit of degree 8 to 1e6 points, 1e2 times (a Vandermonde tensor through `LinAlg.lstsq`; `Poly` defers fitting) | NumPy `polyfit`, Eigen, `nalgebra`, `gonum` | coefficients within epsilon |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: math/poly-eval
metric: throughput
```

```bench
id: math/poly-fit
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench poly-eval               # Cajeta
./bench.sh --bench poly-eval --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Poly](../../../docs/stdlib/math/poly/Poly.md)
- Source: `runtime/src/cajeta/math/poly/Poly.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/math/`
