# Compile time

This page measures how long the compiler takes on the tour cold and warm, on a synthetic 10k-line program generated in six languages from one description, and under the link-time-optimization modes. The tour has no counterpart in other languages, so its competitor rows compile the synthetic program, which is the closest in size at 9.8k lines against 10k. The tour builds with the debug flavor because no optimized flavor links it on v0.46.3, the filed drop-symbol link defect.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `toolchain/compile-tour-cold` | `cajeta build` of a fresh copy of `samples/tour` with an empty cache | javac, cargo/rustc, go build, g++ through ninja, on the synthetic program | binary runs |
| `toolchain/compile-tour-warm` | the same with a warm artifact cache and no edits (javac keeps no cache and recompiles) | same | binary runs |
| `toolchain/compile-10k` | the generated 10k-line, 200-class, 202-file program | same | binary runs |
| `toolchain/compile-lto` | the 10k program at `--lto=off`, `thin`, `full`; variants; the fork's lld is put on PATH, since the compiler looks for it nowhere else | cargo `lto` off/thin/fat and the fork clang++ `-flto=thin|full` with lld | binary runs |

## Results

Each benchmark has one chart, drawn for the machine selected at the top of the page. Languages are the bars, best first. Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

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

## Standing

A cold build of the tour sample, from a copy with no build directory and no cache, takes 69 s, 160x cargo's and 20x to 100x javac, go and g++. Warm, the tour compiles in 46 ms, 39x behind g++ and 13x ahead of javac, 1.2x of go and 5x of cargo. Ten thousand lines take 34 s, 77x behind cargo; the LTO variants 20 s to 25 s, 11x to 25x behind cargo and 7x to 10x behind clang++. Compile time is the toolchain's own open problem, with the stdlib build on the cold path first.

## Reproduce

```sh
cd bench
./bench.sh --bench compile-tour-cold               # Cajeta
./bench.sh --bench compile-tour-cold --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver. [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/toolchain/`
