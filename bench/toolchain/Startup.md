# Start-up

Time from `exec` to the first line of output for hello-world, as a native executable and as a `.cja` archive through the JIT, cold and warm.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `toolchain/startup-exe` | hello-world executable, 1e2 launches | C++, Rust, Go binaries, Java `java -jar`, Python | output |
| `toolchain/startup-cja-cold` | `.cja` with an empty JIT cache | Java cold, Python cold | output |
| `toolchain/startup-cja-warm` | `.cja` with the machine code cached | Java with CDS, Python with `.pyc` | output |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars; a skipped competitor is an empty bar with its reason. A benchmark with no row on the selected machine says so and shows nothing else.

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
