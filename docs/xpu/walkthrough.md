# One model, start to last token

What happens when `dev.cajeta.llm` runs **Meta-Llama-3.1-8B-Instruct at
Q4_K_M**, from the first instruction of the process to the last generated
token, on the two GPUs the engine is measured on. Every step below answers
the same four questions: what happens, what code runs, where the NVIDIA and
AMD paths part, and what the optimizer decided and when. The pieces are
documented on their own pages; this page is the thread through them.

The run: a 512-token prompt, 128 generated tokens, four sequences at once,
the shape every number on the [benchmarks](../../bench/llm/README.md) is read
at. The engine is `dev.cajeta.llm` at commit `04227de` (2026-10-25) on
cajeta 0.46.3; the reference machines are Phoenix (RTX 4090, `sm_89`, CUDA
13.3, under WSL2) and Proton (Radeon 8060S, `gfx1151`, ROCm). Kernel names
in `code` are the names in the [catalog](catalog-llm.md), one row each.

## The model, as the engine sees it

| fact | value |
|---|---|
| architecture | `llama`, 32 layers, hidden 4096, feed-forward 14336 |
| attention | 32 query heads, 8 key/value heads (grouped-query ratio 4), head dimension 128, rotary base 500000 with the Llama-3.1 frequency table |
| vocabulary | 128256 tokens, the GPT-2-style byte-pair tokenizer from the file |
| tensors | 292, 4.58 GiB |
| formats | query, key, output, gate and up projections **Q4_K** in every layer; the value projection and the down projection **Q6_K** in 16 layers and Q4_K in the other 16; the output head **Q6_K**; the embedding table Q4_K; the norms and the rotary table f32 |
| engine defaults | context 4096, four scheduler slots, 16-token K/V blocks, an adaptive prefill chunk |

"Q4_K_M" is a recipe, not a format: a route is chosen **per tensor**, so this
one model exercises the Q4_K and the Q6_K kernel families on every step, and
the Q6_K output head runs on the decode path 128 times per sequence.

<figure class="diagram">
<svg viewBox="0 0 760 330" role="img" aria-label="The five layers of the compute stack and the path this walkthrough takes through them: the LLM engine uses Math's Tensor and XPU's kernels directly; nucleo and ml sit beside it on the same substrate." style="max-width:100%;height:auto;font-family:var(--font-ui);font-size:12px">
  <defs><marker id="wa" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="currentColor"/></marker></defs>
  <g fill="none" stroke="currentColor" stroke-width="1.2">
    <rect x="20" y="250" width="720" height="56" rx="4"/>
    <rect x="20" y="170" width="720" height="56" rx="4"/>
    <rect x="20" y="90" width="340" height="56" rx="4"/>
    <rect x="400" y="90" width="340" height="56" rx="4"/>
    <rect x="20" y="10" width="340" height="56" rx="4"/>
    <rect x="400" y="10" width="340" height="56" rx="4" stroke="var(--accent)" stroke-width="2"/>
  </g>
  <g fill="currentColor">
    <text x="32" y="272" font-weight="600">XPU</text>
    <text x="32" y="290" opacity="0.75">cajeta.xpu: @Kernel, Device, KernelBuffer, KernelStream, KernelTimer; manifest, adaptor, ShapeFilter, ShapeChoice, Autotune</text>
    <text x="32" y="192" font-weight="600">Math</text>
    <text x="32" y="210" opacity="0.75">cajeta.math: Tensor, dtypes, linalg, fft, npio</text>
    <text x="32" y="112" font-weight="600">Nucleo</text>
    <text x="32" y="130" opacity="0.75">Column, Table, Tape, Fuse, Grad, Vmap, Jit</text>
    <text x="412" y="112" font-weight="600">LLM</text>
    <text x="412" y="130" opacity="0.75">dev.cajeta.llm: load, scheduler, CausalLM, K/V, sampling</text>
    <text x="32" y="32" font-weight="600">ML</text>
    <text x="32" y="50" opacity="0.75">dev.cajeta.ml, dev.cajeta.xgboost</text>
    <text x="412" y="32" font-weight="600" fill="var(--accent)">this walkthrough</text>
    <text x="412" y="50" opacity="0.75">Llama-3.1-8B Q4_K_M, 512 in, 128 out, 4 sequences</text>
  </g>
  <g stroke="currentColor" stroke-width="1.2" fill="none" marker-end="url(#wa)">
    <line x1="190" y1="66" x2="190" y2="88"/>
    <line x1="190" y1="146" x2="190" y2="168"/>
    <line x1="570" y1="66" x2="570" y2="88"/>
    <line x1="500" y1="146" x2="500" y2="168"/>
    <path d="M640,146 L640,240 L600,240 L600,248"/>
  </g>
  <g fill="currentColor" font-size="11" opacity="0.75">
    <text x="196" y="80">estimators over Tensor</text>
    <text x="196" y="160">columns, tape over Tensor</text>
    <text x="506" y="160">Tensor&lt;float32&gt; activations</text>
    <text x="646" y="200">KernelBuffer, @Kernel launches</text>
  </g>
</svg>
<figcaption>The stack, bottom-up, and the path this page follows. The LLM engine builds on Math and XPU directly; it does not pass through nucleo, which shares the same substrate beside it.</figcaption>
</figure>

## 1. Execution start

**What happens.** A program (the `cajeta run` command, cabra, or a caller
of the API) constructs `EngineOptions` with the model path and calls
`LlmEngine.load`. Nothing has touched a device yet. The first thing `load`
does is decide which device it will be.

**What code runs.** `LlmEngine.load` (`LlmEngine.cajeta:102`). If the options
name a device it calls `Device.force(name)` before any other device call;
otherwise the first device touch selects one backend from those the binary
bundles, in the fixed order **CUDA, HIP, Vulkan, CPU**, restricted by
`CAJETA_XPU_BACKEND` when set. The choice is per process and cached;
`Device.activeBackend()` names it. Next, `resolveTuneDir` (`:73`) settles
where this machine's measured choices live: the `tuneDir` option, else
`CAJETA_LLM_TUNE_DIR` (`off` keeps none), else the platform cache directory
with `cajeta-llm/tune` appended; the directory is handed to every kernel
store before a kernel is bound, so no probe can run without a place to
remember its answer.

The device is then **read, not looked up**. `Device` answers from the driver:
multiprocessor count, shared memory per block with and without the opt-in
ceiling, registers and threads per multiprocessor, the wave size, free
memory, the datasheet bandwidth, what other processes hold
(`foreignLoadPercent`). The kernel registry is the other half of the
question: `Device.kernelAvailable(name)` says whether this backend holds
device code for a kernel whose shared memory fits this part, and the library
wraps it as `KernelCap.usable(name)`, which also consults the library's own
defect registry and latches the answer per kernel for the process.

**NVIDIA against AMD.** On Phoenix the selection lands on `cuda`; the 4090
reports 128 multiprocessors, a 32-lane wave, 48 KB of shared memory per block
and 99 KB with opt-in, 100 KB per multiprocessor. On Proton it lands on
`hip`; gfx1151 reports a 32-lane wave for every kernel that declares
`@Wave(width = 32)` (the backend pins `+wavefrontsize32` at build), and 64 KB
of shared memory per block, which is also the whole per-compute-unit figure.
That one difference, 99 KB against 64 KB, is what later decides which prefill
tile member survives on each part. A Vulkan run (RADV on Proton, or the
NVIDIA ICD on Phoenix) reports its subgroup size and no event clock. Nothing
in the library reads the vendor's name to make a decision; the one place a
backend name is tested is `backendIsVulkan`, and §3 says why.

**How it optimizes.** Nothing is tuned yet, but two things that will decide
every later choice are fixed here: the device key under which the autotune
store files its answers (the backend plus the geometry that changes the
right answer, so a store copied between machines is ignored rather than
believed), and the registry of kernels this backend can run. A kernel the
compiler could not lower for this backend was declared so at build time
(`@Unlowered`, tracked against a plan item) and is simply absent; nothing
falls through to it.

## 2. Load

**What happens.** The file is memory-mapped, its metadata becomes the model
configuration, the tokenizer and the chat template are read from it, the
model is built and bound to the checkpoint tensor by tensor, and then the
engine makes the model **device resident** and runs every probe it can run
without a user's prompt, so the first request pays for none of them.

**What code runs**, in the order `LlmEngine.load` runs it:

1. `GgufFile` opens the checkpoint; `general.architecture = llama` and the
   `llama.*` keys fill `ModelConfig`; the tokenizer (`tok/`) and the
   end-of-generation set come from the same file. The host holds no second
   copy of the weights.
2. `DecoderModel` is built from the configuration and bound by tensor name:
   32 `DecoderLayer`s, each an `Attention` (four `Linear` projections) and an
   `Mlp` (three), plus the embedding, the final norm and the output head.
   There is no rename table; a tensor the layout does not expect is an error.
3. Residency. Each `Linear` uploads its packed weight to a `KernelBuffer`
   and performs the **relayouts** its device kernels want: a Q6_K weight is
   split into a scale prefix and 208-byte bodies (`splitScaleKernel`,
   `splitPayloadKernel`) so the tile kernels read it with aligned 16-byte
   loads; a Q4_K slab gets a 32-bit word view for the kernels that widen
   nibbles in registers. Under the default `auto` prefill-weight policy the
   formats whose int8 widen route pays get a resident int8 copy
   (`q4kWidenKernel`, `q6kWidenKernel`), inside the widen budget; decode
   always reads the packed bytes.
4. The K/V cache: on the host, `PagedKvCache` in 16-token blocks shared by
   reference count; on the device, `KvPool` holds one f16 K plane and one V
   plane per layer, slot `s` at element `s × plane`, so one batched attention
   launch reaches every sequence. The ceiling at four slots is reported as
   `kv-ceiling`.
5. `prewarmPrefillWeights`, then `prewarmBatchShapes(maxSeqs × chunk)`: for
   every projection, `Linear.prewarmTileAt(rows)` asks its `TileFamily` to
   settle the tile member for the shapes the engine will meet (§3 says how).
   `probeColsFor` runs the Cols probe for each format the batched decode will
   use, and `ColsSegProbe.prewarm` the fused q|k|v and gate|up variants.
6. The mixture-of-experts residency reserve (none for this dense model), then
   **one throwaway chunk-sized prefill and one decode step**, which run the
   remaining once-per-process probes: `matrixCoresPay`, `tileBeatsDeq`, the
   mat-vec variant probe (`probeArms`: prefetching, plain, or two-row), and
   the fused-Cols A/B at the engine's batch width. A `load-phase` record
   breaks the load into bytes moved, packs, prewarm and warm-up.
7. `Scheduler(model, chunk)` is built with the 150 ms chunk budget; the
   tokenizer is attached; the block store opens if configured.

**NVIDIA against AMD.** The same bytes go up on both. What differs is what
the prewarm decides: on the 4090 the 128×128 prefill tile opts into 99 KB of
shared memory and survives the filter; on gfx1151 it does not fit the 64 KB
block limit and the 64×64 member serves (§3). The int8 widen copies are made
where `tileBeatsDeq` says the widen route still pays for a format; the answer
is per device and per format and is not assumed from the part's name. On
Proton the K/V planes are allocated from the unified memory the device shares
with the host, and the engine counts and reports pool growth because on
WSL2 a 2 GiB mapping costs about 30 ms to create and as much to destroy.

**How it optimizes.** Load is where the store fills. After this step the
tune directory holds, for this device key: the tile member per prefill shape
(`shape.q4kTcTile.<rows>x<out>x<in>` and the Q6_K twin, each with a
`.margin`), whether the Cols kernel pays per format at the batch width
(`colsPays-<type>-b4`), the fused segment verdicts (`colsSeg-qkv-…`,
`colsSeg-gateup-…`), and the mat-vec variant per weight shape
(`mvVariant-<type>-<rows>x<cols>-c<ncols>`), every entry written against the
build id of the kernels it measured. A second load on the same machine reads
them back and runs no probe. `matrixCoresPay` and `tileBeatsDeq` are kept in
memory only, once per process.

## 3. Prefill

**What happens.** Four requests arrive with 512-token prompts. The scheduler
admits them (each one's whole uncached suffix fits in free K/V blocks, plus
one block per layer of decode headroom), and because all four are greedy and
the resident batched prefill is ready, it runs **one prefill over all their
rows**: 2048 rows through 32 layers, each projection as one GEMM. The
output of the step is one token per sequence, the first generated token.

**What code runs.** `Scheduler.stepOnce` collects the prompt chunks
(`ChunkPolicy.chunkForShare` picks the chunk; with nothing decoding it learns
the largest that fits, up to 512) and calls `CausalLM.stepPrefillBatch`
(`CausalLM.cajeta:826`), which checks `prefillBatchReady` and `batchReady(total
rows)` and otherwise falls back to one `stepSeq(seq, tokens, off, n)` per
request with a `prefill-batch-refused` record saying why. Inside a chunk:

- A `PrefillArena` sized to the chunk is carved into borrowed slices for the
  hidden state and every intermediate (`stepSeq:700`); nothing survives the
  call, so peak activation memory is one layer's at any depth.
- **Embedding on the device**: the token ids go up (2 KB for 512 tokens) and
  `embedRowQ4kKernel` gathers and dequantizes the Q4_K table rows into the
  hidden state where the layer stack reads it. The host decode of every row
  plus the upload it replaced cost 45 ms of a 118 ms prefill on `sm_89`.
- `forwardRows(rows)` runs the layers. Per layer, on the device throughout:
  1. the input norm fused with the q8_K activation pack
     (`rmsnormPackRowF32` when the width is a multiple of 256, else
     `rmsnormRowsF32` then `q8kPackKernel`): activations become 256-value
     q8_K blocks so the integer dot product does the arithmetic;
  2. the query, key and value projections, each through `Linear`'s **batch
     route** (§3.1), the Q4_K ones on the tensor-core tile
     `QkTile_128_128_32_64_128_q4kQ8` or its 64×64 member, the Q6_K value
     projection (in 16 layers) on `Q6Tile_…`;
  3. rotary embedding and the K/V write (`ropeAppendRowsKernel`,
     `kvAppendRangeKernel`, or the fused `qkPrepBatchKernel`), writing f16
     keys and values into the layer's planes of `KvPool`;
  4. flash attention over the sequence so far, `attnFlashPrefillGqa4Kernel`
     (one workgroup reads a K/V block once for the four query heads that
     share it) or the f16 matrix-core `attnFlashPrefillTileKernel`, the
     partials combined by `attnCombinePrefillKernel`;
  5. the output projection on its tile, accumulated into the residual
     (`addF32`);
  6. the post-attention norm and pack; gate and up projections on their
     tiles; the GLU fused with the next pack (`gluFusedPackF32`); the down
     projection (Q6_K in 16 layers) on its tile, accumulated.
- After the last layer only the last row of each sequence goes through the
  **tail**: the final norm, the Q6_K output head, and `argmaxRowKernel`, one
  integer per sequence read back; the sampler's temperature is zero in the
  measured runs, so the argmax is the token.

Every projection announces the arm it took once per route as a
`batch-route` record, so a run's diagnostic stream names every kernel that
served it.

<figure class="diagram">
<svg viewBox="0 0 900 300" role="img" aria-label="One transformer layer in prefill: the hidden state is normed and packed to q8_K, projected to q, k and v on tensor-core tiles, rotated and appended to the K/V planes, attended by the flash prefill kernel, projected back and added; then normed and packed again, through gate and up tiles, the GLU fused with the next pack, and the down tile added back." style="max-width:100%;height:auto;font-family:var(--font-ui);font-size:11px">
  <defs><marker id="pa" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="currentColor"/></marker></defs>
  <g fill="none" stroke="currentColor" stroke-width="1.2">
    <rect x="10" y="60" width="70" height="40" rx="4"/>
    <rect x="120" y="60" width="110" height="40" rx="4"/>
    <rect x="270" y="20" width="130" height="40" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <rect x="270" y="80" width="130" height="40" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <rect x="270" y="140" width="130" height="40" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <rect x="440" y="80" width="110" height="40" rx="4"/>
    <rect x="590" y="80" width="130" height="40" rx="4"/>
    <rect x="760" y="80" width="130" height="40" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <rect x="120" y="220" width="110" height="40" rx="4"/>
    <rect x="270" y="220" width="160" height="40" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <rect x="470" y="220" width="120" height="40" rx="4"/>
    <rect x="630" y="220" width="130" height="40" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <rect x="590" y="150" width="130" height="30" rx="4" stroke-dasharray="4 3"/>
  </g>
  <g fill="currentColor">
    <text x="45" y="84" text-anchor="middle">hs (rows×4096)</text>
    <text x="175" y="78" text-anchor="middle">norm + q8_K pack</text>
    <text x="175" y="92" text-anchor="middle" opacity="0.7">rmsnormPackRowF32</text>
    <text x="335" y="38" text-anchor="middle">q · Q4_K tile</text>
    <text x="335" y="52" text-anchor="middle" opacity="0.7">QkTile_128_128…</text>
    <text x="335" y="98" text-anchor="middle">k · Q4_K tile</text>
    <text x="335" y="112" text-anchor="middle" opacity="0.7">QkTile_…</text>
    <text x="335" y="158" text-anchor="middle">v · Q4_K or Q6_K tile</text>
    <text x="335" y="172" text-anchor="middle" opacity="0.7">QkTile_… / Q6Tile_…</text>
    <text x="495" y="98" text-anchor="middle">rope + K/V write</text>
    <text x="495" y="112" text-anchor="middle" opacity="0.7">qkPrepBatchKernel</text>
    <text x="655" y="98" text-anchor="middle">flash attention</text>
    <text x="655" y="112" text-anchor="middle" opacity="0.7">attnFlashPrefillGqa4</text>
    <text x="825" y="98" text-anchor="middle">o · Q4_K tile, += hs</text>
    <text x="825" y="112" text-anchor="middle" opacity="0.7">QkTile_…, addF32</text>
    <text x="655" y="169" text-anchor="middle" opacity="0.8">KvPool planes, f16, per layer</text>
    <text x="175" y="238" text-anchor="middle">norm + q8_K pack</text>
    <text x="175" y="252" text-anchor="middle" opacity="0.7">rmsnormPackRowF32</text>
    <text x="350" y="238" text-anchor="middle">gate, up · Q4_K tiles</text>
    <text x="350" y="252" text-anchor="middle" opacity="0.7">QkTile_… ×2</text>
    <text x="530" y="238" text-anchor="middle">GLU + pack</text>
    <text x="530" y="252" text-anchor="middle" opacity="0.7">gluFusedPackF32</text>
    <text x="695" y="238" text-anchor="middle">down · tile, += hs</text>
    <text x="695" y="252" text-anchor="middle" opacity="0.7">QkTile_… / Q6Tile_…</text>
  </g>
  <g stroke="currentColor" stroke-width="1.2" fill="none" marker-end="url(#pa)">
    <line x1="80" y1="80" x2="118" y2="80"/>
    <path d="M230,80 L250,80 L250,40 L268,40"/>
    <line x1="230" y1="100" x2="268" y2="100"/>
    <path d="M230,80 L250,80 L250,160 L268,160"/>
    <path d="M400,40 L420,40 L420,100 L438,100"/>
    <line x1="400" y1="100" x2="438" y2="100"/>
    <path d="M400,160 L420,160 L420,100"/>
    <line x1="550" y1="100" x2="588" y2="100"/>
    <line x1="720" y1="100" x2="758" y2="100"/>
    <line x1="495" y1="120" x2="495" y2="150" />
    <path d="M495,150 L495,165 L588,165"/>
    <path d="M655,150 L655,120"/>
    <path d="M825,120 L825,200 L60,200 L60,240 L118,240"/>
    <line x1="230" y1="240" x2="268" y2="240"/>
    <line x1="430" y1="240" x2="468" y2="240"/>
    <line x1="590" y1="240" x2="628" y2="240"/>
    <path d="M760,240 L800,240 L800,290 L45,290 L45,102"/>
  </g>
  <g fill="currentColor" font-size="10" opacity="0.7">
    <text x="500" y="145" >writes k, v</text>
    <text x="660" y="140">reads k, v</text>
    <text x="400" y="196">residual, after the output projection</text>
    <text x="400" y="286">residual, into the next layer</text>
  </g>
</svg>
<figcaption>One layer of the prefill, as the device runs it for 2048 rows. Tensor-core tiles (outlined in color) do the matrix work; everything between them is a fused norm, pack, rotary, attention or activation kernel. The dashed box is the layer's K/V planes, written here and read by every later step.</figcaption>
</figure>

### 3.1 How a projection chooses its GEMM

`Linear.batchRefusal(rows)` (`Linear.cajeta:2143`) runs first and answers
with the gate that refused, or null: quantized weight, routing on, a batch
GEMM kernel on this backend, the format admitted. For an admitted projection
the arms are tried in order (`:2600`), each gated by a capability the device
answered, a measurement, or a fit:

| arm | taken when | kernel for this model |
|---|---|---|
| tensor-core tile | not Vulkan, not an expert slot, `matrixCoresPay()` and `tilePreferred()` true, the family available, `TcTile.fits` at the padded row count | `QkTile_*` (Q4_K), `Q6Tile_*` (Q6_K); rows padded to the 128 grain |
| int8 widen GEMM | the policy admitted the format and the widen copy exists | `q4kWmmaDeqMw8Kernel`, `q6kWmmaDeqMw8Kernel` |
| packed WMMA GEMM | Q4_K or Q6_K without a tile | `q4kWmmaKernel`, `q6kWmmaKernel` |
| f16 cooperative GEMM | always on Vulkan; elsewhere only for formats with no int8 batch kernel | `q4kF16CoopKernel`, `q6kF16CoopKernel` |
| LDS-tiled dp4a | Vulkan, when the cooperative route's column constraints fail | `q4kQ8MmqKernel`, `q6kQ8MmqKernel` |
| dot-product batch GEMM | the floor | `q4kQ8BatchMatMulKernel`, `q6kQ8BatchMatMulKernel` |

A refused tile says so once with every gate's value (`q4 tcTile refused:
avail=… slot=… window=… int8Faster=… ragged=… fits=…`), so a route that fell
to a slower arm is never silent. Split-K adds workgroups along the reduction
when a problem does not fill the device and `tcSplitFixupKernel` sums the
partials.

**NVIDIA against AMD.** On `sm_89` the tile's int8 operands with int32
accumulators are the native `wmma` shape, so the fragments are the
hardware's and the multiply is the tensor-core instruction; the 128×128
member needs more than 48 KB of shared memory and opts into the 99 KB
ceiling. On `gfx1151` the same int8 shape is native WMMA at wave 32, but the
block limit is 64 KB with no opt-in, so the filter prunes the 128×128 member
under rule R3 and the 64×64 member (128 threads) serves; the
`fromWords` fragment build is a register shuffle on AMD and a shared-memory
staging pass on NVIDIA, inside the compiler, so the tile's source is one.
Under Vulkan the engine turns the WMMA, tile and widen routes off
(`backendIsVulkan`, the one backend-name test in the library): the
accumulator epilogue demotes to a software tile that trips the display
ring's watchdog and the Q6_K WMMA loses the RADV context, so Vulkan batches
through the f16 cooperative GEMM, the `mmq` tile, or the floor. The bf16
matrix form is native on NVPTX and the software tier on Vulkan; f32 the
other way round; f16 native on both, which is why the cooperative f16 GEMM
is the portable arm.


<figure class="diagram">
<svg viewBox="0 0 900 300" role="img" aria-label="Where the three backends part for this model: backend selection in a fixed order; on CUDA the int8 tiles are native wmma and the 128 member opts into 99 KB of shared memory, decode launches are replayed as a graph; on HIP the same tiles are native wave-32 WMMA but the 64 KB block limit prunes the 128 member so the 64 member serves, launches are issued one by one; on Vulkan the tile and widen routes are off and prefill goes through the f16 cooperative GEMM, the mmq tile or the dot floor, with no event clock." style="max-width:100%;height:auto;font-family:var(--font-ui);font-size:11px">
  <defs><marker id="sa" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="currentColor"/></marker></defs>
  <g fill="none" stroke="currentColor" stroke-width="1.2">
    <rect x="10" y="20" width="160" height="40" rx="4"/>
    <rect x="230" y="20" width="200" height="40" rx="4"/>
    <rect x="450" y="20" width="200" height="40" rx="4"/>
    <rect x="670" y="20" width="200" height="40" rx="4"/>
    <line x1="230" y1="80" x2="870" y2="80" opacity="0.4"/>
    <line x1="230" y1="150" x2="870" y2="150" opacity="0.4"/>
    <line x1="230" y1="220" x2="870" y2="220" opacity="0.4"/>
  </g>
  <g fill="currentColor">
    <text x="90" y="38" text-anchor="middle" font-weight="600">first device touch</text>
    <text x="90" y="52" text-anchor="middle" opacity="0.7">cuda → hip → vulkan → cpu</text>
    <text x="330" y="38" text-anchor="middle" font-weight="600">CUDA · RTX 4090, sm_89</text>
    <text x="330" y="52" text-anchor="middle" opacity="0.7">wave 32 · 48 KB, 99 KB opt-in</text>
    <text x="550" y="38" text-anchor="middle" font-weight="600">HIP · Radeon 8060S, gfx1151</text>
    <text x="550" y="52" text-anchor="middle" opacity="0.7">wave 32 pinned · 64 KB = per CU</text>
    <text x="770" y="38" text-anchor="middle" font-weight="600">Vulkan · RADV or the NVIDIA ICD</text>
    <text x="770" y="52" text-anchor="middle" opacity="0.7">virtual waves · no event clock</text>
    <text x="20" y="100" font-weight="600">prefill GEMM</text>
    <text x="20" y="114" opacity="0.7">Q4_K, Q6_K tiles</text>
    <text x="240" y="100">QkTile 128×128, native wmma int8</text>
    <text x="240" y="114" opacity="0.7">opts into 99 KB; R3 passes</text>
    <text x="460" y="100">QkTile 64×64, native WMMA int8</text>
    <text x="460" y="114" opacity="0.7">128 member pruned by R3 at 64 KB</text>
    <text x="680" y="100">q4kF16CoopKernel, else mmq, else dot</text>
    <text x="680" y="114" opacity="0.7">tile and widen routes off: backendIsVulkan</text>
    <text x="20" y="170" font-weight="600">decode mat-vec</text>
    <text x="20" y="184" opacity="0.7">one row per wave</text>
    <text x="240" y="170">q4kQ8WaveMatVecKernel (Pf if it pays)</text>
    <text x="240" y="184" opacity="0.7">tid % Group.width() = tid % 32</text>
    <text x="460" y="170">the same source, wave 32</text>
    <text x="460" y="184" opacity="0.7">fromWords is a register shuffle here</text>
    <text x="680" y="170">the same source as a virtual wave</text>
    <text x="680" y="184" opacity="0.7">W/S slots per invocation</text>
    <text x="20" y="240" font-weight="600">issuing a step</text>
    <text x="20" y="254" opacity="0.7">~1,360 launches</text>
    <text x="240" y="240">deferred stream, graph replay</text>
    <text x="240" y="254" opacity="0.7">about 1 ms of host time a step</text>
    <text x="460" y="240">one launch at a time</text>
    <text x="460" y="254" opacity="0.7">the batched pipeline hides the host</text>
    <text x="680" y="240">one dispatch at a time</text>
    <text x="680" y="254" opacity="0.7">host timing; the profiler's timestamp seam</text>
  </g>
  <g stroke="currentColor" stroke-width="1.2" fill="none" marker-end="url(#sa)">
    <line x1="170" y1="40" x2="228" y2="40"/>
    <path d="M200,40 L200,10 L550,10 L550,18"/>
    <path d="M200,10 L770,10 L770,18"/>
  </g>
  <g fill="currentColor" font-size="10" opacity="0.7"><text x="176" y="34">among the bundled</text></g>
</svg>
<figcaption>Where the backends part for this model. The source is one on every row; what differs is a fact the device reported (the shared-memory block limit, the wave), a tier the compiler recorded, or a mechanism one backend has and another lacks (graph replay, the event clock).</figcaption>
</figure>

**How it optimizes.** Three moments, three mechanisms.

- *Compile time.* `QkTile` is one body over `<TM, TN, WM, WN, KS>`;
  `@Instantiate` built the 128×128 (256 threads) and 64×64 (128 threads)
  members, `@Requires` refused any shape the body cannot serve, and
  `@Occupancy(maxThreads = 2 × TN)` budgeted registers for the real block
  (255 on the big member, no spill). The manifest beside each member
  records registers, spill, shared bytes, the pinned block, and that its
  matrix verbs lowered to `wmma` at the native tier. Two gates fail the build
  rather than ship a slow kernel: a spill, or a backend with no device code
  that no `@Unlowered` declaration tracks.
- *Bind.* `TileFamily` builds a `ShapeFilter` over `DeviceFacts.current()`
  for the problem `(rows, outDim, inDim)`: R1 prunes a spilling member, R2
  one whose threads × registers exceed the register file, R3 one whose
  shared bytes exceed the per-block limit, R4 one with no resident
  workgroup; R5 and R6 only record whether the grid fills the device (or
  the split-K that would) and the padding wasted. Every pruned member keeps
  its rule and the limit it exceeded; an empty survivor set is an
  `XpuLaunchException` naming them, never a silent fallback.
- *First use.* With two survivors, `ShapeChoice` times both through the
  family's probe, five interleaved rounds, fastest median wins, a lead under
  three percent recorded as a tie, and remembers the winner under
  `shape.q4kTcTile.2048x4096x4096` against a build id of every survivor's
  code hash. On this model the prewarm at load already settled every shape
  the batch can take, so the first real prefill runs the remembered member.
  Padding to the grain and the split-K count are decided per launch without
  timing. Two measurements gate the whole arm once per process:
  `matrixCoresPay` admits the matrix cores only if a matrix-core GEMM beats
  the dot-product kernel by more than ten percent on a synthetic 4096² Q4_K
  problem (on the CPU backend it does not), and `tileBeatsDeq` decides
  between the tile and the widen route at a 512-row gate shape.

### 3.2 Where prefill stands

On the 4090 the Q4_K and Q6_K prefill routes run at 1.1 to 1.2 of
llama.cpp's; the Q2_K, Q3_K and Q5_K routes at 0.42 to 0.69, because their
tiles are younger. The single-sequence prefill of the 30B mixture-of-experts
model swings 1.75× between runs a minute apart on Phoenix, the GPU clock,
which is why the benchmark reads prefill at four sequences and only after
the tune store has settled.

## 4. Decode

**What happens.** From the second token on, each sequence advances one
token per step, and a token's cost is almost entirely **reading every
weight byte once**: 4.58 GiB a step, against which the arithmetic is noise.
A decode kernel's only figure of merit is bytes per second against the
memory bus. Two regimes exist and the scheduler picks per step: a lone
greedy sequence runs the **pipelined** decode, and several sequences run
the **batched** decode, where one pass over a weight row serves them all.

### 4.1 One sequence: the pipelined decode

**What code runs.** `Scheduler.stepPipelined` (`Scheduler.cajeta:555`) keeps
one step in flight. On the first call it seeds a pinned device token slot
with the prompt's last argmax and calls `CausalLM.queueStep(seq, in, out)`,
which returns without waiting. On every later call it queues the *next*
step speculatively, then waits for the previous one's event and reads back
one integer. The host never knows a token before the step that consumes it
has been queued. `queueStep` (`CausalLM.cajeta:1074`):

1. `embedRowQ4kKernel` reads the token id from the device slot and writes the
   embedding into the resident hidden state; no host round trip.
2. `forwardRows(rows = 1)` takes the **resident** path (`decode-mode:
   resident`, said once): `residentLayer` per layer:
   - the input norm staged on the device (`rmsnormRowF32`);
   - the three attention projections in **one launch** where the load-time
     A/B said the fused form pays (`qkvWaveMatVecKernel`), else three wave
     mat-vecs: `q4kQ8WaveMatVecKernel` for q and k, and for v the Q4_K or
     the Q6_K kernel (`q6kQ8WaveMatVecKernel`) as the layer's tensor
     dictates; the prefetching twins (`q4kQ8WavePfMatVecKernel`,
     `q6kQ8WavePfMatVecKernel`) where the variant probe said they pay on
     this device;
   - `DeviceKv.prep`: norm, rotary and the K/V write fused in `qkPrepKernel`
     (else `ropeDecodeKernel` and `kvAppendKernel`), appending this
     position's key and value to the layer's planes;
   - attention split across the context, `attnFlashDecodeGqa4Kernel` writing
     partials and `attnReducePackQ8Kernel` reducing them and re-quantizing
     the result to q8_K in the same launch, so the output projection reads
     packed activations directly;
   - the output projection accumulated into the residual on the device;
   - the post-attention norm, gate and up as wave mat-vecs, `gluFusedPackF32`,
     the down projection (Q6_K or Q4_K) accumulated back.
3. The **fused tail**: the final norm, the Q6_K output head as one wave
   mat-vec over 128256 rows, and `argmaxRowKernel` writing the token into
   the pinned output slot; an `Event` is recorded on the stream and the
   stream flushed.

One wave owns one output row throughout the mat-vecs: consecutive lanes read
consecutive bytes of the row's 256-value blocks as whole words, dot them
against the q8_K activation bytes with the integer dot product, fold the
sub-block scales in, and a wave reduction finishes the row. The lane index
is `tid % Group.width()` at every one of the fifty-one sites, so the same
source runs at wave 32 and wave 64.

<figure class="diagram">
<svg viewBox="0 0 900 310" role="img" aria-label="The pipelined decode: the host queues step N+1 while step N runs; each step on the device embeds the token from a pinned slot, runs 32 resident layers of wave mat-vecs, K/V append and split attention, then the fused tail writes the argmax to the other slot and records an event; the host waits on the previous event and reads one integer." style="max-width:100%;height:auto;font-family:var(--font-ui);font-size:11px">
  <defs><marker id="da" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="currentColor"/></marker></defs>
  <g fill="currentColor" font-weight="600"><text x="10" y="22">host</text><text x="10" y="150">device stream (deferred on CUDA: replayed as a graph)</text></g>
  <line x1="10" y1="128" x2="890" y2="128" stroke="currentColor" stroke-dasharray="3 4" opacity="0.5"/>
  <g fill="none" stroke="currentColor" stroke-width="1.2">
    <rect x="60" y="40" width="150" height="34" rx="4"/>
    <rect x="250" y="40" width="150" height="34" rx="4"/>
    <rect x="440" y="40" width="150" height="34" rx="4"/>
    <rect x="630" y="40" width="150" height="34" rx="4"/>
    <rect x="60" y="170" width="110" height="34" rx="4"/>
    <rect x="200" y="170" width="300" height="34" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <rect x="530" y="170" width="130" height="34" rx="4"/>
    <rect x="690" y="170" width="100" height="34" rx="4"/>
    <rect x="60" y="250" width="110" height="34" rx="4"/>
    <rect x="200" y="250" width="300" height="34" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <rect x="530" y="250" width="130" height="34" rx="4"/>
    <rect x="690" y="250" width="100" height="34" rx="4"/>
    <rect x="820" y="160" width="60" height="30" rx="4" stroke-dasharray="4 3"/>
    <rect x="820" y="255" width="60" height="30" rx="4" stroke-dasharray="4 3"/>
  </g>
  <g fill="currentColor" text-anchor="middle">
    <text x="135" y="61">queue step N</text>
    <text x="325" y="61">queue step N+1</text>
    <text x="515" y="61">await event N</text>
    <text x="705" y="61">read 4 bytes, emit token</text>
    <text x="115" y="191">embed from slot 0</text>
    <text x="350" y="186">32 × residentLayer</text>
    <text x="350" y="199" opacity="0.7">wave mat-vecs · qkPrep · flash decode + reduce-pack · GLU</text>
    <text x="595" y="191">tail: norm, head, argmax</text>
    <text x="740" y="191">event N</text>
    <text x="115" y="271">embed from slot 1</text>
    <text x="350" y="266">32 × residentLayer</text>
    <text x="350" y="279" opacity="0.7">the same 1,360 launches, grids patched</text>
    <text x="595" y="271">tail: norm, head, argmax</text>
    <text x="740" y="271">event N+1</text>
    <text x="850" y="179">slot 1</text>
    <text x="850" y="274">slot 0</text>
  </g>
  <g stroke="currentColor" stroke-width="1.2" fill="none" marker-end="url(#da)">
    <line x1="210" y1="57" x2="248" y2="57"/>
    <line x1="400" y1="57" x2="438" y2="57"/>
    <line x1="590" y1="57" x2="628" y2="57"/>
    <line x1="170" y1="187" x2="198" y2="187"/>
    <line x1="500" y1="187" x2="528" y2="187"/>
    <line x1="660" y1="187" x2="688" y2="187"/>
    <line x1="170" y1="267" x2="198" y2="267"/>
    <line x1="500" y1="267" x2="528" y2="267"/>
    <line x1="660" y1="267" x2="688" y2="267"/>
    <path d="M660,180 L818,175"/>
    <path d="M660,262 L818,270"/>
    <path d="M135,74 L135,168"/>
    <path d="M325,74 L325,230 L115,230 L115,248"/>
    <path d="M740,170 L740,110 L515,110 L515,76"/>
    <path d="M850,190 L850,225 L115,225 L115,248" stroke-dasharray="4 3"/>
  </g>
  <g fill="currentColor" font-size="10" opacity="0.7">
    <text x="140" y="120">launches ~1 ms of host time</text>
    <text x="520" y="105">the only host wait</text>
    <text x="700" y="160">argmax →</text>
    <text x="700" y="245">argmax →</text>
    <text x="330" y="222">next step reads the slot the previous argmax wrote</text>
  </g>
</svg>
<figcaption>The pipelined single-sequence decode. Two pinned token slots alternate between steps; the host's only wait is on the previous step's event, and on CUDA the step's launch sequence is replayed as a captured graph after its third sighting.</figcaption>
</figure>

**NVIDIA against AMD.** On CUDA the decode stream is **deferred**
(`KernelStream.setDeferred`): the first sight of the step's launch sequence
runs direct, the third is recorded as a graph, and later steps replay it
with the changed grids and arguments patched into the nodes, which is how a
step of about 1,360 launches is issued in about a millisecond of host time.
Deferral is a no-op on HIP and Vulkan, so there the host pays for every
launch, and the pipelining (the host's share of a token overlapping the
device's) matters more, not less. The fused tail, the device argmax and the
device embedding are what make the pipeline possible; where any is missing
the step runs plainly with a sync. The wave mat-vecs are the same source on
both parts at wave 32; on Vulkan a declared 32-lane kernel runs as a virtual
wave over the device's subgroups. The K/V planes are f16 on every backend.

**How it optimizes.** Decode's choices were all made before the first token:
the wave kernel per weight at bind (`Linear` latches the format's wave route
when the kernel is usable, the activations are q8_K packed and the width a
multiple of 256, else the f32-activation wave, else the scalar kernel, else
the host); the prefetching variant per weight shape from `mvVariant-…`; the
fused q|k|v from the load-time A/B. Nothing is timed during decode. The
measured standing on the 4090 is at or above llama.cpp's single-sequence
rate at every quantization from Q2_K to Q8_0: the kernels were at the bus,
and the gaps between them were the host, which the pipeline removed.

### 4.2 Four sequences: the batched decode

**What code runs.** `decodeBatchReady` (`CausalLM.cajeta:3800`) is latched
once per model: the K/V cache device resident, the attention kernels usable,
head dimension at most 128, hidden and query widths multiples of 256, every
projection and the output head answering `colsReady` (no epilogue, packed
activations, a Cols kernel for its format in the registry). A refusal is
recorded as `decode-batch refused: <why>` and the sequences decode singly.
When it holds, `Scheduler.stepOnce` collects every decoding row into one
batched step (`:4000` onward):

- every sequence's token embedded on the device (`embedRowsLaunch`), the
  hidden rows normed and packed (`q8kPackRowsLaunch`);
- each projection as **one Cols launch** over the batch's activation
  columns: `q4kQ8WaveMatVecColsKernel` (or the prefetching and two-row
  variants `q4kQ8WavePfMatVecColsKernel`, `q4kQ8WaveR2MatVecColsKernel`
  where the Cols probe said they pay at width 4), `q6kQ8WaveMatVecColsKernel`
  for the Q6_K tensors; the fused `q4kQ8WaveMatVecColsSegPlainKernel` and
  `…ColsSegPfKernel` serving q|k|v and gate|up in one launch where the
  load-time A/B said so. One pass over a weight row serves four sequences,
  which is why four cost close to one in weight traffic;
- `qkPrepBatchKernel`, then one `attnFlashDecodeGqa4BatchKernel` launch
  attending every sequence through the whole `KvPool` buffer (slot `s` at
  element `s × plane`), `attnReducePackQ8BatchKernel` reducing and packing;
- the tail per row and `argmaxRowKernel` per row; a `decode-batch-phase`
  record splits the step into host embedding, upload, issue and device tail.


<figure class="diagram">
<svg viewBox="0 0 900 250" role="img" aria-label="The scheduler's steps for four requests: all four admitted at step 1 and prefilled together in chunks over steps 1 to 3; from step 4 all four decode in one batched step per token; a fifth request arriving at step 6 is admitted when blocks free up at step 12 and prefills in chunks sized to the 150 ms budget while the others keep decoding." style="max-width:100%;height:auto;font-family:var(--font-ui);font-size:11px">
  <g fill="currentColor">
    <text x="10" y="40">request 1</text><text x="10" y="70">request 2</text><text x="10" y="100">request 3</text><text x="10" y="130">request 4</text><text x="10" y="175">request 5</text>
    <text x="10" y="225" font-weight="600">step</text>
  </g>
  <g fill="none" stroke="currentColor" stroke-width="1" opacity="0.4">
    <line x1="90" y1="210" x2="890" y2="210"/>
  </g>
  <g fill="currentColor" font-size="10" text-anchor="middle">
    <text x="110" y="225">1</text><text x="150" y="225">2</text><text x="190" y="225">3</text><text x="230" y="225">4</text><text x="270" y="225">5</text><text x="310" y="225">6</text><text x="350" y="225">7</text><text x="390" y="225">8</text><text x="430" y="225">9</text><text x="470" y="225">10</text><text x="510" y="225">11</text><text x="550" y="225">12</text><text x="590" y="225">13</text><text x="630" y="225">14</text><text x="670" y="225">15</text><text x="710" y="225">16</text><text x="750" y="225">17</text><text x="790" y="225">18</text><text x="830" y="225">19</text><text x="870" y="225">20</text>
  </g>
  <g fill="var(--accent)" opacity="0.85">
    <rect x="92" y="28" width="116" height="14" rx="2"/><rect x="92" y="58" width="116" height="14" rx="2"/><rect x="92" y="88" width="116" height="14" rx="2"/><rect x="92" y="118" width="116" height="14" rx="2"/>
    <rect x="532" y="163" width="236" height="14" rx="2"/>
  </g>
  <g fill="currentColor" opacity="0.45">
    <rect x="212" y="28" width="676" height="14" rx="2"/><rect x="212" y="58" width="676" height="14" rx="2"/><rect x="212" y="88" width="676" height="14" rx="2"/><rect x="212" y="118" width="676" height="14" rx="2"/>
    <rect x="772" y="163" width="116" height="14" rx="2"/>
  </g>
  <g fill="none" stroke="currentColor" stroke-width="1" stroke-dasharray="3 3" opacity="0.6">
    <rect x="292" y="163" width="236" height="14" rx="2"/>
  </g>
  <g fill="currentColor" font-size="10">
    <text x="96" y="22">one batched prefill, 2048 rows, chunked</text>
    <text x="216" y="52" opacity="0.8">one batched decode step per token: Cols launches, one attention launch</text>
    <text x="296" y="158" opacity="0.8">waiting: free blocks must cover its whole prompt plus decode headroom</text>
    <text x="536" y="193" opacity="0.8">prefill in chunks under the 150 ms budget, beside the decoders</text>
    <text x="776" y="193" opacity="0.8">joins the batch</text>
  </g>
</svg>
<figcaption>The scheduler over twenty steps for the measured run plus one late arrival. Colored bars are prefill, gray bars decode. The late request waits for blocks rather than preempting older work, and its prefill is chunked so the four decoders never pause longer than the budget.</figcaption>
</figure>

Since 2026-10-24 the batched greedy decode also runs one step ahead on HIP,
the single-sequence pipeline's design applied to the batch, rows embedded on
the device from the previous step's argmaxes with one event per step.

**NVIDIA against AMD.** The Cols kernels exist for exactly the six formats
the batched decode admits (Q4_K, Q6_K, Q5_K, Q8_0, Q2_K, Q3_K); this model
uses two of them. On the 4090 the four-sequence decode sits at 0.95 of
llama.cpp for Q4_K_M, and between 0.89 and 0.97 across the six formats; on
Proton's Vulkan path the decode at four sequences sits at 0.98 of llama.cpp's
Vulkan. The batched pipeline is HIP's answer to the graph replay it cannot
have.

**How it optimizes.** Per format and batch width, the Cols probe at load
timed the Cols kernel against single-row launches after checking each
candidate against the host mat-vec (`colsPays-q4k-b4`); the fused segments
were A/B'd in place on the real batched step; the per-launch choice between
one Cols launch and several single-row launches reads those answers without
timing.

### 4.3 Longer contexts

At 8k and 32k tokens the attention's share grows: the decode kernel splits
the context across workgroups and the reduce kernel's partial count grows
with it; the K/V pool's planes grow to the slots a batch needs, each under
2 GiB, and growth is counted because on WSL2 a 2 GiB mapping costs tens of
milliseconds to create and destroy, so the engine moves it off the clock
rather than making it cheap. The benchmark's `llm/decode-8k` and
`llm/decode-32k` rows measure exactly this.

## 5. What the optimizer did, kernel by kernel

For every kernel this page named, where its configuration came from.

| kernel | compile time (the adaptor) | bind (the filter) | first use or load (measured) | per launch |
|---|---|---|---|---|
| `QkTile_128_128_32_64_128_q4kQ8`, `QkTile_64_64_32_32_128_q4kQ8` | one template, two `@Instantiate` members, `@Requires` on the shape, `@Occupancy(maxThreads = 2 × TN)`; manifest: registers, no spill, shared bytes, `wmma` native | `ShapeFilter` R1 to R6 at `(rows, out, in)`; the 128 member pruned on gfx1151 by R3 | `ShapeChoice`, five interleaved rounds, remembered as `shape.q4kTcTile.MxNxK` with margin, against the build id; prewarmed at load | padding to the grain, split-K when the grid does not fill the device |
| `Q6Tile_*` | the same, over the split Q6_K layout | the same | `shape.q6kTcTile.MxNxK` | the same |
| `q4kWmmaDeqMw8Kernel`, `q6kWmmaDeqMw8Kernel` | `@Occupancy(maxWaves)` in waves so the block follows the wave width | the registry | `tileBeatsDeq` once per process decides tile against widen; the widen copies made under `auto` where they pay | |
| `q4kQ8WaveMatVecKernel`, `q6kQ8WaveMatVecKernel` and the `Pf` twins | lane index `tid % Group.width()`; `@Wave(32)` only where the body needs it | `KernelCap.usable`, the width a multiple of 256 | `probeArms` at load: prefetching, plain or two-row, remembered as `mvVariant-<type>-…` | never chunked across launches |
| `qkvWaveMatVecKernel` | | | the fused-Cols A/B at load, in place on the real step | |
| `*ColsKernel`, `*ColsSegPlain`, `*ColsSegPf` | | `colsReady` per projection latched once | the Cols probe per format at width 4 (`colsPays-…`), each candidate first checked against the host mat-vec | one Cols launch or single-row launches, from the probe |
| `attnFlashDecodeGqa4Kernel`, `attnReducePackQ8Kernel`, the `Batch` twins | specialized at ratio 4 and head dimension 128; a batched twin generated from the per-row kernel and held byte-equal by a test | `KernelCap.usable` | | the split count follows the context |
| `attnFlashPrefillGqa4Kernel`, `attnFlashPrefillTileKernel` | the tile form on the f16 matrix cores | | | |
| `qkPrepKernel`, `qkPrepBatchKernel`, `gluFusedPackF32`, `rmsnormPackRowF32`, `attnReducePackQ8Kernel` | fusions chosen by design: each saves a launch and a pass over activations | | | |
| `embedRowQ4kKernel`, `argmaxRowKernel` | | `pipelineReady`: the device embedding and argmax are what make the pipeline legal | | |
| every one of them | the build gates: a spill or an unlowered backend fails the build by name | the registry answers absent, never falls through | a value is kept only against the build that measured it; a measurement under foreign load is used and not stored | the launch is checked after every submission: seven named refusals |

The CPU backend runs the same catalog as a target, not an emulator: the
work-item loop vectorizes at the host's SIMD width, the wave verbs become
vector reductions and shuffles, and because the backend reports no register
or spill count, the filter prunes nothing there and what differs is which
members are fast; `matrixCoresPay` answers no, and the dot-product floor
serves the prefill.

## 6. What the profiler shows

`CAJETA_PROFILER=1` arms the host sampler and the device capture before any
backend initializes and writes a `.pftrace` (into `.cajeta/profiles/`, one
file per run) that the IntelliJ tool window or Perfetto opens. Read against
this page it shows:

- the **load** as a `load-phase` breakdown: bytes moved, the relayout and
  widen kernels, the prewarm probes' bracketed launches (recognizable as
  twenty-launch bursts of one kernel, five rounds, interleaved), the
  throwaway prefill and decode step;
- each **prefill** chunk as the per-layer sequence of §3, the tiles the
  longest spans, each device span carrying its tier (DEVICE from CUPTI on
  CUDA, from rocprofiler on HIP; EVENT where only the driver is installed;
  HOST on Vulkan outside the timestamp seam), with `batch-route` and
  `tile-choice` records beside the launches that took them;
- each **decode** step as a dense run of about 1,360 launches of a few
  microseconds each (an armed profiler keeps every launch direct, so the
  graph replay is off while tracing), the two events per step in the
  pipeline, and the host's wait on the previous event as the only host gap;
  `sched-phase` records give the step's admission, K/V reserve, model and
  total nanoseconds, and `decode-batch-phase` the batched step's host
  embedding, upload, issue and device tail.

The trace this section will link is the first quiet-box run's, the same
execution the [benchmarks](../../bench/llm/README.md) read, so that the
numbers and the picture are one measurement. Until that run exists this
section describes the trace from the instrumentation's own records rather
than from a captured file.

## Where to go from here

- The kernel families and how one is chosen, in full:
  [Language models on the device](kernels-llm.md).
- The engine around them: [Inside the engine](llm/inside.md), the
  [API](llm/engine.md), [configuration](llm/configuration.md).
- The substrate: [One kernel, any device](README.md), the
  [scheduler](scheduler.md), [optimization](optimization.md), and the
  contract, [cajeta.xpu](../specification/xpu/CajetaXPU.md).
- How the numbers are read: [Measuring](llm/measuring.md) and the
  [benchmark pages](../../bench/llm/README.md).
