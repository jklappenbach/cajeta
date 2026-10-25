# Debugger

Attach time of the debug adapter.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `toolchain/debugger-attach` | attach the DAP server to a running tour process and set one breakpoint, 20 times | jdb attach, gdb attach to a Rust and a C++ binary, delve attach, debugpy attach | breakpoint reported |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: toolchain/debugger-attach
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench debugger-attach               # Cajeta
./bench.sh --bench debugger-attach --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/toolchain/`
