# Compile time

How long the compiler takes: the tour cold and warm, a synthetic 10k-line program, and the link-time-optimization modes. The competitors compile an equivalent program with their own toolchains.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `toolchain/compile-tour-cold` | `cajeta build` of `samples/tour` with an empty cache | javac on an equivalent tree, rustc, go build, g++ | binary runs |
| `toolchain/compile-tour-warm` | the same with a warm compile cache and no edits | same | binary runs |
| `toolchain/compile-10k` | a generated 10k-line, 200-class program | same | binary runs |
| `toolchain/compile-lto` | the 10k program at `--lto=off`, `thin`, `full`; variants | rustc and clang LTO modes | binary runs |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

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
