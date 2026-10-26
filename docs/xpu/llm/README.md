# Language models

`dev.cajeta.llm` is a decoder-only language-model inference engine written
in Cajeta: it loads open weights from GGUF or Hugging Face safetensors, runs
KV-cached incremental decode with continuous batching on any XPU backend
(NVIDIA, AMD, Vulkan or the host CPU), samples, and detokenises. llama.cpp
supplies the architecture reference and the parity target; none of its code
is used. The engine is a library with a function-call API and no wire
protocol; a command line ships with it, and **cabra** is the serving harness
that keeps a model loaded and serves conversations over stdio or WebSocket.

| page | what it answers |
|---|---|
| [Quickstart](quickstart.md) | load a GGUF, generate, stream, in the shortest complete program |
| [The engine API](engine.md) | `LlmEngine`, `EngineOptions`, requests, sampling, sinks, diagnostics |
| [Models and weight formats](models.md) | which files, architectures and quantisations load, and what runs where |
| [Inside the engine](inside.md) | the forward path, the continuous-batching scheduler, the K/V cache and store, the pipelined decode, residency |
| [Configuration](configuration.md) | every option, the tune directory, the environment, memory budgets, diagnostics |
| [Running and serving](serving.md) | the command line, embedding, cabra's three modes and protocol, the web client, getting the pieces |
| [Measuring](measuring.md) | the gates a number must pass, the instruments, and where it stands against llama.cpp |

The kernels the engine runs on, how they are chosen for a format and a shape,
and the technology that adapts them to the device are documented under
[Kernels](../README.md), with the complete
[catalog of every cajeta-llm kernel](../catalog-llm.md).

## Status (2026-10-24)

The library is `dev.cajeta.llm` 0.1.5 on Olla; cabra is `dev.cajeta.cabra`
0.1.4. What works:

- **Models.** Llama 2 and 3.x, Mistral and Mixtral (GGUF architecture
  `llama`), Qwen2 and Qwen2-VL text, Qwen2-MoE, Qwen3 and Qwen3-MoE, BitNet.
  Hugging Face safetensors for `LlamaForCausalLM` and `BitnetForCausalLM`.
  Gemma 3 is specified and deferred.
- **Formats.** f16 and f32; Q4_0, Q4_1, Q5_0, Q5_1, Q8_0; Q2_K through Q6_K;
  the IQ2, IQ3 and IQ4 codebook formats; the ternary TQ1_0 and TQ2_0; MXFP4.
  Every format runs on the host; the device routes are in
  [Models](models.md) and the [kernel catalog](../catalog-llm.md).
- **Engine.** Continuous batching with chunked prefill, admission against the
  K/V pool, youngest-victim preemption and FIFO resume; a paged K/V cache with
  a file-backed prefix store; device residency with a zero-allocation decode
  step; a pipelined greedy decode and a batched decode across sequences; the
  model's own chat template; per-request sampling; structured diagnostics
  through a callback.
- **Backends.** CUDA, HIP, Vulkan and CPU from one source. The CUDA backend on
  an RTX 4090 is the measured reference: single-sequence decode at or above
  llama.cpp on every Llama-3.1-8B quantisation, four-sequence decode at 0.89
  to 0.97, Q4_K and Q6_K prefill at 1.1 to 1.2, with the remaining prefill
  routes and the mixture-of-experts prefill behind ([Measuring](measuring.md)).
  The Vulkan decode on gfx1151 sits at 0.98 of llama.cpp's Vulkan at four
  sequences after the batch-cadence work of the same day.

Open items are tracked in the project plan: the Vulkan and CPU parity gates,
long-context attention, resident memory against llama.cpp on the
unified-memory parts, and the XDNA NPU as a backend.
