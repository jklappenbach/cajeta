# Fuse

`cajeta.nucleo.expr.Fuse` is compile-time fusion of elementwise tensor expressions. The row reads the fused expression against the same expression written as separate operations.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `nucleo/fuse-elementwise-chain` | `(a * b + c) / d - e` over 1e7 float32 with b, c, d, e scalars (Fuse v1 takes one tensor parameter), fused and unfused; the competitors run the same expression | NumPy (unfused), numexpr, `torch.compile`, JAX jit | within epsilon |

## Results

Each benchmark has one chart, drawn for the machine selected at the top of the page. Languages are the bars, best first. Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: nucleo/fuse-elementwise-chain
metric: throughput
```

## Standing

The fused chain is 34x behind numexpr and 10x to 17x behind torch and jax; unfused 88x behind numpy. Fusion does remove the intermediates (the fused row is 6.6x faster than the unfused one), but the fused kernel still runs on the cpu tensor paths. Same dependency as [Column](Column.md).

## Reproduce

```sh
cd bench
./bench.sh --bench fuse-elementwise-chain               # Cajeta
./bench.sh --bench fuse-elementwise-chain --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver. [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Fuse](../../docs/stdlib/nucleo/Fuse.md)
- Source: `runtime/src/cajeta/nucleo/expr/`
- Benchmark source: `bench/src/main/cajeta/bench/nucleo/`
