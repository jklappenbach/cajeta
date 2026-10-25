# Tape

`cajeta.nucleo.autograd.Tape` — the define-by-run autograd tape.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `nucleo/tape-forward-backward` | forward and backward through a 16-16-16-1 sigmoid MLP on a batch of 16, MSE, SGD, 1e3 steps (the Tape is scalar-only, one float32 per node, so the 256 × 784 shape would be 1e8 nodes a step); skips on v0.46.3 because `Tape.grow()` dangles its grown arrays past 32 nodes, the filed primitive-array store defect | PyTorch autograd (CPU), JAX grad | last loss and sum|dW1| against a float64 hand-written backprop |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: nucleo/tape-forward-backward
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench tape-forward-backward               # Cajeta
./bench.sh --bench tape-forward-backward --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Tape](../../docs/stdlib/nucleo/Tape.md)
- Source: `runtime/src/cajeta/nucleo/autograd/`
- Benchmark source: `bench/src/main/cajeta/bench/nucleo/`
