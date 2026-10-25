# Notebook kernel start-up

Time for the Jupyter kernel to answer its first `kernel_info` request and its first execution. The measurement script and its findings predate this catalog and are kept beside it.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `toolchain/kernel-startup` | kernel launch to first reply and first cell, 20 launches | IPython kernel, IJava, the Rust `evcxr` kernel | reply received |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: toolchain/kernel-startup
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench kernel-startup               # Cajeta
./bench.sh --bench kernel-startup --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/toolchain/`
- The original measurement: [kernel-startup findings](kernel-startup/FINDINGS.md)
