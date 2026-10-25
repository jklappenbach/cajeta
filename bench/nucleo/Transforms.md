# Transforms

`cajeta.nucleo.transform` — the compile-time function transforms `Grad`, `Vmap` and `Jit`.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `nucleo/transforms-grad` | gradient of a scalar function of a 1e6 vector, 1e3 times | JAX `grad`, PyTorch `func.grad` | within epsilon |
| `nucleo/transforms-vmap` | vectorize a per-element scalar function over 1e4 × 1e3 values (Vmap has no batching rule for `Tensor.sum`, so a per-row body cannot be batched yet) | JAX `vmap`, PyTorch `vmap` | within epsilon |
| `nucleo/transforms-jit` | a 10-op scalar expression under `Jit(Vmap(f))` over 1e6 float32, warm (`Jit` over a tensor lambda crashes at the call on v0.46.3, filed) | JAX `jit`, `torch.compile` | within epsilon |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: nucleo/transforms-grad
metric: time
```

```bench
id: nucleo/transforms-vmap
metric: time
```

```bench
id: nucleo/transforms-jit
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench transforms-grad               # Cajeta
./bench.sh --bench transforms-grad --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Transforms](../../docs/stdlib/nucleo/Transforms.md)
- Source: `runtime/src/cajeta/nucleo/transform/`
- Benchmark source: `bench/src/main/cajeta/bench/nucleo/`
