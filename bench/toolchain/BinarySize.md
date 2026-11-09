# Binary size

This page measures the size on disk of hello-world and of the tour, stripped.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `toolchain/binary-size-hello` | hello-world executable | C++, Rust, Go, Java jar plus runtime (noted), Python (interpreter noted) | runs |
| `toolchain/binary-size-tour` | the tour executable (debug build, stripped) and its `.cja` (checked with `cajeta archive verify`, since nothing runs a `.cja` directly); variants exe, cja | the synthetic program: C++, Rust, Go stripped, a Java jar and a Python zipapp, runtimes not counted | runs |

## Results

Each benchmark has one chart, drawn for the machine selected at the top of the page. Languages are the bars, best first. Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: toolchain/binary-size-hello
metric: memory
```

```bench
id: toolchain/binary-size-tour
metric: memory
```

## Standing

The hello executable is a Cajeta-only row and the tour executable is 70x the size of cargo's, 30x to 50x the C++ and Go binaries, and 1.3x to 1.6x the Java and Python artifacts: the executable carries the runtime and the stdlib it links. The `.cja` archives have no competitor pair. Size is not time; these rows stay out of the standings mean.

## Reproduce

```sh
cd bench
./bench.sh --bench binary-size-hello               # Cajeta
./bench.sh --bench binary-size-hello --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver. [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/toolchain/`
