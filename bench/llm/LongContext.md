# Long context

`dev.cajeta.llm` — decode at 8k and 32k tokens of context, where attention dominates.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `llm/decode-8k` | 128 tokens at 8192 context, one and four sequences | llama.cpp (CUDA on Phoenix; HIP and Vulkan on Proton), commit recorded on the row | greedy agreement |
| `llm/decode-32k` | 128 tokens at 32768 context, one sequence | llama.cpp (CUDA on Phoenix; HIP and Vulkan on Proton), commit recorded on the row | greedy agreement |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: llm/decode-8k
metric: throughput
```

```bench
id: llm/decode-32k
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench decode-8k               # Cajeta
./bench.sh --bench decode-8k --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `cajeta-llm/bench/`
- Library: `cajeta-llm`
