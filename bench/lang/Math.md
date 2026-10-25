# Math and SIMD

`cajeta.lang.Math` — numeric utilities on the host, plus the `Vector<T,N>` SIMD type the two oldest rows use. The transcendental loop is the libm comparison; `saxpy` and `dot-product` are the portable-SIMD comparison against Highway and Rust portable-simd.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `lang/math-transcendental` | 1e7 `sin`, `exp` and `sqrt` over a float64 array | C++ libm, Rust, Go, Java, NumPy | sum within epsilon |
| `lang/math-intdiv` | 1e8 int64 divisions and remainders | C++, Rust, Go, Java, Python | sum |
| `lang/math-intrinsics` | 1e8 `abs`, `min`, `max` and `a*0.5+1.0` in place of `fma` (`Math.fma` is kernel-only on v0.46.3 though documented as native everywhere) | C++, Rust, Go, Java, Python | sum |
| `lang/saxpy` | `y = a*x + y` over 1e7 float32 with `Vector<float32,8>` | C++ (auto-vectorized and Highway), Rust portable-simd, Go, Java Vector API, NumPy | checksum |
| `lang/dot-product` | dot of two 1e7 float32 arrays with `Vector<float32,8>` | same | within epsilon |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: lang/math-transcendental
metric: time
```

```bench
id: lang/math-intdiv
metric: time
```

```bench
id: lang/math-intrinsics
metric: time
```

```bench
id: lang/saxpy
metric: throughput
```

```bench
id: lang/dot-product
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench math-transcendental               # Cajeta
./bench.sh --bench math-transcendental --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Math](../../docs/stdlib/lang/Math.md)
- Source: `runtime/src/cajeta/lang/Math.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/lang/`
