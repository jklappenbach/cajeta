# Neural models

`dev.cajeta.ml` fills the torch role over `Tensor<float32>`. The rows are a training step and inference for an MLP, a small CNN and a transformer block at batch 64, on the CPU and on the device, plus the autodiff overhead and checkpoint load, and one row per forward op on the same activations.

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

## Ops

One forward of a single `dev.cajeta.ml` op per iteration, on the encoder block's activation (64 × 128 × 512), the CNN's (64 × 16 × 28 × 28) or 8192 × 512 logits; variants cpu, device. The check is the output within a recorded tolerance of torch's on the same inputs.

| id | workload | against | check |
|---|---|---|---|
| `ml/op-add` | elementwise add of two activations | PyTorch | within 1e-6 |
| `ml/op-relu` | ReLU | PyTorch | within 1e-6 |
| `ml/op-gelu` | GELU, erf form | PyTorch | within 1e-5 |
| `ml/op-tanh` | tanh | PyTorch | within 1e-6 |
| `ml/op-sigmoid` | sigmoid | PyTorch | within 1e-6 |
| `ml/op-softmax` | softmax over the last axis | PyTorch | within 1e-6 |
| `ml/op-layernorm` | LayerNorm over the last axis, eps 1e-5 | PyTorch | within 1e-4 |
| `ml/op-matmul` | 8192 × 512 by 512 × 2048 | PyTorch | within 1e-3 |
| `ml/op-conv2d` | 16 → 16 channels, 3 × 3, stride 1, pad 1 | PyTorch | within 1e-4 |
| `ml/op-maxpool2d` | k 2, stride 2 | PyTorch | within 1e-6 |
| `ml/op-cross-entropy` | mean cross-entropy, 8192 rows × 512 classes | PyTorch | within 1e-5 |

## Results

Each benchmark has one chart, drawn for the machine selected at the top of the page. Languages are the bars, best first. Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

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

```bench
id: ml/op-add
metric: throughput
```

```bench
id: ml/op-relu
metric: throughput
```

```bench
id: ml/op-gelu
metric: throughput
```

```bench
id: ml/op-tanh
metric: throughput
```

```bench
id: ml/op-sigmoid
metric: throughput
```

```bench
id: ml/op-softmax
metric: throughput
```

```bench
id: ml/op-layernorm
metric: throughput
```

```bench
id: ml/op-matmul
metric: throughput
```

```bench
id: ml/op-conv2d
metric: throughput
```

```bench
id: ml/op-maxpool2d
metric: throughput
```

```bench
id: ml/op-cross-entropy
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench mlp-train-step               # Cajeta
./bench.sh --bench mlp-train-step --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver. [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `cajeta-ml/bench/ and cajeta-xgboost/bench/`
- Library: `cajeta-ml`
