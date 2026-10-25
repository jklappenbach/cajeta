# Tape

`cajeta.nucleo.autograd.Tape` — the define-by-run autograd tape.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `nucleo/tape-forward-backward` | forward and backward through a 3-layer MLP on a 256 × 784 batch, 1e3 steps | PyTorch autograd (CPU), JAX grad | gradient within epsilon of torch |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

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
