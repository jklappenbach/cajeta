# Debugger

Time to a first breakpoint under the debug adapter. `cajeta dap` has no `attach` request on v0.46.3 (the debugging guide lists one; only launch is handled, filed), so the Cajeta row launches the tour against a warm JIT cache and stops at one breakpoint, while the competitors attach to a running process.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `toolchain/debugger-attach` | DAP launch of the tour to the first reported breakpoint, 20 times; competitors attach to a running process and stop at one breakpoint | jdb attach, gdb attach to a Rust and a C++ binary, delve attach, debugpy attach | breakpoint reported |

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
