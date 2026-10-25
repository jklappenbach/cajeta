# Compile time

How long the compiler takes: the tour cold and warm, a synthetic 10k-line program generated in six languages from one description, and the link-time-optimization modes. The tour has no counterpart in other languages, so its competitor rows compile the synthetic program, the closest in size (9.8k lines against 10k). The tour builds with the debug flavor because no optimized flavor links it on v0.46.3 (the filed drop-symbol link defect).

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `toolchain/compile-tour-cold` | `cajeta build` of a fresh copy of `samples/tour` with an empty cache | javac, cargo/rustc, go build, g++ through ninja, on the synthetic program | binary runs |
| `toolchain/compile-tour-warm` | the same with a warm artifact cache and no edits (javac keeps no cache and recompiles) | same | binary runs |
| `toolchain/compile-10k` | the generated 10k-line, 200-class, 202-file program | same | binary runs |
| `toolchain/compile-lto` | the 10k program at `--lto=off`, `thin`, `full`; variants; the fork's lld is put on PATH, since the compiler looks for it nowhere else | cargo `lto` off/thin/fat and the fork clang++ `-flto=thin|full` with lld | binary runs |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: toolchain/compile-tour-cold
metric: time
```

```bench
id: toolchain/compile-tour-warm
metric: time
```

```bench
id: toolchain/compile-10k
metric: time
```

```bench
id: toolchain/compile-lto
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench compile-tour-cold               # Cajeta
./bench.sh --bench compile-tour-cold --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/toolchain/`
