# Memory

This page measures the resident memory of `dev.cajeta.llm` at a 4096-token context.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `llm/rss-4096` | peak resident set during a 4096-context generation; variants per model | llama.cpp (CUDA on Phoenix; HIP and Vulkan on Proton), commit recorded on the row | generation completes |

## Results

Each benchmark has one chart, drawn for the machine selected at the top of the page. Languages are the bars, best first. Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: llm/rss-4096
metric: memory
```

## Standing

Not measured yet: the first published run (proton, 2026-11-09) holds no rows for this library. Its bench runs under its own repository and is collected by `bench.sh publish` once it has run on a quiet box.

## Reproduce

```sh
cd bench
./bench.sh --bench rss-4096               # Cajeta
./bench.sh --bench rss-4096 --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver. [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `cajeta-llm/bench/`
- Library: `cajeta-llm`
