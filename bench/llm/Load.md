# Load and first token

`dev.cajeta.llm` — time to load a model from safetensors and from GGUF, and time to the first generated token.

## Benchmarks

| id | workload | against | check |
|---|---|---|---|
| `llm/load-gguf` | load Llama-3.1-8B Q4_K_M from GGUF to ready; variants per quantization | llama.cpp (CUDA on Phoenix; HIP and Vulkan on Proton), commit recorded on the row | first token matches |
| `llm/load-safetensors` | load Llama-3.1-8B from safetensors | llama.cpp cannot; the row is cajeta-llm alone | first token matches |
| `llm/ttft` | time to first token after a 512-token prompt, warm | llama.cpp (CUDA on Phoenix; HIP and Vulkan on Proton), commit recorded on the row | token matches |

## Results

One chart per benchmark, for the machine selected at the top of the page. Languages are the bars, best first; Cajeta's bar is in color and the competitors' are gray. A skipped competitor keeps its row with the reason in place of a bar. A benchmark with no row on the selected machine says so and shows nothing else.

```bench
id: llm/load-gguf
metric: time
```

```bench
id: llm/load-safetensors
metric: time
```

```bench
id: llm/ttft
metric: time
```

## Reproduce

```sh
cd bench
./bench.sh --bench load-gguf               # Cajeta
./bench.sh --bench load-gguf --competitors # every pinned competitor
```

Every row records the compiler and competitor versions, flags, machine, OS and driver; [METHODOLOGY](../METHODOLOGY.md) states the rules a number must satisfy before it is published.

## See also

- Benchmark source: `bench/src/main/cajeta/bench/llm/`
- Library: `cajeta-llm`
