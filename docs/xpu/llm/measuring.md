# Measuring, and where it stands

How the engine's speed and correctness are measured against llama.cpp, the
rules a number must satisfy before it is believed, and the standing on the
reference hardware.

## 1. Two gates before a number

**Correctness first.** A route change passes only when `GreedyVsLlama` agrees
with llama.cpp's greedy walk on the same prompt ids: 64 tokens, an agreement
rate at or above 0.99, and every divergence at a position where llama.cpp's
own top-2 logit gap is under 1e-3 (a near tie, where the two implementations
may legitimately differ). The reference walk comes from `llama-greedy`, a
small program built against the llama.cpp build on the box that records the
walk with its gaps.

**Read the tokens with the throughput.** The benchmark prints every generated
token; a batched throughput figure is read only beside them, compared token
for token with the single-sequence walk of the same run. A stream that is one
real token and then zeros is NaN logits, not a measurement. Four quant formats
once decoded garbage at full speed for a morning because the batched route's
dispatcher knew two kernels while its admission knew six; the test that caught
it goes through the route, not the kernels.

## 2. The instruments

| instrument | what it answers |
|---|---|
| `bench/SchedThroughput <gguf> seqs=N prompt=P gen=G chunk=C ctx=X [vary] [trace] [tune=DIR]` | end-to-end prefill and decode throughput through the scheduler at N sequences; `vary` gives each sequence a different prompt, `trace` prints the route, batch and phase records |
| `llama-batched-bench -npl N -npp P -ntg G` | llama.cpp's equivalent, run in the same minute; the GPU clock is bimodal, so pairs are interleaved and never read alone |
| `tools/parity/cupti-tally.cpp` with `kernel-tally.py` | a CUPTI injection library that tallies every kernel's device time in any CUDA program, graph-launched kernels included; the window and gaps view attribute a step kernel by kernel on both sides |
| `tools/parity/run-parity-legs.sh` | one matrix-multiply shape per row, device-timed on each side's own stream, joined into `census/parity-<backend>.tsv` under five rules (same op, type and shape; the engine labelled with its build; the same residency; the device timer tier; a calibration row that reproduced a recorded number) |
| `tools/parity/run-greedy.sh` | the correctness gate above |
| `census/oracle-map.tsv`, `census/<backend>.tsv` | every shipped kernel's llama.cpp counterpart, and every registered kernel's census standing after the test run |
| `CAJETA_PROFILER=1` | the whole-run profile, host and device, with the device spans at their tier |

A measurement is taken in interleaved rounds, ours then llama.cpp, in the
same minute, and the median is read; a single pair is never believed. On the
30B a prefill is read only after the tune store has settled (its first two
prefills on a fresh store carry the Id family's in-place trial). The
attribution rule that found every lever this autumn: tally first, then read
the kernel.

## 3. Where it stands

RTX 4090 under WSL2, llama.cpp built for CUDA, same minute, 2026-10-24.
Ratios are ours over llama.cpp; 1.0 is parity.

**Llama-3.1-8B-Instruct**, prompt 512, 128 generated:

| | Q2_K | Q3_K_M | Q4_K_M | Q5_K_M | Q6_K | Q8_0 |
|---|---|---|---|---|---|---|
| decode, one sequence | ≥ 1.0 | ≥ 1.0 | ≥ 1.0 | ≥ 1.0 | ≥ 1.0 | ≥ 1.0 |
| decode, four sequences | 0.91 | 0.97 | 0.95 | 0.90 | 0.89 | 0.94 |
| prefill | 0.42–0.50 | 0.42–0.50 | 1.1–1.2 | 0.69 | 1.1–1.2 | 0.96 |

**Qwen3-Coder-30B-A3B-Instruct** (mixture of experts, 128 experts, 8
active, Q5_K_M), prompt 512, context 1024 a sequence:

| | ours | llama.cpp | ratio |
|---|---|---|---|
| decode, one sequence | 162–167 tok/s | 196–210 | 0.80–0.83 |
| decode, four sequences | 437–479 | 475–547 | 0.9–1.04 |
| prefill, four sequences | 2963–3315 tok/s | 4755–7210 | 0.46–0.69 |

The single-sequence prefill of the 30B swings 1.75x between runs a minute
apart on this box (the GPU clock), so it is read at four sequences.

Where the remaining gaps are, kernel by kernel, is the subject of
[Language models on the device](../kernels-llm.md): the 8B's Q2_K, Q3_K and
Q5_K prefill routes, the 30B's grouping sort (12 ms against llama.cpp's 2 in
a 512-token prefill), its Q6_K expert banks still on the older id GEMM, and
the mixture-of-experts single-sequence decode.

## 4. The test gates

`run-tests.sh` builds the library and the test binary for one backend
(`XPU_BACKEND=nvptx|amdgpu|vulkan|cpu`, `CAJETA=` the compiler, `CUDA_PATH`
for the assembler) and runs the suite: on 2026-10-24 the nvptx leg is 849
tests in eight minutes, the cpu leg about 80 minutes and needs a quiet box.
After the suite the kernel census requires every registered kernel to have
been value-checked or excused by name, and the oracle-map test requires every
shipped kernel to have a row. `CAJETA_UNIT_FILTER=Name,Name` runs a subset
while iterating, without the census; a filtered run is never the leg.
