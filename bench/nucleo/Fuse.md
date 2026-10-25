# Fuse

`cajeta.nucleo.expr.Fuse` — compile-time fusion of elementwise tensor expressions: the fused row against the same expression written as separate operations.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `nucleo/fuse-elementwise-chain` | `(a * b + c) / d - e` over 1e7 float32, fused and unfused | NumPy (unfused), numexpr, PyTorch compiled, JAX jit | within epsilon |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: nucleo/fuse-elementwise-chain
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench fuse-elementwise-chain               # Cajeta
./bench.sh --bench fuse-elementwise-chain --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Fuse](../../docs/stdlib/nucleo/Fuse.md)
- Source: `runtime/src/cajeta/nucleo/expr/`
- Benchmark source: `bench/src/main/cajeta/bench/nucleo/`
