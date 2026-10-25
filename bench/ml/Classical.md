# Classical models

`dev.cajeta.ml` — the scikit-learn role over `Tensor<float64>`: fit and predict for the estimators a data scientist reaches for first, on synthetic data at 1e5 × 100 and on the sklearn digits and California housing sets. Each row's metric is checked against scikit-learn's on the same data within a recorded tolerance.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `ml/linear-regression` | fit and predict; variants synthetic, housing | scikit-learn | R² within tolerance |
| `ml/logistic-regression` | fit and predict; variants synthetic, digits | scikit-learn | accuracy within tolerance |
| `ml/kmeans` | k = 10 to convergence; variants | scikit-learn | inertia within tolerance |
| `ml/knn` | k = 5 predict over 1e4 queries | scikit-learn | accuracy within tolerance |
| `ml/pca` | 20 components | scikit-learn | explained variance within tolerance |
| `ml/standard-scaler` | fit and transform 1e5 × 100 | scikit-learn | within epsilon |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: ml/linear-regression
metric: time
```

```bench
id: ml/logistic-regression
metric: time
```

```bench
id: ml/kmeans
metric: time
```

```bench
id: ml/knn
metric: time
```

```bench
id: ml/pca
metric: time
```

```bench
id: ml/standard-scaler
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench linear-regression               # Cajeta
./bench.sh --bench linear-regression --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/ml/`
- Library: `cajeta-ml`, benchmark code in `cajeta-ml/bench/`
