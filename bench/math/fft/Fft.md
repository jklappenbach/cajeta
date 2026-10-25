# Fft

`cajeta.math.fft.Fft` — the fast Fourier transform.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `math/fft-1d` | complex64 1-D FFT (`Fft` is float32 interleaved); variants 2^16 and 2^20; NumPy and gonum rows are complex128 and say so | FFTW (float), pocketfft, Rust `rustfft`, `gonum/fourier`, JTransforms | against a direct DFT at small N and round-trip at full N |
| `math/fft-2d` | 2-D complex64 FFT of 2048 × 2048 | same | round-trip within epsilon |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: math/fft-1d
metric: time
```

```bench
id: math/fft-2d
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench fft-1d               # Cajeta
./bench.sh --bench fft-1d --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Fft](../../../docs/stdlib/math/fft/Fft.md)
- Source: `runtime/src/cajeta/math/fft/Fft.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/math/`
