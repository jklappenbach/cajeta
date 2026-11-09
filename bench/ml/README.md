# dev.cajeta.ml and dev.cajeta.xgboost

This section measures classical estimators against scikit-learn, neural models against PyTorch, and gradient-boosted trees against upstream XGBoost.

| Page | |
|---|---|
| [Classical models](Classical.md) | The scikit-learn role over Tensor<float64>: fit and predict for the estimators a data scientist reaches for first, on synthetic data at 1e5 × 100 and on the sklearn digits and California housing sets |
| [Neural models](Neural.md) | The torch role over Tensor<float32>: a training step and inference for an MLP, a small CNN and a transformer block at batch 64, on the CPU and on the device, plus the autodiff overhead and checkpoint load |
| [XGBoost](XGBoost.md) | Gradient-boosted trees against upstream XGBoost: histogram fit on the CPU, fit on the device, and predict |

## Standing

Geometric mean 19x behind over 36 benchmarks, 2 wins and 34 losses, in two halves. On the Radeon 8060S the elementwise ops are at parity with PyTorch and maxpool wins; softmax, layernorm, cross-entropy and the model steps are 1.5x to 5.4x behind (cajeta-ml-gpu 28.2.4 to 28.2.6); XGBoost's GPU fit is at parity. On the cpu every row is 6x to 4800x behind torch's 32-thread kernels and xgboost's tree builder, and linear and logistic regression, kNN, PCA, the transformer block and the matmul op did not finish within their cap; these are the cpu tensor paths (benchmark-reference 22.2.8), the cpu parity spec's subject.
