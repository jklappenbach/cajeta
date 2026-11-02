# Configuration

This page lists everything the engine can be told, with its default and what
it changes. The same options reach the library through `EngineOptions`, the
command line through the flags in the right-hand column, and cabra through
its own flags of the same names.

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
| `prefillWeights` | `auto` | `--prefill-weights` | `packed` batches every format from its quantised blocks; `int8` widens each weight once into a resident int8 copy for the widen GEMMs (about twice that weight's bytes, prefill only); `auto` times the tile against the int8 GEMM at load |
| `moeResidencyBudget` | -1 (auto: free device memory at the first bank's bind, less a reserve for the K/V pool and scratch) | `--moe-residency-budget` | bytes of expert weights kept resident; 0 turns residency off and every expert dispatch streams through the windowed path |
| `widenBudget` | -1 (auto: half of available memory) | `--widen-budget` | ceiling on the int8 widen in bytes; 0 never widens |
| `cpuInt8Decode` | true | `--cpu-int8-decode on\|off` | the CPU backend decodes Q4_K and Q6_K on its int8 kernels |
| `cpuThreads` | 0 (one worker a physical core) | `--cpu-threads` | the CPU backend's worker threads, up to every hardware thread |
| `cpuWidth` | `auto` | `--cpu-width` | the CPU backend's interleave width for Q4_K and Q6_K weights; `auto` times 8 and 16 rows at load for decode, `auto-prefill` for prefill, and `8` or `16` pins it |
| `colsPays` | `auto` | `--cols-pays` | whether the batched Cols kernel serves the decode batch; `auto` times it per format and width, `on` or `off` pins it |
| `colsVariant` | `auto` | `--cols-variant` | the Cols kernel's variant for Q4_K and Q6_K; `prefetch`, `plain` or `two-row` pins it |
| `mvPrefetch` | `auto` | `--mv-prefetch` | whether the one-column decode mat-vec prefetches its weights; `on` or `off` pins it |
| `colsSeg` | `auto` | `--cols-seg` | the fused q\|k\|v and gate\|up Cols launches; `auto` decides by an A/B at load, `fused`, `separate` or `gate-up` pins it, and `probe` asks a per-shape probe instead |
| `matrixCores` | `auto` | `--matrix-cores` | whether the matrix-core GEMMs beat the dotAccum batch kernel; `on` or `off` pins it |
| `attnTile` | `auto` | `--attn-tile` | whether the prefill attention tile beats the wave kernels; `on` or `off` pins it |

Each of the last six options pins one load-time choice. Its `auto` leaves the
choice to the engine and keeps any pin a program set in code before the load.
A misspelt value stops the load with a message that names the option.

Sampling is per request. `SampleParams(temperature, seed)` carries `topK`
(0 = off), `topP` (0 = off), `repeatPenalty` (1.0 = off), stop tokens and
stop strings. The command line's `--temp`, `--top-k`, `--top-p`, `--seed`
and `--repeat-penalty` set them for its one request. A temperature of 0 is
greedy and deterministic.

## 2. The tune directory

The engine remembers its measured choices per machine, so that the second
run of a model starts at the settled choice. Every timed choice in §6 is kept
here except the CPU thread count and the prefill chunk. The directory is
resolved in this order:

1. the `tuneDir` option or `--tune-dir`;
2. `CAJETA_LLM_TUNE_DIR` (the value `off` disables persistence);
3. the platform cache directory (`XDG_CACHE_HOME` or `~/.cache` on Linux,
   `LOCALAPPDATA` on Windows) with `cajeta-llm/tune` appended.

Entries are keyed by the device's hardware class and by the build of the
kernels they were measured against. The hardware class is the backend, the
multiprocessor layout, the register and shared-memory sizes, the L2 and the
VRAM, so two identical cards share entries and a store made on one serves the
other. A value from another kind of device or an older build is ignored. The first run of a model on a fresh store pays for the
measurements, which take tens of milliseconds to a few seconds depending on
the shapes, and a throughput figure should be read only after the store has
settled. The test suites run with the store off.

## 3. Environment

| variable | effect |
|---|---|
| `CAJETA_XPU_BACKEND` | the backend to select at the first device touch (`cuda`, `hip`, `vulkan`, `cpu`); the `device` option is the in-process form |
| `CAJETA_LLM_TUNE_DIR` | the tune directory (§2) |
| `CAJETA_XPU_DEFER` | `0` runs every launch direct; `all` defers every stream; unset leaves the engine's choice (the decode stream is deferred and replayed as a graph on CUDA) |
| `CAJETA_XPU_POISON` | `1` fills fresh device buffers with 0xFF so an unwritten read answers NaN; the suites' default |
| `CAJETA_PROFILER` | `1` arms the host sampler and device capture (the profiler keeps every launch direct) |
| `CAJETA_XPU_CPU_WORKERS` | caps the CPU backend's worker threads |
| `CAJETA_XPU_CPU_LEVEL` | lowers the CPU backend's ISA level (`v3`, `v3-vnni`, `v4`, `v4-amx` or `host`) below what the host supports; it never raises it, and below `v4` the CPU interleave width is 8 with no probe |

## 4. Residency and memory

A model is device resident after load. The weights, the K/V pool, the
activation scratch and the prefill tiles' buffers live on the device, and a
decode step performs no allocation. Two budgets bound the rest. The
mixture-of-experts residency budget decides how many expert banks stay on
the device. Its `auto` value is the free device memory at the first bank's
bind less a reserve for the K/V pool and scratch, and on a unified-memory
part the budget shares physical RAM with the page cache the fallback path
streams through. The widen budget decides whether a format's int8 prefill
copy fits at all before the prefill-weight policy asks whether it pays. On a
72B the widen would be 50 GB over 45 GB of packed weights. The budget
answers with a route, and the process is not killed for memory.

## 5. Diagnostics

The engine records routing and scheduling decisions as structured records
(`Diag`) and hands each to a callback the embedder registers. It formats
nothing, stores nothing and chooses no logging backend. With no callback
registered nothing is recorded. The command line's `--verbose` installs a
callback that writes the records to the log at DEBUG. Those records say
which route each projection took, which tile shape was chosen, and when a
batch was refused and why. cabra forwards a session's records to the client
that asked for them. The record names used throughout the documentation
(`batch-route`, `tile-choice`, `decode-batch`, `sched-phase`, `moe-rows`)
are these.

## 6. Pinning for a fixed target

A deployment that runs on one known kind of device can load without timing
anything. A load times a handful of choices the first time it meets a device
and a build, and the table lists each one with what pins it. With every
choice pinned, or answered by a store seeded on the same kind of device, a
load does no measurement and starts as fast as it can.

| choice | what it decides | backends | cost at load | kept in the store | pin it with |
|---|---|---|---|---|---|
| CPU interleave width | 8 or 16 rows for the Q4_K and Q6_K int8 weights | CPU at level `v4` and above | about 5.5 s on the first load of a build | yes | `cpuWidth` |
| CPU threads | the CPU backend's worker count | CPU | none today | no | `cpuThreads` |
| Cols pays | whether the batched Cols kernel serves a decode batch, per format and width | GPUs | not measured separately | yes | `colsPays` |
| Cols variant | prefetch, plain or two-row Cols kernel for Q4_K and Q6_K | GPUs | 100 to 150 ms | yes | `colsVariant` |
| One-column prefetch | whether the decode mat-vec prefetches its weights | GPUs | not measured separately | yes | `mvPrefetch` |
| Fused Cols launches | q\|k\|v and gate\|up fused or separate, decided by an A/B of whole decode steps | GPUs, at more than one sequence | about 0.8 s | yes | `colsSeg` |
| Matrix cores | whether the tensor-core GEMMs beat the dotAccum batch kernel | CUDA, HIP, and the CPU at `v4-amx` | not measured separately | yes | `matrixCores` |
| Tile or int8 | the prefill tile against the int8 widen GEMM | CUDA, HIP | a few ms | yes | `prefillWeights` |
| Prefill attention tile | the flash prefill tile against the wave kernels, per head shape | GPUs | not measured separately | yes | `attnTile` |
| Tile shapes | the tile family member for each problem size | CUDA, HIP | about 120 ms a problem on an RTX 4090 | yes | a seeded store |
| Expert tile shapes | the tile family member for a mixture-of-experts layer, timed during the first real prefills | CUDA, HIP | a few prefill layers | yes | a seeded store |
| Prefill chunk | the chunk the scheduler sizes against its step budget, adapted while serving | all | none at load | no | `chunk` |

Tile shapes have no option because there are dozens of them, one for each
problem the model meets. A store carries them instead. Seed one by loading
the model once on a device of the kind you deploy to, with the store pointed
at a directory you ship. Then point every deployed engine's `tuneDir` at a
copy of that directory. The store is keyed by hardware class, driver version
and kernel build, so it only answers on matching hardware and a matching
build, and anything else is timed afresh.

```sh
# once, on a card of the deployed kind
cajeta-llm run --model model.gguf --tune-dir ./tune --prompt "warm up" -n 16
# on every deployed card, with ./tune shipped beside the model
cajeta-llm run --model model.gguf --tune-dir ./tune \
    --cols-pays on --cols-variant plain --mv-prefetch off --cols-seg separate \
    --matrix-cores on --attn-tile on --prefill-weights packed --verbose
```

Check a pinned load with `--verbose`. A load that timed nothing emits no
`cols-probe`, `mv-probe`, `colsseg-probe`, `mc-probe`, `tile-probe`,
`attn-tile-probe` or `cpu-width` record, its `colsseg-ab` record reads
`pinned`, and every `tile-choice` record reports a stored answer. That held
for the commands above with Llama-3.1-8B Q4_K_M on an RTX 4090. The first
load through new pins can still take seconds longer while the driver compiles
kernels it has not run before, and later loads do not pay that again.

Read the pinned values off a seeded load rather than guessing them. Its
`--verbose` records name each answer the engine chose, and the options above
reproduce them.

