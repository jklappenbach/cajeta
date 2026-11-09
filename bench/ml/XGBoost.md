# XGBoost

`dev.cajeta.xgboost` is gradient-boosted trees, measured against upstream XGBoost. The rows are histogram fit on the CPU, fit on the device, and predict.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `ml/xgb-hist-fit` | hist, 1e6 × 50, 100 rounds, depth 6 | XGBoost `hist` | logloss within tolerance |
| `ml/xgb-gpu-fit` | the same on the device | XGBoost `gpu_hist` (CUDA on Phoenix, ROCm on Proton) | logloss within tolerance |
| `ml/xgb-predict` | predict 1e6 rows | XGBoost | predictions within epsilon |

## Results

Each benchmark has one chart, drawn for the machine selected at the top of the page. Languages are the bars, best first. Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: ml/xgb-hist-fit
metric: time
```

```bench
id: ml/xgb-gpu-fit
metric: time
```

```bench
id: ml/xgb-predict
metric: throughput
```

## Standing

Fitting on the GPU is at parity with xgboost's gpu_hist (0.99x). On the cpu the histogram fit is 57x and prediction 56x behind xgboost's multi-threaded tree builder. Not yet investigated.

## Reproduce

```sh
cd bench
./bench.sh --bench xgb-hist-fit               # Cajeta
./bench.sh --bench xgb-hist-fit --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver. [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `cajeta-ml/bench/ and cajeta-xgboost/bench/`
- Library: `cajeta-xgboost`, benchmark code in `cajeta-xgboost/bench/`
