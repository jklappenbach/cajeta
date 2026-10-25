# Command

`cajeta.process.Command` — spawning a child.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `process/command-spawn-wait` | spawn and wait `/bin/true` 1e3 times | C++ `posix_spawn`, Rust `Command`, Go `exec.Command`, Java `ProcessBuilder`, Python `subprocess` | exit codes |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: process/command-spawn-wait
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench command-spawn-wait               # Cajeta
./bench.sh --bench command-spawn-wait --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Reference: [Command](../../docs/stdlib/process/Command.md)
- Source: `runtime/src/cajeta/process/Command.cajeta`
- Benchmark source: `bench/src/main/cajeta/bench/process/`
