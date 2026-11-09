# Debugger

This page measures the time to a first breakpoint under the debug adapter. `cajeta dap` has no `attach` request on v0.46.3, although the debugging guide lists one, and the gap is filed. Only launch is handled, so the Cajeta row launches the tour against a warm JIT cache and stops at one breakpoint, while the competitors attach to a running process.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `toolchain/debugger-attach` | DAP launch of the tour to the first reported breakpoint, 20 times; competitors attach to a running process and stop at one breakpoint | jdb attach, gdb attach to a Rust and a C++ binary, delve attach, debugpy attach | breakpoint reported |

## Results

Each benchmark has one chart, drawn for the machine selected at the top of the page. Languages are the bars, best first. Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: toolchain/debugger-attach
metric: time
```

## Standing

Reaching the first breakpoint takes 21.5 s, 229x jdb and 100x gdb and debugpy: the debug adapter has no attach, so the row launches the tour under the JIT to a breakpoint, as its note says. The launch path is the [Startup](Startup.md) cold path. Not yet scheduled.

## Reproduce

```sh
cd bench
./bench.sh --bench debugger-attach               # Cajeta
./bench.sh --bench debugger-attach --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver. [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/toolchain/`
