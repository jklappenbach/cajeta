# LinAlg

`cajeta.math.linalg.LinAlg` — dense linear algebra: solve, inverse, QR and SVD at 512 and 2048.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `math/linalg-solve` | solve `Ax = b` for a random well-conditioned float64 matrix; variants 512, 2048 | SciPy (LAPACK), Eigen, Rust `nalgebra`, `gonum`, EJML | residual norm |
| `math/linalg-inverse` | explicit inverse; variants 512, 2048 | same | `A·A⁻¹` within epsilon |
| `math/linalg-qr` | QR decomposition; variants 512, 2048 | same | `Q·R` within epsilon |
| `math/linalg-svd` | full SVD; variants 512, 2048 | same | reconstruction within epsilon |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: math/linalg-solve
metric: time
```

```bench
id: math/linalg-inverse
metric: time
```

```bench
id: math/linalg-qr
metric: time
```

```bench
id: math/linalg-svd
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench linalg-solve               # Cajeta
./bench.sh --bench linalg-solve --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [LinAlg](../../../docs/stdlib/math/linalg/LinAlg.md)
- Source: `runtime/src/cajeta/math/linalg/LinAlg.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/math/`
