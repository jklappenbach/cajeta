# Models and weight formats

What `dev.cajeta.llm` can load, and in which form.

## 1. Files

| file | how it is read |
|---|---|
| a `.gguf` file | the GGUF metadata populates the model configuration; the tokenizer and the chat template come from the file |
| a `.safetensors` file, or a sharded `.safetensors.index.json` | with a Hugging Face `config.json` beside it (or named by the `config` option) and the tokenizer files of the repository |

Checkpoint files are memory-mapped; a model is read once at load and the
engine holds no second copy of the weights on the host.

## 2. Architectures

From GGUF, by the `general.architecture` key:

| architecture | notes |
|---|---|
| `llama` | Llama 2 and 3.x, Mistral, Mixtral and the other Llama-shaped checkpoints; a mixture of experts is recognised by `expert_count`, not by the name |
| `qwen2`, `qwen2vl` | the text path of Qwen2 and Qwen2-VL |
| `qwen2moe` | Qwen2 mixture of experts, with the shared expert |
| `qwen3` | Qwen3 dense, with QK-norm |
| `qwen3moe` | Qwen3 mixture of experts (Qwen3-Coder-30B-A3B is the measured reference) |
| `bitnet` | ternary BitNet checkpoints; a converter from the Hugging Face `BitnetForCausalLM` layout ships in the library |

From Hugging Face `config.json`: `LlamaForCausalLM` and `BitnetForCausalLM`.
A safetensors load carries no tokenizer, so it serves token-id submission and
the converter (`tools/convert/hf-to-gguf.sh`) rather than text prompts.
Grouped-query attention, rotary embeddings (the Llama-3.1 frequency table from
`rope_freqs.weight`; linear, dynamic, YaRN and llama3 scaling from a Hugging
Face configuration), sliding and per-layer windows,
tied word embeddings, QK-norm, and the mixture-of-experts fields (expert
count, active experts, the gated shared expert) are read from the
configuration; nothing is assumed from the model's name.

Gemma 3 is specified and deferred; it is not in this release.

## 3. Weight formats

Every format below has a host implementation, and a model whose format no
device kernel serves still runs on the host. The device columns say which
regimes have a dedicated kernel on the CUDA backend today; the catalog
([every cajeta-llm kernel](../catalog-llm.md)) has the row-by-row
record for every backend, and [Language models on the device](../kernels-llm.md)
describes the routes.

| format | block | decode (one row) | batched decode (several sequences) | prefill |
|---|---|---|---|---|
| f16 | | f32-activation wave mat-vec | | f16 cooperative GEMM |
| Q4_0, Q5_0 | 32 | wave mat-vec | | int8 widen GEMM |
| Q4_1, Q5_1 | 32 | scalar mat-vec | | f16 cooperative GEMM |
| Q8_0 | 32 | wave mat-vec | Cols kernel | int8 widen GEMM |
| Q2_K | 256 | wave mat-vec | Cols kernel | int8 widen GEMM |
| Q3_K | 256 | wave mat-vec | Cols kernel | int8 widen GEMM |
| Q4_K | 256 | wave mat-vec, prefetching twin | Cols kernel, fused q\|k\|v and gate\|up | tensor-core tile (`QkTile`); expert banks on the Id tile |
| Q5_K | 256 | wave mat-vec | Cols kernel | tensor-core tile (`Q5Tile`); expert banks on the Q5 Id tile |
| Q6_K | 256 | wave mat-vec, prefetching twin | Cols kernel | tensor-core tile (`Q6Tile`); expert banks on the multi-wave id GEMM |
| IQ2_XXS, IQ2_XS, IQ2_S, IQ3_XXS, IQ3_S | 256 | wave mat-vec (codebook) | | f16 cooperative GEMM |
| IQ4_NL, IQ4_XS | 32 / 256 | wave mat-vec | | f16 cooperative GEMM; IQ4_NL expert banks on the cooperative id GEMM |
| IQ1_S, IQ1_M | 256 | host decode | | |
| TQ1_0, TQ2_0 (ternary) | 256 | wave mat-vec | | f16 cooperative GEMM |
| MXFP4 | 32 | scalar mat-vec (a faster codebook variant exists and is not yet routed) | | |

A checkpoint mixes formats (a Q5_K_M file holds Q5_K and Q6_K tensors and an
f16 or Q8_0 output head), and the route is chosen per tensor. Activations are
quantised on the device to q8_K for the integer routes.

## 4. Context and cache

The context length is an option (`ctx`, default 4096); the K/V cache is paged
in blocks of `blockTokens` tokens (default 16) and shared across the
sequences the scheduler runs. A file-backed prefix store (`kvStore`) keeps
evicted blocks keyed by prefix hash, so a later request with the same prefix,
or the same process after a restart, reuses them instead of recomputing.
