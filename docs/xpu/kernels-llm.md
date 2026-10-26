# Language models on the device: the cajeta-llm kernels

How `dev.cajeta.llm` runs a transformer on an accelerator, kernel by kernel:
what each family computes for the model, how the work is cut across lanes,
waves and workgroups, how a kernel is chosen for a weight format and a batch
shape on the device the process landed on, and where the library measures
rather than assumes. Everything is written in Cajeta against the portable
`cajeta.xpu` surface described in [One kernel, any device](README.md); the
library contains no vendor code and the same source serves NVIDIA through
NVPTX, AMD through AMDGPU, any Vulkan device through SPIR-V, and the host CPU.

The engine that drives these kernels (loading, the scheduler, the K/V
cache, the API, serving) is documented under
[Language models](llm/README.md); this page is about the kernels.

Two facts about the workload decide the kernel designs. **Decode is a
bandwidth problem**: generating one token reads every weight byte of the model
once and does almost nothing with it, so a decode kernel's only figure of merit
is bytes per second against the memory bus. **Prefill is a compute problem**:
processing a prompt multiplies every weight by hundreds of activation rows, so
a prefill kernel lives or dies by how it feeds the matrix units. Between them
sits batched decode, several sequences sharing one pass over the weights, and
the mixture-of-experts models, whose weights are read through an index of
which expert each token chose.

## 1. The weights, and what the kernels read

Weights arrive quantised in the GGUF block formats: K-quants (Q2_K through
Q6_K, 256 values a block with per-sub-block scales), the legacy formats (Q4_0,
Q5_0, Q4_1, Q5_1, Q8_0), the IQ codebook formats (IQ2_XXS through IQ4_NL,
IQ4_XS), the ternary TQ1_0 and TQ2_0, MXFP4, and f16. Every format has a
host-side reference implementation, and the device kernels for a format are
optimisations above that floor: a model whose format no device kernel serves
still runs, on the host.

Activations are quantised on the device to q8_K (`q8kPackKernel`, 256 values
a block, one scale and eight sub-block sums), so that the integer dot product
(`dotAccum`, `dp4a` on NVIDIA, the equivalent on AMD and Vulkan) does the
arithmetic. The per-sub-block scales of the K-quants then fold into a
rank-one epilogue. This is the same arithmetic llama.cpp's CUDA kernels use,
and the parity harness compares the two kernel for kernel (§6).

At bind the library may relay out a weight for its device: the Q6_K blocks are
split into a scale prefix and 208-byte bodies so that the tile kernels can read
them with aligned 16-byte loads; a Q4_K slab is viewed as 32-bit words for the
kernels that widen nibbles in registers; the attention projections may be
fused q|k|v into one weight so one launch serves three.

## 2. The kernel families

About 255 kernel sources register 265 kernels on CUDA (class templates are
instantiated at two shapes each) and 212 on the CPU backend. By family:

### 2.1 Decode mat-vecs: one row, one wave

| family | kernels | formats |
|---|---|---|
| `*Q8WaveMatVecKernel`, and prefetching `Pf` twins for Q4_K and Q6_K | ~24 | Q4_K, Q6_K, Q2_K, Q3_K, Q5_K, Q4_0, Q5_0, Q8_0, IQ4_NL, the IQ codebooks, TQ1_0, TQ2_0 |
| fused forms: `qkvWaveMatVec`, `*GateUpGlu` | ~6 | the three attention projections, or gate and up with the activation, in one launch |
| f32-activation waves `*F32WaveMatVecKernel` | 5 | Q8_0, f16, Q4_0, Q5_0, IQ4_NL (where the q8_K pack is not available) |
| the older one-item-per-row scalar mat-vecs | ~18 | the host-order reference shape, kept as the floor and the oracle |

One wave owns one output row. Consecutive lanes read consecutive bytes of the
row's blocks as whole words (the compiler lowers an aligned byte-vector load
to a word load; counting `ld.global.b8` in the PTX is how a kernel that lost
that is found). Each lane dots its bytes against the matching q8_K
activation bytes with the integer dot product, the per-sub-block scale folds
in, and a wave reduction finishes the row. The prefetch twins issue the next
block's loads before the current block's arithmetic; whether that pays is
measured per device (§4).

The lane index is `tid % Group.width()` throughout (fifty-one sites): the
same source runs at wave 32 and wave 64. Kernels whose indexing cannot be
width-free declare `@Wave(width = 32)` and are built for a 32-lane wave on
every backend.

### 2.2 Batched decode: the Cols twins

| family | kernels | formats |
|---|---|---|
| `*Q8WaveMatVecColsKernel`, with `Pf` and two-row `R2` variants | 12 | Q4_K, Q6_K, Q5_K, Q8_0, Q2_K, Q3_K |
| `ColsSegPlain`, `ColsSegPf` | 2 | q\|k\|v and gate\|up fused into one launch over the column set |

When several sequences decode together, one pass over a weight row can serve
all of them: the Cols kernels read the row once and dot it against up to
`WAVE_COLS_MAX` activation columns, which is how four sequences cost close to
one in weight traffic. On a Llama-3.1-8B at four sequences this is where the
library sits at 0.89 to 0.97 of llama.cpp on an RTX 4090 across six quant
formats (§6). The kernels exist for exactly the six formats the batched decode
admits; a format without a Cols kernel refuses the batch with a named reason
and the sequences decode one at a time.

### 2.3 Prefill tiles: feeding the matrix units

| family | kernels | formats | shapes |
|---|---|---|---|
| `QkTile`, `Q5Tile`, `Q6Tile` (class templates `<TM, TN, WM, WN, KS>`) | 3 templates × 2 shapes | Q4_K, Q5_K, Q6_K | 128×128 (256 threads) and 64×64 (128 threads) |
| `MwTile` (packed multi-wave Q4_K) | 1 template × 2 shapes | Q4_K | 128×128 and 64×64 |
| the int8 "deq" GEMMs `*WmmaDeqMw8` and kin | ~15 | Q4_K, Q6_K, Q5_K, Q8_0, Q2_K, Q3_K, the legacy symmetric pair | |
| the f16 cooperative GEMMs `*F16Coop*` | ~55 | every format, through an f16 staging of the weight | 128×128 |
| dp4a batch GEMMs, Vulkan's LDS-tiled `*Q8MmqKernel` | 5 | Q4_K, Q6_K | |
| `tcSplitFixupKernel` | 1 | the split-K reduction | |

The tile templates are the design that reaches llama.cpp's pace. A workgroup
of `2 × TN` threads computes a TM-token by TN-row tile: every thread stages one
weight column's nibble half into a dequantised int8 tile in shared memory and
eight consecutive threads copy one token's 128 activation bytes beside it,
both one half block ahead of the arithmetic, so the loads of the next half
overlap the matrix-multiply of this one. The waves then load 16×16 fragments
from shared memory with the matrix load, multiply on the tensor cores, and fold
the per-sub-block scales through the accumulator epilogue
(`TileOps.scaledAccumI32`, `rank1Accum`). A Q5_K member differs from the Q4_K
one only in the staging, where the fifth-bit plane is OR-ed into bit four of
each staged byte; a Q6_K member reads the split layout. Split-K adds
workgroups along the reduction when a problem is too small to fill the device,
and the fixup kernel sums the partials.

The shape of a tile is not fixed by the author: §4.2 describes how the
128×128 and 64×64 members are filtered against the device and timed against
each other at each problem the engine meets.

### 2.4 Mixture of experts: indexed GEMMs and mat-vecs

| family | kernels | notes |
|---|---|---|
| decode id mat-vecs `*Q8IdMatVecKernel`, fused `IdGateUpGlu`, `IdDownCombine` | 12 | one launch over every token's selected experts, llama.cpp's `mul_mat_vec_id` shape |
| WMMA id GEMMs `q4k/q5k/q6kWmmaIdMwKernel`, the 16-row `Epi` and 128-row wide variants | 9 | the multi-wave shape: 8 waves, each a 16-column slice of one expert over up to 64 token rows |
| `IdTile` (Q4_K), `Q5IdTile` (Q5_K) and `Q6IdTile` (Q6_K), class templates over the expert chunk maps | 3 templates × 2 shapes | the prefill tile design (§2.3) applied to expert banks |
| `IdHandTile` | 1 | the multi-wave kernel listed as a family member, so timing can retire it |
| the f16 coop id GEMMs | 2 | IQ3_XXS, IQ4_NL |
| router and grouping: `routerF32MatVec`, `routerTopK`, `routerTopKWave`, `rmsnormRouterTopK`, `routerRowsF32`, `routeHist`, `moeTopK(Batch)`, `moeGroupKernel` | ~10 | the gate, top-k, and the device counting sort into 64- and 128-row chunk maps |
| gather, scatter, combine, replicate | ~6 | the plumbing that groups tokens by expert and sums the experts' outputs |

For a prefill, tokens are sorted by the expert they chose into chunk maps on
the device (`moeGroupKernel`), and one launch of an id GEMM covers every
(expert, chunk) pair: a workgroup reads the map entry, finds its expert's slab
and its token rows, and computes as the dense tile does. Rows past a chunk's
end belong to the next expert and never land; a ragged tail stores through a
per-wave shared-memory tile, row by row. For a decode, every token's eight
selected experts are visited by one id mat-vec launch per bank, and a fused
down-projection kernel sums the experts' outputs as it goes.

### 2.5 Attention

| family | kernels | notes |
|---|---|---|
| decode: `qkPrep` (norm, rope, K/V write fused), `attnFlashDecodeGqa1/4/8`, `attnFlashDecodeReduce`, `attnReducePackQ8` | ~8 | split-K over the context, partials reduced and re-quantised to q8_K in one kernel |
| batched twins `qkPrepBatch`, `attnFlashDecodeGqa4/8Batch`, `attnReducePackQ8Batch` | 4 | one launch attends every sequence of the batch |
| prefill: `attnFlashPrefill`, `attnFlashPrefillGqa4`, `attnFlashPrefillTile` (f16 matrix cores), `attnFlashPrefillOnline`, the row-wise scores and combines | ~9 | |
| `kvAppend(Range)`, `rope*`, `attnScore`, `attnCombine` | ~4 | the unfused tail, kept as the oracle |

The grouped-query kernels are specialised by the ratio of query heads to
key/value heads (1, 4, 8) at head dimension 128 so that one workgroup reads a
K/V block once for every query head that shares it. A batched twin is
generated from the per-row kernel it is compared against, and the test holds
them byte-equal: the one time a batched kernel was built from a sibling's body
instead, it drifted by one quantisation step and the test caught it.

### 2.6 Utility

Normalisation, activation and residual kernels (`rmsnorm*`, `glu*`, `add*`,
`axpby`, `rope`, `qkNormRows`), the embedding gathers for four formats, the
weight relayouts (`blockPad`, `split*`, `blockRepack2`, the widen kernels), and
the device q8_K quantiser. Nineteen of these sit in `Prim`.

## 3. How a kernel is chosen

The library never asks which vendor it is running on to choose a kernel. It
asks three questions the layer keeps apart: can this device run the kernel,
how fast is it here, and which admitted kernel is the policy. The answers come
from the kernel registry, from measurement, and from the route, in that order.

### 3.1 The capability question

`KernelCap.usable(name)` is the gate every route passes through. It is
`Device.kernelAvailable(name)`, meaning the active backend holds device code
for the kernel and its shared memory fits this device, and the kernel is not
in the library's defect registry for this backend (a kernel known to lower
wrongly on one backend, listed by name against the plan item that tracks it).
The answer is latched per kernel for the process. A kernel the compiler could
not lower for a backend was declared so at build time (`@Unlowered`, see the
[overview](README.md)); a kernel the registry does not hold is not a candidate,
and nothing falls through to it.

### 3.2 Decode, one row

Per weight, once, after the device setup: the format's wave kernel is taken
when it is usable, the device quantises activations to q8_K, the input width
is a multiple of 256, and the format matches. Otherwise the f32-activation
wave, otherwise the scalar kernel, otherwise the host. The prefetch variant of
the Q4_K and Q6_K kernels is taken when the per-device probe (§4.1) said it
pays. Wave routes are never chunked across launches; the scalar routes are, by
a measured row count that is zero on the CPU backend.

### 3.3 Batched decode

The engine latches `decodeBatchReady` once: the K/V cache is device resident,
the attention kernels are usable, the head dimension is at most 128, the
hidden and query widths are multiples of 256, and **every** projection and the
output head answers `colsReady`: no epilogue, packed activations, and a Cols
kernel for its format in the registry. Mixture-of-experts layers must also
offer the zero-sync row path. Any refusal is recorded as `decode-batch
refused: <why>` and the sequences decode singly. At each batched launch the
Cols probe's answer for the format at this width decides between one Cols
launch and a single-row launch per sequence; the fused q|k|v and gate|up
launches run only where the load-time A/B said they pay.

### 3.4 Prefill

`batchRefusal(rows)` runs first and answers with the gate that refused or
`null`: quantised, routing enabled, a batch GEMM here, a WMMA-capable
backend, a route for the format (Vulkan: the cooperative GEMM when its column
constraints hold; native: a format kernel or the cooperative route), the Q5_K
tile, the int8 widen route for the formats it serves. A refused projection
runs row by row. For an admitted one, the arms in order:

1. ragged rows are padded to the tile grain where matrix cores pay;
2. the **tensor-core tile** for Q4_K, Q5_K, Q6_K: wmma and tile switches on,
   not Vulkan, not an expert slot or a sharing window, `matrixCoresPay()` and
   `tilePreferred()` true, the family listed as available, and the family's
   `fits` at the padded row count. A refusal is announced once with every
   gate's value;
3. the **int8 widen GEMMs** (`deqMw8` and kin), where the prefill-weight
   policy admits the format (under `auto`, Q4_K, Q5_K, Q6_K, Q3_K, Q8_0 and the
   legacy pair; never Q2_K);
4. the packed WMMA GEMMs for Q4_K and Q6_K;
5. the **f16 cooperative GEMM**: always on Vulkan, and off Vulkan only for
   formats with no native int8 batch kernel;
6. on Vulkan, the LDS-tiled dp4a `mmq`;
7. the dot-product batch GEMM, the floor.

Each arm announces itself once per route through a `batch-route` diagnostic,
so a run's trace names every kernel that served it. Two of the gates are
measured once per process: `matrixCoresPay()` times a matrix-core GEMM
against the dot-product kernel on a synthetic 4096² Q4_K and admits the matrix
cores only if they win by more than ten percent (on the CPU backend they do
not, and the floor serves); `tileBeatsDeq()` times the tile against the int8
widen route at a 512-row gate shape, and `tilePreferred` follows it under the
`auto` weight policy.

### 3.5 Mixture of experts

Per bank, in order: the **Id family** of the bank's format (Q4_K or Q5_K,
rows a multiple of 128, columns of 256, and a listed member with device code
that fits the batch's row bucket), the **multi-wave id GEMM** by format, the
**cooperative id GEMM** for the codebook formats, the 16-row epilogue id GEMM.
A readiness answer is a promise to launch: an arm that said ready and did not
launch throws, because the layer's output would otherwise be whatever the
allocator left in the buffer (found under device-memory poisoning, which fills
every fresh buffer with 0xFF so that an unwritten read answers NaN
deterministically).

## 4. What adapts, when it measures, and what it remembers

### 4.1 At load

Probes that run once when a model is loaded, each remembering its answer in
the engine's tune store (§4.4):

| probe | what it decides | how it measures |
|---|---|---|
| the Cols probe, per format and batch width | whether the Cols kernel beats single-row launches at this width | each candidate is first checked against the host mat-vec, then timed |
| the fused-Cols A/B, after the warm-up at the engine's batch width | whether the fused q\|k\|v and gate\|up launches beat separate ones | an in-situ A/B on the real batched step |
| the mat-vec variant probe (`probeArms`) | prefetch, plain, or two-row | timed brackets of twenty launches, interleaved rounds, medians |
| the tile prewarm (`prewarmQ4/Q5/Q6`) | the tile member for each prefill shape the engine will meet (`maxSeqs × chunk` rows) | the family's measured choice (§4.2), run ahead of the first prefill |
| `matrixCoresPay`, `tileBeatsDeq` | whether the matrix cores pay at all here; the tile against the widen route | once per process, in memory only |

### 4.2 Per problem: the tile families

A tile family is a list of shapes of one kernel design (`TileFamily` over
`QkTile<…>`, `Q5Tile<…>`, `Q6Tile<…>`, `MwTile<…>`, `IdTile<…>`,
`Q5IdTile<…>`, `Q6IdTile<…>`). At a problem `(rows, outDim, cols)` the family builds a
`ShapeFilter` over the device's facts (`DeviceFacts.current()`: register file,
shared memory per block and per multiprocessor, threads and blocks per
multiprocessor, multiprocessor count, from the driver) and the compiler's
measured facts about each member (`KernelFacts.measured`: registers a thread,
spill bytes, shared bytes, from the kernel manifest). The filter prunes by
rule, each pruned member kept with its reason: any spill; registers times
threads over the register file; shared bytes over the per-block limit; no
resident workgroup. Two more rules advise without pruning: whether the grid
fills the multiprocessors or needs split-K, and the padding the shape wastes.
A fact the backend does not report prunes nothing. An empty survivor set is a
named refusal, never a silent fallback.

Survivors are timed through the family's probe, interleaved and repeated
(five rounds, the fastest median wins, a lead under three percent is a tie),
and the winner is remembered under `shape.<family>.<M>x<N>x<K>` against a
build id made from every survivor's code hash, so a rebuild that changes any
candidate re-measures. A measurement taken while another process held more
than a quarter of the device is used but not stored.

The Id families time **in place**: there is no synthetic launch. Each
mixture-of-experts layer between two layer marks is one sample of one
survivor, bracketed by `KernelTimer` on the device clock (or the host clock
where the timer tier is unavailable), until every survivor has its rounds.
The load-time warm-up holds the trial so that the first prefill a user sees
runs the untimed choice. A fresh store therefore runs its trial through the
first two prefills of a shape, with the stream synchronisations that timing
needs; a measurement of a 30B prefill taken before the store settled read
three times the kernel time (2026-10-24), which is why the parity harness
reads a prefill only after the store has settled.

### 4.3 Per launch

Padding to the tile grain, the split-K count for a problem that does not
fill the device, the choice between one Cols launch and several single-row
launches, and the batched-decode admission are decided per launch from the
problem and the remembered probes, without timing.

### 4.4 The tune store

The store is `cajeta.xpu.Autotune`: one small file per (device, name) under a
directory the engine resolves from its option, else `CAJETA_LLM_TUNE_DIR`
(`off` disables persistence), else the platform cache directory with
`cajeta-llm/tune` appended. The device key is the backend plus the geometry the
answer depends on, so a store copied between machines is ignored rather than
believed. Every entry written against a build id is read back only against
the same build id; a stale entry is discarded and reported. The test suite
runs with the store off, so no earlier run's verdict reaches a test, and the
tests pin the probes' answers (`ColsProbe.setPays`, `setShapeOverride`) where
a route is asserted.

Keys the store holds today:

```
shape.q4kTcTile.<M>x<N>x<K>      shape.q5kTcTile…   shape.q6kTcTile…
shape.q4kWmmaMw.<M>x<N>x<K>      shape.q4kIdMw.<bucket>x<out>x<cols>
shape.q5kIdMw.<bucket>x<out>x<cols>        (each with a .margin twin)
colsPays-<type>-b<nc>            colsSeg-{qkv|gateup}-<rows>x<cols>-b<nc>
mvVariant-<type>-<rows>x<cols>-c<ncols>
partWgs
```

## 5. Where the backends differ, and how the code says so

The library encodes a backend's name in exactly one decision: `backendIsVulkan`
turns off the WMMA, tile and widen routes there, because the accumulator
epilogue demotes to a software tile that trips the display ring's watchdog and
the Q6_K WMMA loses the RADV context; Vulkan batches through the cooperative
f16 GEMM, the `mmq` tile, or the dot-product floor. Everything else is a
capability or a measurement:

| difference | how the kernel adapts |
|---|---|
| wave width 32 or 64 | `Group.width()` at every lane index; `@Wave(width = 32)` where the indexing needs it, built for 32 lanes on every backend |
| matrix cores present, absent, or slower than the integer dot | `matrixCoresPay()` measured once; the floor otherwise |
| the B-operand fragment layout (AMD builds a fragment from a lane's four words; NVIDIA cannot) | `fromWords` lowers to a register shuffle on AMD and to a shared-memory staging pass on NVPTX, in the compiler, so the kernel source is one |
| shared memory per block (49 KB default and 101 KB by opt-in on an RTX 4090; 64 KB on gfx1151) | the 128×128 tile opts in where it can and is pruned by the filter where it cannot; the 64×64 member serves |
| registers per thread | `@Occupancy(maxThreads = 2 × TN)` on the tiles and `maxWaves = (TM/WM)(TN/WN)` on the multi-wave kernels budget the register file for the real launch block; a kernel that spills is pruned, and the build's spill gate names it |
| a construct one backend cannot lower | `@Unlowered(backend, tracked)` on the kernel; the route sees no registered kernel and takes the next arm; the test suite's census (§6) refuses an untracked skip |
| the host's SIMD width on the CPU backend | the work-item loop vectorises at the host's width, or at the declared wave; holds conditional on the host width (`hostWaveBelow`) where a kernel lowers on a 16-lane host and not an 8-lane one |

## 6. How it is measured

**The census.** After every test run on a backend, every registered kernel
must be CHECKED (some test credited it against a host or oracle value
through `ValueCheck`), or RAN-UNCHECKED or SKIP against a named plan item, or
a PROBE or EXTERNAL kernel. An UNCOVERED kernel fails the suite, as does an
excuse that has gone stale. The tables are committed per backend
(`census/nvptx.tsv`, `amdgpu.tsv`, `cpu.tsv`); on CUDA 239 kernels are
CHECKED today.

**The oracle map.** Every shipped kernel has a row naming its llama.cpp
counterpart (`mmq.cu:mul_mat_q`, `mmvq.cu:mul_mat_vec_q`,
`fattn.cu:flash_attn_ext`, …) or NONE, and the shape it is compared at. The
parity harness (`tools/parity`) tallies both programs' kernels under the same
CUPTI injector and compares them launch for launch.

**The gate on a route change.** Any change to which kernel serves a weight is
gated by `GreedyVsLlama`: cajeta's greedy walk on the same prompt as
llama.cpp's, 64 tokens, agreement at or above 0.99, every divergence at a
top-2 gap under 1e-3. A batched number is read only with its greedy stream:
a stream that is one real token and then zeros is NaN logits, not a
measurement. On 2026-10-24 four quant formats decoded garbage at full speed
for a morning because the batched route's dispatcher knew two kernels while
its admission knew six; a test through the route, not the kernels, caught it.

**Where it stands** (RTX 4090, Llama-3.1-8B-Instruct, prompt 512, ours
against llama.cpp in the same minute, 2026-10-24):

| | Q2_K | Q3_K_M | Q4_K_M | Q5_K_M | Q6_K | Q8_0 |
|---|---|---|---|---|---|---|
| 4-sequence decode | 0.91 | 0.97 | 0.95 | 0.90 | 0.89 | 0.94 |
| single-sequence decode | ≥ 1.0 | ≥ 1.0 | ≥ 1.0 | ≥ 1.0 | ≥ 1.0 | ≥ 1.0 |
| prefill | 0.42–0.50 | 0.42–0.50 | 1.1–1.2 | 0.69 | 1.1–1.2 | 0.96 |

Qwen3-Coder-30B-A3B (mixture of experts, 128 experts, 8 active, Q5_K_M):
single-sequence decode 0.80 to 0.83, four-sequence decode 0.9 to 1.04,
four-sequence prefill 0.46 to 0.69.

**A lesson the measurements taught.** The Q5_K expert GEMM ran at 536 us a
launch on the 30B against llama.cpp's 225. Reading its instruction stream
would have been the wrong first step: the dense Q5_K tile, run on the
equivalent problem (64 rows against the whole expert slab laid out as one
weight), took 289 us for the same bytes. The gap was the kernel design, not
the format: the multi-wave kernel read both operands from global memory inside
its dependent multiply chain with no prefetch and staged every B fragment
through shared memory, and sat latency-bound at a quarter of the bandwidth
with 111 registers. Moving the tile design over the expert chunk maps took the
launch to 293 us in an afternoon. When a kernel reads far below the bus, time
the design that reaches the bus on the equivalent problem before anything else.
