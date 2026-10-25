# Prefill

`dev.cajeta.llm` — prompt-processing tokens per second at 512 and 4096 tokens, keyed like Decode.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `llm/prefill-512` | pp512, one and four sequences; variants per model as Decode | llama.cpp (CUDA on Phoenix; HIP and Vulkan on Proton), commit recorded on the row | greedy agreement on the following token |
| `llm/prefill-4096` | pp4096, one sequence; variants per model | llama.cpp (CUDA on Phoenix; HIP and Vulkan on Proton), commit recorded on the row | same |

The 30B's single-sequence prefill swings 1.75× between runs a minute apart on Phoenix (the GPU clock); it is read at four sequences.

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: llm/prefill-512
metric: throughput
```

```bench
id: llm/prefill-4096
metric: throughput
```

## Reproduce

```sh
cd bench
./bench.sh --bench prefill-512               # Cajeta
./bench.sh --bench prefill-512 --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `cajeta-llm/bench/`
- Library: `cajeta-llm`
