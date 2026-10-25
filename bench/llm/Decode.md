# Decode

`dev.cajeta.llm` — decode tokens per second at one and four sequences. Every row carries its operating system, driver, model and platform; the chart groups rows by model and lays cajeta-llm beside llama.cpp for each OS, driver and backend. A decode number is read only beside its greedy-agreement rate and its printed tokens.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `llm/decode-1seq` | 128 tokens after a 512-token prompt, one sequence; variants per model: Llama-3.1-8B at Q2_K, Q3_K_M, Q4_K_M, Q5_K_M, Q6_K, Q8_0; Qwen3-Coder-30B-A3B Q5_K_M | llama.cpp (CUDA on Phoenix; HIP and Vulkan on Proton), commit recorded on the row | greedy agreement ≥ 0.99 with llama.cpp |
| `llm/decode-4seq` | the same at four sequences | llama.cpp (CUDA on Phoenix; HIP and Vulkan on Proton), commit recorded on the row | greedy agreement ≥ 0.99, tokens compared with the one-sequence walk |

Method: [measuring](../../docs/llm/measuring.md). Interleaved rounds in the same minute, the median read.

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: llm/decode-1seq
metric: throughput
```

```bench
id: llm/decode-4seq
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench decode-1seq               # Cajeta
./bench.sh --bench decode-1seq --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `cajeta-llm/bench/`
- Library: `cajeta-llm`, benchmark code in `cajeta-llm/bench/`
