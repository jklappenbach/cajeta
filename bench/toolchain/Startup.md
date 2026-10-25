# Start-up

Time from `exec` to the first line of output for hello-world, as a native executable and through the JIT, cold and warm. No command runs a `.cja` archive through the JIT on v0.46.3 (`jit-run` takes a source root), so the JIT rows use `cajeta jit-run` on the hello source with an empty and a filled cache directory.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `toolchain/startup-exe` | hello-world executable, 1e2 launches | C++, Rust, Go binaries, Java `java -jar`, Python | output |
| `toolchain/startup-cja-cold` | `cajeta jit-run` with an empty cache directory | Java with `-Xshare:off`, CPython with an emptied bytecode cache | output |
| `toolchain/startup-cja-warm` | `cajeta jit-run` with the machine code cached | Java with an AppCDS archive, CPython with its bytecode cache filled | output |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: toolchain/startup-exe
metric: time
```

```bench
id: toolchain/startup-cja-cold
metric: time
```

```bench
id: toolchain/startup-cja-warm
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench startup-exe               # Cajeta
./bench.sh --bench startup-exe --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/toolchain/`
