# Neural models

`dev.cajeta.ml` — the torch role over `Tensor<float32>`: a training step and inference for an MLP, a small CNN and a transformer block at batch 64, on the CPU and on the device, plus the autodiff overhead and checkpoint load.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `ml/mlp-train-step` | 784-512-10 MLP, one step; variants cpu, device | PyTorch (CPU, CUDA/ROCm) | loss decreases over 100 steps |
| `ml/mlp-infer` | inference at batch 64 | PyTorch | logits within epsilon |
| `ml/cnn-train-step` | two-conv CNN on 28×28, one step | PyTorch | loss decreases |
| `ml/cnn-infer` | inference at batch 64 | PyTorch | within epsilon |
| `ml/transformer-block-train` | one encoder block, d = 512, seq 128, one step | PyTorch | loss decreases |
| `ml/transformer-block-infer` | inference | PyTorch | within epsilon |
| `ml/autodiff-overhead` | backward over forward ratio for the MLP | PyTorch autograd | gradients within epsilon |
| `ml/checkpoint-load` | load a torch checkpoint of the MLP, 1e2 times | `torch.load` | weights equal |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: ml/mlp-train-step
metric: throughput
```

```bench
id: ml/mlp-infer
metric: throughput
```

```bench
id: ml/cnn-train-step
metric: throughput
```

```bench
id: ml/cnn-infer
metric: throughput
```

```bench
id: ml/transformer-block-train
metric: throughput
```

```bench
id: ml/transformer-block-infer
metric: throughput
```

```bench
id: ml/autodiff-overhead
metric: time
```

```bench
id: ml/checkpoint-load
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench mlp-train-step               # Cajeta
./bench.sh --bench mlp-train-step --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `cajeta-ml/bench/ and cajeta-xgboost/bench/`
- Library: `cajeta-ml`
