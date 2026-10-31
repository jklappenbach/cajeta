# dev.cajeta.llm against llama.cpp

This section publishes prefill and decode tokens per second, keyed by operating system, driver, model and platform. The matrix below is the page to read, and the other pages are its cuts. The method is [measuring](../../docs/xpu/llm/measuring.md).

| Page | |
|---|---|
| [Decode](Decode.md) | Decode tokens per second at one and four sequences |
| [Prefill](Prefill.md) | Prompt-processing tokens per second at 512 and 4096 tokens, keyed like Decode |
| [Load and first token](Load.md) | Time to load a model from safetensors and from GGUF, and time to the first generated token |
| [Memory](Memory.md) | Resident memory at a 4096-token context |
| [Long context](LongContext.md) | Decode at 8k and 32k tokens of context, where attention dominates |
| [Kernel parity](Kernels.md) | The parity legs: one matrix-multiply shape per row, device-timed on each side's own stream, joined under the five rules the parity harness enforces |

## The matrix

There is one block per model. Rows are operating system × driver × backend. The two column groups are prefill and decode tokens per second, and each cell holds cajeta-llm beside llama.cpp with the ratio printed. A row is published only when both platforms ran in the same interleaved session on the same model file and the greedy-agreement gate passed.

```bench-matrix
keys: os, driver, backend
measures: prefill, decode
ids: llm/prefill-512, llm/decode-1seq, llm/decode-4seq
```

The legend follows. OS is `Ubuntu 26.04` (Proton, native), `Ubuntu 24.04 / WSL2` (Phoenix), or `Windows 11` (Phoenix, native). The Windows row appears when both engines' Windows builds are green. Driver is the NVIDIA driver with its CUDA version, the ROCm version, or the Vulkan driver (RADV, or the vendor ICD) as reported at run time. Model is the name, parameters, quantization and file checksum. Platform is `cajeta-llm <version> <backend>` or `llama.cpp <commit> <backend>`.

## Standing

Written after the first published run.
