# Configuration

Everything the engine can be told, with its default and what it changes. The
same options reach the library through `EngineOptions`, the command line
through the flags in the right-hand column, and cabra through its own flags
of the same names.

## 1. Engine options

| `EngineOptions` field | default | flag | meaning |
|---|---|---|---|
| `model` | required | `--model` | the `.gguf`, `.safetensors` or sharded index file |
| `config` | required for safetensors | `--config` | the Hugging Face `config.json` for safetensors models; ignored for GGUF |
| `ctx` | 4096 | `--ctx` | context length in tokens |
| `maxSeqs` | 4 | `--max-seqs` | sequences the scheduler runs at once (its slot count) |
| `blockTokens` | 16 | `--block-size` | tokens per K/V block |
| `chunk` | 0 (adaptive) | `--chunk` | prefill chunk in tokens; 0 lets the chunk policy pick, N pins it |
| `chunkBudgetMs` | 150 | `--chunk-budget-ms` | while other sequences decode, a prefill step is sized to stay under this |
| `device` | empty (dispatcher order) | `--device` | force a backend: `cuda`, `hip`, `vulkan` or `cpu` |
| `tuneDir` | per-user cache | `--tune-dir` | where this machine's measured choices are kept; `off` keeps none |
| `kvStore` | empty (disabled) | `--kv-store`, `--no-kv-store` | the prefix block store's path |
| `kvStoreSize` | 256 MiB | `--kv-store-size` | the store's size bound |
| `prefillWeights` | `auto` | | `packed` batches every format from its quantised blocks; `int8` widens each weight once into a resident int8 copy for the widen GEMMs (about twice that weight's bytes, prefill only); `auto` measures per format at load |
| `moeResidencyBudget` | -1 (auto: free device memory at the first bank's bind, less a reserve for the K/V pool and scratch) | | bytes of expert weights kept resident; 0 turns residency off and every expert dispatch streams through the windowed path |
| `widenBudget` | -1 (auto: half of available memory) | | ceiling on the int8 widen in bytes; 0 never widens |

Sampling is per request, not per engine: `SampleParams(temperature, seed)`
with `topK` (0 = off), `topP` (0 = off), `repeatPenalty` (1.0 = off), stop
tokens and stop strings; the command line's `--temp`, `--top-k`, `--top-p`,
`--seed` and `--repeat-penalty` set them for its one request. A temperature
of 0 is greedy and deterministic.

## 2. The tune directory

Measured choices (which tile shape at which problem, whether a prefetching
mat-vec pays, whether the batched Cols kernel pays at a width, whether fused
projections pay) are remembered per machine so that the second run of a model
starts at the settled choice. The directory is resolved in this order:

1. the `tuneDir` option or `--tune-dir`;
2. `CAJETA_LLM_TUNE_DIR` (the value `off` disables persistence);
3. the platform cache directory (`XDG_CACHE_HOME` or `~/.cache` on Linux,
   `LOCALAPPDATA` on Windows) with `cajeta-llm/tune` appended.

Entries are keyed by the device's backend and geometry and by the build of the
kernels they were measured against; a value from another machine or an older
build is ignored. The first run of a model on a fresh store pays for the
measurements (tens of milliseconds to a few seconds depending on the shapes),
and a throughput figure should be read only after the store has settled. The
test suites run with the store off.

## 3. Environment

| variable | effect |
|---|---|
| `CAJETA_XPU_BACKEND` | the backend to select at the first device touch (`cuda`, `hip`, `vulkan`, `cpu`); the `device` option is the in-process form |
| `CAJETA_LLM_TUNE_DIR` | the tune directory (§2) |
| `CAJETA_XPU_DEFER` | `0` runs every launch direct; `all` defers every stream; unset leaves the engine's choice (the decode stream is deferred and replayed as a graph on CUDA) |
| `CAJETA_XPU_POISON` | `1` fills fresh device buffers with 0xFF so an unwritten read answers NaN; the suites' default |
| `CAJETA_PROFILER` | `1` arms the host sampler and device capture (the profiler keeps every launch direct) |
| `CAJETA_XPU_CPU_WORKERS` | caps the CPU backend's worker threads |

## 4. Residency and memory

A model is device resident after load: the weights, the K/V pool, the
activation scratch and the prefill tiles' buffers live on the device, and a
decode step performs no allocation. Two budgets bound the rest. The
mixture-of-experts residency budget decides how many expert banks stay on the
device (`auto` is the free device memory at the first bank's bind less a
reserve for the K/V pool and scratch; on a unified-memory part the budget
shares physical RAM with the page cache the fallback path streams through). The widen budget decides whether a format's int8
prefill copy fits at all before the prefill-weight policy asks whether it
pays; on a 72B the widen would be 50 GB over 45 GB of packed weights, and the
answer is a route rather than an out-of-memory kill.

## 5. Diagnostics

The engine records routing and scheduling decisions as structured records
(`Diag`) and hands each to a callback the embedder registers; it formats
nothing, stores nothing and chooses no logging backend. With no callback
registered nothing is recorded. The command line's `--verbose` installs a
callback that writes the records to the log at DEBUG (which route each
projection took, which tile shape was chosen, when a batch was refused and
why); cabra forwards a session's records to the client that asked for them.
The record names used throughout the documentation (`batch-route`,
`tile-choice`, `decode-batch`, `sched-phase`, `moe-rows`) are these.
