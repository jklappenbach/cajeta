# Npy

`cajeta.math.npio.Npy` — NumPy `.npy` interchange. Decoding 2^23 int32 was measured at 125 to 147 ms before the tensor-element-access work; its target is 10 ms.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `math/npy-load-i32` | decode a 2^23 int32 `.npy` image held in memory (no disk read in the measured window) | NumPy `load` from a `BytesIO`, Rust `npyz`, Java `npy` | checksum |
| `math/npy-save-i32` | encode it back to a `.npy` image in memory | same | byte equality |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: math/npy-load-i32
metric: throughput
```

```bench
id: math/npy-save-i32
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench npy-load-i32               # Cajeta
./bench.sh --bench npy-load-i32 --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Npy](../../../docs/stdlib/math/npio/Npy.md)
- Source: `runtime/src/cajeta/math/npio/Npy.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/math/`
