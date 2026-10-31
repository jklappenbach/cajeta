# One model, start to last token

How `dev.cajeta.llm` turns a prompt into tokens, as an algorithm: the steps
in order, the kernels each step launches, the decision each step makes and
the signal it reads, and what changes when the model is a different
architecture or the device a different backend. Each phase below opens with
an outline and a diagram; the prose under them says why the step is shaped
that way. The pieces have their own pages (the
[kernels](kernels-llm.md), the [engine](llm/README.md), the
[substrate](README.md)); this page is the thread through them.

The reference run is **Meta-Llama-3.1-8B-Instruct at Q4_K_M**: a 512-token
prompt, 128 generated tokens, four sequences at once, the shape every number
on the [benchmarks](../../bench/llm/README.md) is read at. The
mixture-of-experts sections follow **Qwen3-Coder-30B-A3B at Q5_K_M**, the
other measured model. Code references are to `dev.cajeta.llm` at `59ef166`
(2026-10-29) on cajeta 0.48.0, with the file and line in parentheses; the
machines are Phoenix (RTX 4090, `sm_89`, CUDA under WSL2) and Proton
(Radeon 8060S, `gfx1151`, ROCm, and the same part under Vulkan).

Two facts about the workload shape everything. **Decode is a bandwidth
problem**: one token reads every weight byte once (4.58 GiB a step for this
model) and does almost nothing with it, so a decode kernel's figure of merit
is bytes per second against the memory bus, and the engine's job is to leave
no gap between kernels. **Prefill is a compute problem**: a prompt multiplies
every weight by hundreds of rows, so the engine's job is to feed the matrix
units and to pick, per weight tensor, the GEMM that feeds them best here.

## 0. The whole path

1. A caller submits a prompt (`LlmEngine.submitText`): tokens, a sampler, a
   token sink. The request waits.
2. Each **scheduler step** admits what fits, grows the K/V pool, and gives
   every running request one chunk of work: a prefilling request a chunk of
   prompt tokens, a decoding request exactly one token. Decode is the
   one-token chunk of the same model step (`stepSeq`), which is the identity
   the scheduler is built on (`Scheduler.cajeta:21-43`).
3. A **prefill chunk** embeds its tokens on the device, runs the layer stack
   once over all its rows, writes K/V for every position, and, on the last
   chunk, runs the tail for the last row: the first generated token.
4. From then on each **decode step** runs the stack for one new row per
   sequence, alone through the pipelined path or several sequences through
   the batched path, and the tail's argmax is the next token.
5. A token goes to the sink, the stop rules run, and a finished request
   frees its slot and K/V blocks, which admits the next waiting request in
   the same step.

<figure class="diagram">
<svg viewBox="0 0 1000 340" role="img" aria-label="The whole path: a request enters the scheduler; each step admits, reserves K/V and hands every running request one chunk; prefill chunks run embedding, the layer stack and, on the last chunk, the tail; decode steps run one row per sequence through the pipelined or the batched path; tokens go to the sink and the stop rules; a finished request frees its slot, which admits the next waiting request." style="max-width:100%;height:auto;font-family:var(--font-ui);font-size:11px">
  <defs><marker id="fa" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="currentColor"/></marker></defs>
  <g fill="none" stroke="currentColor" stroke-width="1.2">
    <rect x="10" y="20" width="130" height="44" rx="4"/>
    <rect x="180" y="10" width="210" height="64" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <rect x="430" y="10" width="260" height="64" rx="4"/>
    <rect x="730" y="10" width="260" height="64" rx="4"/>
    <rect x="430" y="110" width="560" height="100" rx="4" stroke-dasharray="4 3"/>
    <rect x="445" y="140" width="260" height="56" rx="4"/>
    <rect x="725" y="140" width="250" height="56" rx="4"/>
    <rect x="180" y="250" width="300" height="56" rx="4"/>
    <rect x="520" y="250" width="260" height="56" rx="4"/>
  </g>
  <g fill="currentColor">
    <text x="20" y="38" font-weight="600">request</text>
    <text x="20" y="54" opacity="0.75">tokens, sampler, sink</text>
    <text x="190" y="30" font-weight="600" fill="var(--accent)">scheduler step</text>
    <text x="190" y="46" opacity="0.75">admit, reserve K/V, one chunk per</text>
    <text x="190" y="60" opacity="0.75">running request, admit again</text>
    <text x="440" y="30" font-weight="600">prefill chunk (n rows)</text>
    <text x="440" y="46" opacity="0.75">embed on the device, 32 layers over n rows,</text>
    <text x="440" y="60" opacity="0.75">K/V written; the last chunk runs the tail</text>
    <text x="740" y="30" font-weight="600">decode step (one row a sequence)</text>
    <text x="740" y="46" opacity="0.75">one sequence: pipelined, a step ahead</text>
    <text x="740" y="60" opacity="0.75">several: batched, one pass over the weights</text>
    <text x="440" y="128" opacity="0.75">the model step, stepSeq(slot, tokens, offset, n): decode is n = 1</text>
    <text x="455" y="160" font-weight="600">layer × 32</text>
    <text x="455" y="176" opacity="0.75">norm+pack, q|k|v, rope + K/V append,</text>
    <text x="455" y="190" opacity="0.75">attention, o-proj, norm+pack, FFN or experts</text>
    <text x="735" y="160" font-weight="600">tail</text>
    <text x="735" y="176" opacity="0.75">final norm, output head, argmax on the</text>
    <text x="735" y="190" opacity="0.75">device; one integer back per sequence</text>
    <text x="190" y="270" font-weight="600">token out</text>
    <text x="190" y="286" opacity="0.75">sink callback; stop token, stop strings, budget;</text>
    <text x="190" y="300" opacity="0.75">greedy: the argmax; sampled: the host sampler</text>
    <text x="530" y="270" font-weight="600">finish</text>
    <text x="530" y="286" opacity="0.75">release the slot and the K/V blocks, save</text>
    <text x="530" y="300" opacity="0.75">full blocks to the store, admit the next</text>
  </g>
  <g stroke="currentColor" stroke-width="1.2" fill="none" marker-end="url(#fa)">
    <line x1="140" y1="42" x2="178" y2="42"/>
    <line x1="390" y1="42" x2="428" y2="42"/>
    <line x1="690" y1="42" x2="728" y2="42"/>
    <line x1="560" y1="74" x2="560" y2="108"/>
    <line x1="860" y1="74" x2="860" y2="108"/>
    <line x1="705" y1="168" x2="723" y2="168"/>
    <path d="M850,196 L850,228 L330,228 L330,248"/>
    <line x1="480" y1="278" x2="518" y2="278"/>
    <path d="M650,306 L650,324 L75,324 L75,66"/>
    <path d="M330,250 L330,236 L960,236 L960,76" stroke-dasharray="3 3"/>
  </g>
  <g fill="currentColor" font-size="10" opacity="0.7">
    <text x="85" y="320">a freed slot admits the next waiting request</text>
    <text x="700" y="246">the next token feeds the next decode step</text>
  </g>
</svg>
<figcaption>The path and its loops. The outer loop is the scheduler step; inside it every running request advances by one chunk through the same model step; inside that the layer stack runs once over the chunk's rows and the tail reads one integer per sequence back.</figcaption>
</figure>

## 1. Load: making every later decision before the first request

1. **Choose the device** (`LlmEngine.cajeta:108-109`). An option forces a
   backend; otherwise the first device touch picks one in the fixed order
   CUDA, HIP, Vulkan, CPU. The choice is per process. Nothing in the engine
   reads the vendor's name after this except one guard (§5.3).
2. **Resolve the tune directory** (`:76-101`) and hand it to every kernel
   store before any kernel is bound (`:114-120`): the option, else
   `CAJETA_LLM_TUNE_DIR`, else the platform cache directory. No probe runs
   without a place to remember its answer.
3. **Open the checkpoint and read the configuration** (`:175-193`): a GGUF's
   `general.architecture` and its arch-prefixed keys fill `ModelConfig`
   (`ModelConfig.cajeta:167-199`), with the tokenizer and the end-of-generation
   set from the same file; a safetensors load takes a `config.json`.
4. **Build and bind the model by tensor name** (`:197-199`): 32
   `DecoderLayer`s, each an `Attention` with four `Linear` projections and an
   `Mlp` (or `MoeFfn`) with three, plus the embedding, the final norm and the
   output head. Hugging Face names are translated to ggml names at the load
   seam (`Checkpoint.cajeta:34-38, 74-135`); a tensor the layout does not
   expect is an error, not a guess.
5. **Make the model resident.** Each `Linear` keeps its weight packed when
   the format has device kernels (`Linear.cajeta:891-914`), uploads it, and
   performs the relayouts those kernels want: a Q6_K weight split into a scale
   prefix and 208-byte bodies for aligned loads, a Q4_K slab viewed as 32-bit
   words. Under the `auto` prefill-weight policy, the formats whose int8
   widen route still pays get a resident int8 copy inside the widen budget
   (`WidenBudget.cajeta:9-43`). Decode always reads the packed bytes.
6. **Allocate the K/V cache** (§2.5): the paged host cache in 16-token
   blocks, and on the device one f16 K plane and one V plane per layer in a
   pool that every sequence's slot shares.
7. **Run every probe that needs no prompt** (`:208-263`): the tile member per
   prefill shape the engine will meet (`prewarmBatchShapes(maxSeqs × chunk)`),
   the Cols verdicts per format and batch width, the mat-vec variant per
   weight shape, then one throwaway chunk-sized prefill and one decode step
   (which settle `matrixCoresPay`, `tileBeatsDeq` and the attention tile
   probe), then the fused q|k|v and gate|up A/B on the real batched step.
   Mixture-of-experts tile trials are held during the warm-up so the first
   real prefill runs an untimed choice.
8. **Build the scheduler** (`:275`) with the 150 ms chunk budget; attach the
   tokenizer and, if configured, the block store.

Load is where the tune store fills. After it, the directory holds for this
device key the tile member per shape, whether the Cols kernel pays per
format at the batch width, the fused-segment verdicts and the mat-vec
variant per weight shape, each written against the build id of the kernels
it measured; a second load on the same machine reads them back and runs no
probe. §4 says when each decision is made and what it reads.

## 2. Prefill

### 2.1 The algorithm

1. **Admit** (`Scheduler.cajeta:1010-1077`). A request is admitted when a
   slot is free and the free K/V blocks cover its whole uncached suffix plus
   one block per unwindowed layer of decode headroom. Resumed requests go
   first, in order of preemption; then waiting requests, first in first out.
   A prefix the block store already holds is adopted block by block, always
   leaving at least one token to compute so the newest logits are real
   (`:1091-1150`).
2. **Choose the chunk** (`ChunkPolicy.cajeta:68-104`). Sizes are 32 to 512.
   With nothing decoding, the policy learns the best measured rows per
   nanosecond and probes one size up; with a decode loaded, it takes the
   largest size predicted to fit the 150 ms pause budget shared across the
   prompts, so sequences already generating never stall for a long prompt.
3. **Batch across requests** (`:487-497, 730-776`). Every running greedy
   request with a chunk pending is collected into one pass over all their
   rows (2048 rows for the measured run). Only greedy requests batch: the
   pass keeps one set of logits. A sampled request prefills alone.
4. **Carve one arena** for the chunk (`CausalLM.cajeta:707-720`): the hidden
   state and every intermediate as borrowed slices of one buffer sized to
   the rows. Nothing survives the call, so peak activation memory is one
   layer's at any depth.
5. **Embed on the device** (`:731-752`, `EmbedKernel.cajeta:11-33`). Only the
   int32 token ids go up; one workgroup per token gathers and dequantizes
   its row of the packed table into the hidden state. The host decode of
   every row plus the upload it replaced cost 45 ms of a 118 ms prefill.
6. **Run the layer stack** over the rows (§2.2), the hidden state resident on
   the device for the whole stack and crossing the bus twice
   (`:2722-2727`).
7. **Run the tail** for the last row of each sequence (`:3238-3296`): slice
   the row, final norm, stage, the output head as one mat-vec,
   `argmaxRowKernel`, one stream synchronization, one integer down. The
   logits themselves are fetched only when a sampler needs them (`:641-648`).

The whole chunk, about 678 launches for this model, is queued by the host in
under a millisecond and submitted at the tail's synchronization as one
recording on CUDA (§3.4); a mid-prefill flush exists and is off because it
measured as not paying (`:3191-3203`).

### 2.2 One layer, many rows

<figure class="diagram">
<svg viewBox="0 0 920 330" role="img" aria-label="One transformer layer in prefill over n rows: norm fused with the q8_K pack; the query, key and value projections on tensor-core tiles; rotary embedding fused with the K/V append into the layer's f16 planes; the flash prefill tile attention; the output projection accumulated into the residual in the tile's epilogue; norm and pack again; gate and up as one fused tile pair; the GLU fused with the next pack; the down tile accumulated into the residual; or, in a mixture-of-experts layer, the routed feed-forward." style="max-width:100%;height:auto;font-family:var(--font-ui);font-size:11px">
  <defs><marker id="la" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="currentColor"/></marker></defs>
  <g fill="none" stroke="currentColor" stroke-width="1.2">
    <rect x="10" y="56" width="136" height="48" rx="4"/>
    <rect x="170" y="10" width="110" height="36" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <rect x="170" y="62" width="110" height="36" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <rect x="170" y="114" width="110" height="36" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <rect x="320" y="56" width="150" height="48" rx="4"/>
    <rect x="510" y="56" width="170" height="48" rx="4"/>
    <rect x="720" y="56" width="190" height="48" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <rect x="510" y="150" width="170" height="40" rx="4" stroke-dasharray="4 3"/>
    <rect x="10" y="240" width="136" height="48" rx="4"/>
    <rect x="170" y="240" width="170" height="48" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <rect x="376" y="240" width="130" height="48" rx="4"/>
    <rect x="542" y="240" width="110" height="48" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <rect x="680" y="228" width="236" height="72" rx="4" stroke-dasharray="4 3"/>
  </g>
  <g fill="currentColor">
    <text x="20" y="74" font-weight="600">norm + pack</text>
    <text x="20" y="90" opacity="0.75">rmsnormPackRowF32</text>
    <text x="180" y="33" font-weight="600">q · W<tspan font-size="9" baseline-shift="super">T</tspan><tspan opacity="0.75"> tile</tspan></text>
    <text x="180" y="85" font-weight="600">k · W<tspan font-size="9" baseline-shift="super">T</tspan><tspan opacity="0.75"> tile</tspan></text>
    <text x="180" y="137" font-weight="600">v · W<tspan font-size="9" baseline-shift="super">T</tspan><tspan opacity="0.75"> tile</tspan></text>
    <text x="330" y="74" font-weight="600">rope + K/V append</text>
    <text x="330" y="90" opacity="0.75">ropeAppendRowsKernel</text>
    <text x="520" y="74" font-weight="600">flash attention</text>
    <text x="520" y="90" opacity="0.75">attnFlashPrefillF16Kernel</text>
    <text x="730" y="74" font-weight="600">o · W<tspan font-size="9" baseline-shift="super">T</tspan><tspan opacity="0.75"> tile, += residual</tspan></text>
    <text x="730" y="90" opacity="0.75">the add in the tile's epilogue</text>
    <text x="520" y="168" font-weight="600">layer K/V planes</text>
    <text x="520" y="182" opacity="0.75">f16, slot s at s × plane</text>
    <text x="20" y="258" font-weight="600">norm + pack</text>
    <text x="20" y="274" opacity="0.75">rmsnormPackRowF32</text>
    <text x="180" y="258" font-weight="600">gate|up fused tile pair</text>
    <text x="180" y="274" opacity="0.75">one taller tensor, one launch</text>
    <text x="386" y="258" font-weight="600">GLU + pack</text>
    <text x="386" y="274" opacity="0.75">gluFusedPackF32</text>
    <text x="552" y="258" font-weight="600">down tile</text>
    <text x="552" y="274" opacity="0.75">+= residual</text>
    <text x="690" y="246" font-weight="600">or a mixture-of-experts layer</text>
    <text x="690" y="262" opacity="0.75" font-size="10">router, top-k, grouping sort, gather, pack,</text>
    <text x="690" y="276" opacity="0.75" font-size="10">id GEMMs gate and up; GLU, pack; id GEMM down;</text>
    <text x="690" y="290" opacity="0.75" font-size="10">scatter-combine; the shared expert (§2.4)</text>
  </g>
  <g stroke="currentColor" stroke-width="1.2" fill="none" marker-end="url(#la)">
    <line x1="146" y1="80" x2="168" y2="28"/>
    <line x1="146" y1="80" x2="168" y2="80"/>
    <line x1="146" y1="80" x2="168" y2="132"/>
    <line x1="280" y1="28" x2="318" y2="70"/>
    <line x1="280" y1="80" x2="318" y2="80"/>
    <path d="M280,132 L300,132 L300,92 L318,92"/>
    <line x1="470" y1="80" x2="508" y2="80"/>
    <line x1="680" y1="80" x2="718" y2="80"/>
    <line x1="470" y1="96" x2="508" y2="150"/>
    <path d="M815,104 L815,205 L72,205 L72,238"/>
    <line x1="146" y1="264" x2="168" y2="264"/>
    <line x1="340" y1="264" x2="374" y2="264"/>
    <line x1="506" y1="264" x2="540" y2="264"/>
    <path d="M652,264 L666,264 L666,314 L72,314 L72,290" stroke-dasharray="3 3"/>
  </g>
  <g fill="currentColor" font-size="10" opacity="0.7">
    <text x="240" y="200">residual stream, resident on the device for the whole stack</text>
    <text x="240" y="326">to the next layer's norm + pack</text>
  </g>
</svg>
<figcaption>One layer of the prefill as the device runs it. The colored boxes are the GEMMs, each on the route §2.3 chooses for that tensor; everything between them is a fusion chosen by design: norm with pack, rotary with the K/V write, GLU with the next pack, the residual add in a tile's epilogue. The dashed box is the alternative body of a mixture-of-experts layer; a family's extra blocks (QK-norm before the rotary embedding, sub-norms) sit between these without changing the routes (§5.1).</figcaption>
</figure>

Per layer, in order, for `rows` rows (`CausalLM.cajeta:2716-3203`):

1. **Norm and pack in one launch** (`rmsnormPackRowF32`, one workgroup per
   row) when the hidden width is a multiple of 256, else norm then pack
   (`:1718-1737`). Activations become 256-value q8_K blocks (one scale,
   eight sub-block sums) so the integer dot product does the arithmetic on
   every route below.
2. **Query, key and value projections**, each a `[rows × hidden] · Wᵀ` GEMM on
   the route §2.3 picks for that tensor; when all three are Q4_K they can
   run as one fused widened launch split three ways (`Linear.cajeta:3852-3893`).
3. **QK-norm** per head, where the family has it (Qwen3), before the rotary
   embedding (`:2847-2862`).
4. **Rotary embedding folded into the K/V append** (`ropeAppendRowsKernel`,
   `:2863-2877`): keys and values go into the layer's f16 planes at the
   sequence's slot as they are rotated; no separate pass over K.
5. **Flash attention over the sequence so far** (§2.3, attention): one
   workgroup per 64 (query, head) pairs on one K/V head, keys and values
   staged through shared memory in 64-key blocks, the softmax kept running
   and the output rescaled in place (`AttnKernel.cajeta:2447-2466`).
6. **Output projection accumulated into the residual** in the tile's
   epilogue when the rows are a multiple of 128 and the tensor has no bias
   or scale epilogue (`Linear.cajeta:3799-3819`); else the GEMM and a
   separate add.
7. **Norm and pack** again.
8. **Gate and up as one fused tile pair** for any two tensors of the same
   format (the concatenated payload read as one taller tensor,
   `:3572-3587, 3771-3789`), else the Q4_K-only fused widened launch, else
   two GEMMs.
9. **GLU fused with the next pack** (`gluFusedPackF32` writes the q8_K stage
   directly, `CausalLM.cajeta:3133-3137`).
10. **Down projection accumulated into the residual** (`:3178-3186`). A
    mixture-of-experts layer replaces steps 8 to 10 with §2.4.

### 2.3 How a projection chooses its GEMM

`Linear.batchRefusal(rows)` (`Linear.cajeta:2273-2366`) runs first and
answers with the gate that refused, or null: the weight quantized, routing
on, a batch GEMM kernel for the format on this backend, the shape divisible
as the route needs. An admitted projection then tries the arms of
`matmulBatchKeep` in order (`:2753-3290`), each gated by a capability the
device answered, a measurement, or a fit; the arm taken is announced once per
route as a `batch-route` record.

<figure class="diagram">
<svg viewBox="0 0 940 400" role="img" aria-label="The route a projection takes by row count and signals: one row goes to the integer wave mat-vec (else the f16 or f32 wave, else the host); two to eight rows go to the Cols kernel when the Cols probe says it pays, else single-row launches; a chunk goes, on Vulkan, to the f16 cooperative GEMM or the dp4a mmq tile; elsewhere to the tensor-core tile when the matrix cores pay, the tile is preferred over the widen route and a member fits; else the widened int8 WMMA GEMM when the widen budget admitted the copy; else the packed WMMA GEMM for Q4_K and Q6_K; else the f16 cooperative GEMM; else the dot-product floor." style="max-width:100%;height:auto;font-family:var(--font-ui);font-size:11px">
  <defs><marker id="ra" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="currentColor"/></marker></defs>
  <g fill="none" stroke="currentColor" stroke-width="1.2">
    <rect x="10" y="170" width="140" height="44" rx="4"/>
    <rect x="190" y="20" width="200" height="44" rx="4"/>
    <rect x="190" y="100" width="200" height="44" rx="4"/>
    <rect x="190" y="250" width="200" height="44" rx="4"/>
    <rect x="430" y="20" width="250" height="44" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <rect x="430" y="100" width="250" height="44" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <rect x="430" y="180" width="250" height="44" rx="4"/>
    <rect x="430" y="250" width="250" height="44" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <rect x="430" y="330" width="250" height="44" rx="4"/>
    <rect x="720" y="100" width="210" height="44" rx="4"/>
    <rect x="720" y="180" width="210" height="44" rx="4"/>
    <rect x="720" y="250" width="210" height="44" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <rect x="720" y="330" width="210" height="44" rx="4"/>
  </g>
  <g fill="currentColor">
    <text x="20" y="188" font-weight="600">a projection</text>
    <text x="20" y="204" opacity="0.75">rows, format, backend</text>
    <text x="200" y="38" font-weight="600">one row (decode)</text>
    <text x="200" y="54" opacity="0.75">latched at bind</text>
    <text x="200" y="118" font-weight="600">2 to 8 rows (batched decode)</text>
    <text x="200" y="134" opacity="0.75">decided per launch</text>
    <text x="200" y="268" font-weight="600">a chunk (prefill)</text>
    <text x="200" y="284" opacity="0.75">arms tried in order</text>
    <text x="440" y="38" font-weight="600">integer wave mat-vec</text>
    <text x="440" y="54" opacity="0.75">kernel here, q8_K pack, inDim % 256 == 0</text>
    <text x="440" y="118" font-weight="600">Cols kernel</text>
    <text x="440" y="134" opacity="0.75">ColsProbe.pays(format, width)</text>
    <text x="440" y="198" font-weight="600">Vulkan: f16 cooperative GEMM</text>
    <text x="440" y="214" opacity="0.75">else the dp4a mmq tile (Q4_K, Q6_K)</text>
    <text x="440" y="268" font-weight="600">tensor-core tile</text>
    <text x="440" y="284" opacity="0.75">cores pay, tile preferred, a member fits</text>
    <text x="440" y="348" font-weight="600">widened int8 WMMA GEMM</text>
    <text x="440" y="364" opacity="0.75">deqFor(format), WidenBudget admitted</text>
    <text x="730" y="118" font-weight="600">else B single-row launches</text>
    <text x="730" y="134" opacity="0.75">the wave kernel, B times</text>
    <text x="730" y="198" font-weight="600">else f16 wave, f32 wave, host</text>
    <text x="730" y="214" opacity="0.75">formats with no integer kernel</text>
    <text x="730" y="268" font-weight="600">packed WMMA GEMM</text>
    <text x="730" y="284" opacity="0.75">Q4_K and Q6_K, 16-row grain</text>
    <text x="730" y="348" font-weight="600">f16 coop GEMM, then the floor</text>
    <text x="730" y="364" opacity="0.75">the dot-product batch GEMM</text>
  </g>
  <g stroke="currentColor" stroke-width="1.2" fill="none" marker-end="url(#ra)">
    <line x1="150" y1="184" x2="188" y2="44"/>
    <line x1="150" y1="190" x2="188" y2="122"/>
    <line x1="150" y1="198" x2="188" y2="270"/>
    <line x1="390" y1="42" x2="428" y2="42"/>
    <line x1="390" y1="122" x2="428" y2="122"/>
    <path d="M390,272 L410,272 L410,202 L428,202"/>
    <line x1="390" y1="272" x2="428" y2="272"/>
    <path d="M390,272 L410,272 L410,352 L428,352"/>
    <line x1="680" y1="122" x2="718" y2="122"/>
    <path d="M680,42 L700,42 L700,202 L718,202"/>
    <line x1="680" y1="272" x2="718" y2="272"/>
    <line x1="680" y1="352" x2="718" y2="352"/>
    <path d="M825,294 L825,312 L700,312 L700,352"/>
  </g>
  <g fill="currentColor" font-size="10" opacity="0.7">
    <text x="404" y="194" text-anchor="end">on Vulkan</text>
    <text x="404" y="264" text-anchor="end">elsewhere</text>
    <text x="404" y="346" text-anchor="end">tile refused</text>
    <text x="708" y="322">refused: the next arm</text>
  </g>
</svg>
<figcaption>Where a projection's work goes. The colored boxes are the integer routes the engine prefers; each arrow out of a box is a gate the device answered (a kernel registered here), a measurement (the matrix cores pay, the Cols kernel pays, the tile beats the widen route) or a fit (rows and widths on the grain). A refused arm says so once with every gate's value; nothing falls to a slower arm silently.</figcaption>
</figure>

The arms of the chunk route, in the order tried, and what admits each:

| arm | admitted by | this model |
|---|---|---|
| tensor-core tile (`QkTile`, `Q6Tile`, `Q5Tile`, `Q2Tile`, `Q8Tile`, `Q3Tile` families) | not Vulkan, not an expert slot, `matrixCoresPay()`, `tilePreferred()` (always under an explicit weight policy, else `tileBeatsDeq()`), a member fits the padded rows (`rows % TM`, `outDim % TN`, `cols % 256`) | Q4_K and Q6_K tensors; rows padded to 128 |
| widened int8 WMMA GEMM (`*DeqMw8*`) | the format in the policy's widen list, the int8 copy made within the widen budget | the copies exist where `tileBeatsDeq` said the widen still pays |
| packed WMMA GEMM (`MwTile`, `q6kWmma*`) | Q4_K or Q6_K without a tile; 16-row grain | |
| f16 cooperative GEMM | always on Vulkan; elsewhere a format with no integer batch kernel, the tile dividing the shape | IQ and ternary formats, f16 |
| dp4a `mmq` tile | Vulkan, when the cooperative route's column constraint fails | |
| dot-product batch GEMM | the floor | |

Three signals carry most of the weight. `matrixCoresPay()` (`:2456-2497`) is
measured once per process, a matrix-core GEMM against the dot-product kernel
on a synthetic 4096² Q4_K problem, kept under a build id; `sm_89` pays, the
CPU backend's software tiles do not. `tileBeatsDeq()` (`:2538-2558`) decides
the tile against the widened int8 route at a 512-row gate shape; the widen
copies are made only where it says the widen still pays. `WidenBudget`
(`WidenBudget.cajeta:9-43`) admits each copy against live available memory,
not capacity, and a refusal is sticky. Split-K adds workgroups along the
reduction when a problem does not fill the device (the smallest divisor of
the block count up to 8 that does, `TcTile.cajeta:597-625`) and a fixup
launch sums the partials.

**Attention** has its own ordered route (`AttnKernel.cajeta:3360-3445`):

1. the single-pass online-softmax tile, the f16 variant
   `attnFlashPrefillF16Kernel` first where it is registered (45 KB of shared
   memory, two workgroups an SM), else `attnFlashPrefillOnlineKernel` (88 KB,
   the same tile carrying a residual pair); both need head dimension 128 and
   a grouped-query ratio dividing 64, and `prefillTilePays`, a once-per-process
   probe at 128 rows against the wave kernels (`:3080-3095`);
2. the two-pass tile `attnFlashPrefillTileKernel`;
3. the wave kernels, `attnFlashPrefillGqa4Kernel` for ratio 4 (one wave
   reading each K/V block once for the four heads that share it) or
   `attnFlashPrefillKernel`;
4. the score and combine pair.

When the online tile serves, the scores buffer that the two-pass route needs
is allocated as one float; it was 268 MB a slot on an 8B at context 4096
(`DeviceKv.cajeta:385-394`).

### 2.4 A mixture-of-experts layer in prefill

For Qwen3-Coder-30B-A3B, 128 experts with 8 active, the dense feed-forward
is replaced by a routed one (`MoeFfn.cajeta:2175-2330`). The zero-sync route,
taken when the matrix-core routes are on, the backend is not Vulkan, and the
three expert banks have a servable indexed GEMM (`mwServable`, `:2022-2026`):

1. **Router** as one GEMM over all rows (`routerRowsF32`), then **top-k** per
   row (`moeTopKBatch`): each row's eight experts and their weights.
2. **Grouping sort** (`moeGroupWaveKernel`, `QuantKernel.cajeta:20116-20131`):
   a counting sort by expert in one 256-thread workgroup, eight ballots
   covering up to 256 experts, writing the row order, the weights, and
   64-row and 128-row **chunk maps** with a count. It replaced a serial
   kernel that took 251 µs a layer against llama.cpp's 40.
3. **Gather and pack**: the selected rows gathered in expert order
   (`gatherRowsF32`) and packed to q8_K.
4. **Gate and up as indexed GEMMs** (`idGemm`): every chunk-map entry names
   an expert and a run of rows; a workgroup computes one row chunk against a
   tile of that expert's weights (`IdTile.cajeta:1-16`). The expert banks are
   one packed slab each, experts as offset views (`ExpertBank.cajeta:21-31`).
5. **GLU**, then **pack** with each row's slot on a block boundary.
6. **Down as an indexed GEMM**, then **scatter-combine** back into the rows
   with their routing weights (`moeScatterCombine`), then the shared expert
   where the family has one.

No synchronization and no download happens between the router and the
combine. The indexed GEMM itself is chosen per bank (`zeroSyncIdGemm`,
`:2031-2052`): the format's Id tile family first (`IdTile` 128×128 and 64×128
for Q4_K, `Q5IdTile` and `Q6IdTile` for the others), timed **in place** on
real tokens across the live layers rather than on a synthetic launch
(`IdTrial.cajeta:56-99`); then the wide 128-row-map GEMM over widened
weights; then the 64-row-map multi-wave kernel. Where none serves, the
grouped fallback (`:2338-2548`) downloads the selection, sorts on the host,
and runs a GEMM per expert group.

<figure class="diagram">
<svg viewBox="0 0 940 270" role="img" aria-label="A mixture-of-experts layer in prefill, zero-sync: router GEMM over all rows, top-k per row, the wave counting sort producing row order, weights and chunk maps, gather and pack in expert order, indexed GEMMs for gate and up reading one packed slab per bank through the chunk map, GLU and pack, indexed GEMM down, scatter-combine with the routing weights, the shared expert." style="max-width:100%;height:auto;font-family:var(--font-ui);font-size:11px">
  <defs><marker id="ma" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="currentColor"/></marker></defs>
  <g fill="none" stroke="currentColor" stroke-width="1.2">
    <rect x="10" y="20" width="130" height="48" rx="4"/>
    <rect x="170" y="20" width="110" height="48" rx="4"/>
    <rect x="310" y="20" width="160" height="48" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <rect x="500" y="20" width="130" height="48" rx="4"/>
    <rect x="660" y="20" width="140" height="48" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <rect x="820" y="20" width="110" height="48" rx="4"/>
    <rect x="690" y="120" width="140" height="48" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <rect x="500" y="120" width="160" height="48" rx="4"/>
    <rect x="310" y="120" width="160" height="48" rx="4"/>
    <rect x="310" y="200" width="520" height="56" rx="4" stroke-dasharray="4 3"/>
  </g>
  <g fill="currentColor">
    <text x="20" y="38" font-weight="600">router GEMM</text>
    <text x="20" y="54" opacity="0.75">routerRowsF32, all rows</text>
    <text x="180" y="38" font-weight="600">top-k a row</text>
    <text x="180" y="54" opacity="0.75">moeTopKBatch</text>
    <text x="320" y="38" font-weight="600">grouping sort</text>
    <text x="320" y="54" opacity="0.75">moeGroupWaveKernel</text>
    <text x="510" y="38" font-weight="600">gather + pack</text>
    <text x="510" y="54" opacity="0.75">rows in expert order</text>
    <text x="670" y="38" font-weight="600">gate, up id GEMMs</text>
    <text x="670" y="54" opacity="0.75">chunk map → expert tile</text>
    <text x="830" y="38" font-weight="600">GLU + pack</text>
    <text x="830" y="54" opacity="0.75">block-aligned slots</text>
    <text x="700" y="138" font-weight="600">down id GEMM</text>
    <text x="700" y="154" opacity="0.75">the same chunk map</text>
    <text x="510" y="138" font-weight="600">scatter-combine</text>
    <text x="510" y="154" opacity="0.75">× routing weight, += residual</text>
    <text x="320" y="138" font-weight="600">shared expert</text>
    <text x="320" y="154" opacity="0.75">qwen2moe, qwen3moe</text>
    <text x="320" y="218" font-weight="600">expert banks</text>
    <text x="320" y="234" opacity="0.75">one packed slab per bank (gate, up, down), experts as offset views;</text>
    <text x="320" y="248" opacity="0.75">the id GEMM is chosen per bank: Id tile family (timed in place), wide map, 64-row map</text>
  </g>
  <g stroke="currentColor" stroke-width="1.2" fill="none" marker-end="url(#ma)">
    <line x1="140" y1="44" x2="168" y2="44"/>
    <line x1="280" y1="44" x2="308" y2="44"/>
    <line x1="470" y1="44" x2="498" y2="44"/>
    <line x1="630" y1="44" x2="658" y2="44"/>
    <line x1="800" y1="44" x2="818" y2="44"/>
    <path d="M875,68 L875,144 L832,144"/>
    <line x1="690" y1="144" x2="662" y2="144"/>
    <line x1="500" y1="144" x2="472" y2="144"/>
    <path d="M760,198 L760,170" stroke-dasharray="3 3"/>
    <path d="M390,68 L390,100 L730,100 L730,118" stroke-dasharray="3 3"/>
  </g>
  <g fill="currentColor" font-size="10" opacity="0.7">
    <text x="400" y="96">the chunk maps feed every id GEMM</text>
  </g>
</svg>
<figcaption>The routed feed-forward over a chunk, zero-sync. The sort turns per-row choices into per-expert runs so that each expert's weights are read once per chunk; the indexed GEMMs then follow the chunk map into one slab per bank.</figcaption>
</figure>

### 2.5 The K/V cache during prefill

- On the device, `DeviceKv` holds flat f16 K and V planes per layer, position
  `t` at `t × kvDim`, in a `KvPool` whose slot `s` sits at `s × plane` so one
  batched attention launch can reach every sequence (`DeviceKv.cajeta:1-30`,
  `KvPool.cajeta:9-13`). Rotary and the append are one launch.
- The host `PagedKvCache` (16-token blocks, reference-counted, copy-on-write,
  rings for windowed layers) is deliberately left behind during a resident
  pass and caught up lazily (`adoptLazy`, `CausalLM.cajeta:3361-3381`); a
  host-path reader writes back first. Decode inherits the planes, so the
  first decode step reads what prefill wrote with no copy.
- The **block store** is a file-backed prefix cache keyed by a chained XXH3
  hash of the prefix, every extent checksummed on read, coldest-first
  eviction (`BlockStore.cajeta:1-24`). Admission adopts stored blocks up to
  the first miss on any layer; finish and preemption save full blocks.

## 3. Decode

### 3.1 The step, and who runs it

Per scheduler step (`Scheduler.cajeta:357-548`), for the running requests
with one token left to consume:

1. exactly one greedy request, and the model's pipeline is ready: the
   **pipelined** single-sequence decode (§3.2);
2. two or more greedy requests and the batch is admitted: the **batched**
   decode, up to eight rows a step, itself one step ahead (§3.3);
3. otherwise the plain step: a sampled request, or a model whose batch
   refusal was recorded, decodes one row with a synchronization.

Admission to the batch is latched once per model (`decodeBatchReady`,
`CausalLM.cajeta:3800-3823`): the K/V cache resident, the attention kernels
usable, head dimension at most 128, hidden and query widths multiples of
256, every projection and the output head with a Cols kernel for its format.
A refusal is recorded with its reason and the sequences decode singly.

### 3.2 One sequence: the pipelined decode

The host queues step N+1 before it has read step N's token (`stepPipelined`,
`Scheduler.cajeta:555-626`; `queueStep`, `CausalLM.cajeta:1074-1147`):

1. **Seed.** The prompt's last argmax is written into pinned device token
   slot 0 and step 0 → 1 is queued.
2. **Embed from the slot** (`embedRowLaunchNoSync` reading the token id on the
   device, `:1097-1104`); the host never learns a token before the step that
   consumes it is queued.
3. **Thirty-two resident layers** (`residentLayer`, `:3476-3775`), the
   residual never leaving the device:
   - norm and q8_K pack staged in one launch;
   - the three attention projections as **one fused wave mat-vec**
     (`qkvWaveMatVecKernel`) where the load-time A/B said it pays, else three
     wave mat-vecs; the prefetching twin where the variant probe chose it;
   - `DeviceKv.prep`: QK-norm, rotary and the K/V append in one launch
     (`DeviceKv.cajeta:289-308`);
   - attention **split across the context**: `attnFlashDecodeGqa4Kernel`
     writes partials over `ceil(waves per CU × CUs / nKv)` splits, up to 64
     (`AttnKernel.cajeta:3316-3340`); `attnReducePackQ8Kernel` reduces them
     and re-quantizes the result to q8_K in the same launch;
   - the output projection **accumulated into the residual** by the mat-vec
     itself (`Linear.cajeta:4649-4687`);
   - norm and pack; gate and up; GLU fused with the pack; down accumulated.
4. **The fused tail**: final norm, the output head as one wave mat-vec over
   128256 rows, `argmaxRowKernel` writing the token into the other pinned
   slot; an event recorded, the stream flushed, not synchronized
   (`:1522-1541`).
5. **Await the previous step's event** and read one integer; the next step
   is already queued. A stop the host could not foresee costs one drained
   step (`:619-626`).

<figure class="diagram">
<svg viewBox="0 0 920 310" role="img" aria-label="The pipelined decode as a timeline: on the host row, queue step N+1 (about a millisecond), wait on step N's event, read one integer, deliver the token, repeat; on the device row, step N and step N+1 back to back, each embedding from a pinned slot, 32 resident layers, the fused tail writing the argmax to the other slot and recording an event; two pinned slots alternate between steps; on CUDA each step's launch sequence is one graph replay." style="max-width:100%;height:auto;font-family:var(--font-ui);font-size:11px">
  <defs><marker id="ta" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="currentColor"/></marker></defs>
  <g fill="none" stroke="currentColor" stroke-width="1.2">
    <line x1="90" y1="70" x2="910" y2="70" opacity="0.35"/>
    <line x1="90" y1="200" x2="910" y2="200" opacity="0.35"/>
    <rect x="100" y="40" width="80" height="30" rx="3" stroke="var(--accent)" stroke-width="2"/>
    <rect x="190" y="40" width="190" height="30" rx="3" stroke-dasharray="4 3"/>
    <rect x="390" y="40" width="70" height="30" rx="3"/>
    <rect x="470" y="40" width="80" height="30" rx="3" stroke="var(--accent)" stroke-width="2"/>
    <rect x="560" y="40" width="190" height="30" rx="3" stroke-dasharray="4 3"/>
    <rect x="760" y="40" width="70" height="30" rx="3"/>
    <rect x="100" y="130" width="360" height="70" rx="3"/>
    <rect x="470" y="130" width="360" height="70" rx="3"/>
    <rect x="100" y="250" width="170" height="40" rx="3"/>
    <rect x="300" y="250" width="170" height="40" rx="3"/>
  </g>
  <g fill="currentColor">
    <text x="10" y="60" font-weight="600">host</text>
    <text x="10" y="170" font-weight="600">device</text>
    <text x="10" y="275" font-weight="600">pinned slots</text>
    <text x="106" y="59" fill="var(--accent)">queue N+1</text>
    <text x="198" y="59" opacity="0.75">wait on step N's event</text>
    <text x="396" y="59">read tok N</text>
    <text x="476" y="59" fill="var(--accent)">queue N+2</text>
    <text x="568" y="59" opacity="0.75">wait on step N+1's event</text>
    <text x="766" y="59">read N+1</text>
    <text x="110" y="150" font-weight="600">step N</text>
    <text x="110" y="166" opacity="0.75">embed the token from slot A; 32 resident layers:</text>
    <text x="110" y="180" opacity="0.75">fused q|k|v, prep, split attention + reduce-pack, o, FFN;</text>
    <text x="110" y="194" opacity="0.75">tail: norm, head, argmax → slot B; record an event</text>
    <text x="480" y="150" font-weight="600">step N+1</text>
    <text x="480" y="166" opacity="0.75">embed from slot B; 32 resident layers;</text>
    <text x="480" y="180" opacity="0.75">tail → slot A; event. On CUDA each step is one</text>
    <text x="480" y="194" opacity="0.75">graph replay with the position and grids patched in</text>
    <text x="110" y="268" font-weight="600">slot A</text>
    <text x="110" y="283" opacity="0.75">in for N, out for N+1</text>
    <text x="310" y="268" font-weight="600">slot B</text>
    <text x="310" y="283" opacity="0.75">out for N, in for N+1</text>
  </g>
  <g stroke="currentColor" stroke-width="1.2" fill="none" marker-end="url(#ta)">
    <line x1="140" y1="70" x2="140" y2="128"/>
    <line x1="510" y1="70" x2="510" y2="128"/>
    <path d="M455,200 L455,214 L425,214 L425,72" stroke-dasharray="3 3"/>
    <path d="M825,200 L825,214 L795,214 L795,72" stroke-dasharray="3 3"/>
    <path d="M185,250 L185,202" stroke-dasharray="2 3"/>
    <path d="M385,250 L385,202" stroke-dasharray="2 3"/>
    <path d="M470,270 L490,270 L490,228 L540,228 L540,202" stroke-dasharray="2 3"/>
  </g>
  <g fill="currentColor" font-size="10" opacity="0.7">
    <text x="150" y="100">the step's launch sequence, queued in about a millisecond</text>
    <text x="440" y="226" text-anchor="end">event: step N done</text>
    <text x="550" y="240">slot B feeds step N+1</text>
  </g>
</svg>
<figcaption>The pipelined single-sequence decode. The device is never idle between steps and the host's only wait is on the previous step's event; two pinned token slots alternate so that each step reads its input where the previous step wrote its argmax. On CUDA the step's launch sequence is replayed as a captured graph (§3.4).</figcaption>
</figure>

Pipelining needs the fused tail, a device argmax, a device-routed output
head and a device embedding decoder for the table's format
(`pipelineReady`, `:1010-1016`); where any is missing, the step runs plainly
with a synchronization. Every choice the step takes was made before the
first token: the wave kernel per weight latched at bind (the integer wave
when the kernel is here, the activations q8_K packed and the width a
multiple of 256; else the f16 or f32 wave; else the host,
`Linear.cajeta:5185-5280`), the prefetching variant per weight shape from the
tune store, the fused q|k|v from the load-time A/B. Nothing is timed during
decode. The measured standing on the 4090 is at or above llama.cpp's
single-sequence rate at every quantization from Q2_K to Q8_0: the kernels
were at the bus, and the gaps between them were the host, which the pipeline
removed.

### 3.3 Several sequences: the batched decode

One pass over a weight row serves every decoding sequence
(`decodeBatchRows`, `CausalLM.cajeta:3930-4087`):

1. **Embed every row from the device token buffer** (the previous step's
   argmaxes), norm and pack each row in one workgroup.
2. **Each projection as one Cols launch** over the batch's activation
   columns: one wave owns one weight row, decodes each block once and dots it
   against every column (`q4kQ8WaveMatVecColsKernel`,
   `QuantKernel.cajeta:13845-13853`); q|k|v and gate|up fused into one launch
   where all are Q4_K and the segment A/B said so (`Linear.cajeta:4452-4501`).
   Per launch, `ColsProbe.pays(format, width)` decides between the Cols
   kernel and that many single-row launches (`:4372-4420`): on the 4090 the
   Q4_K column kernel at eight columns once tied eight single rows, and a
   four-sequence decode ran 768 single-row launches a step until the probe
   existed (`ColsProbe.cajeta:49-56`).
3. **Attention in three launches for all rows** (`AttnKernel.cajeta:4653-4701`):
   `qkPrepBatchKernel`, then `attnFlashDecodeGqa8BatchKernel` (or the ratio-4
   twin) over the whole `KvPool` buffer guided by a device **row table** of
   `[slotBase, position, count, splits]` per row, then
   `attnReducePackQ8BatchKernel`. The 30B's decode batch attends in three
   launches a layer where it took twelve.
4. The output projection as a Cols launch accumulating into the residual;
   norm and pack; the dense feed-forward as Cols launches with the GLU and
   pack fused, or the routed feed-forward of §3.5.
5. **The tail per row**: final norm and pack, the output head as a Cols
   launch into the logits buffer, one `argmaxRowKernel` per row writing
   `tok[row]` into the device token buffer.

The batched step is itself pipelined (`BatchPipe.cajeta:16-52`): two pinned
token buffers, two events and two row tables alternate so that a host write
never lands in the step in flight; a change to the set (a finish, an
admission) lands the in-flight step first (`Scheduler.cajeta:915-922`). A
sampled request forces the synchronous form, which downloads the logits for
the host sampler (`:930-936`).

<figure class="diagram">
<svg viewBox="0 0 940 290" role="img" aria-label="The batched decode: four sequences' activation columns packed to q8_K; one Cols launch per projection where each wave owns one weight row, decodes each block once and dots it against all four columns; attention through the K/V pool in three launches guided by a row table; the tail writes one argmax per row into the token buffer the next step embeds from." style="max-width:100%;height:auto;font-family:var(--font-ui);font-size:11px">
  <defs><marker id="ba" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="currentColor"/></marker></defs>
  <g fill="none" stroke="currentColor" stroke-width="1.2">
    <rect x="10" y="30" width="150" height="120" rx="4"/>
    <line x1="47" y1="30" x2="47" y2="150" opacity="0.4"/>
    <line x1="84" y1="30" x2="84" y2="150" opacity="0.4"/>
    <line x1="121" y1="30" x2="121" y2="150" opacity="0.4"/>
    <rect x="200" y="30" width="300" height="120" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <line x1="200" y1="60" x2="500" y2="60" opacity="0.4"/>
    <line x1="200" y1="90" x2="500" y2="90" opacity="0.4"/>
    <line x1="200" y1="120" x2="500" y2="120" opacity="0.4"/>
    <rect x="540" y="30" width="200" height="120" rx="4"/>
    <rect x="780" y="30" width="150" height="120" rx="4"/>
    <rect x="200" y="205" width="330" height="60" rx="4" stroke-dasharray="4 3"/>
    <rect x="560" y="205" width="370" height="60" rx="4" stroke-dasharray="4 3"/>
  </g>
  <g fill="currentColor">
    <text x="10" y="20" font-weight="600">activations, 4 columns</text>
    <text x="16" y="94" font-size="10">s0</text>
    <text x="53" y="94" font-size="10">s1</text>
    <text x="90" y="94" font-size="10">s2</text>
    <text x="127" y="94" font-size="10">s3</text>
    <text x="10" y="168" opacity="0.75" font-size="10">q8_K packed, one workgroup a row</text>
    <text x="200" y="20" font-weight="600" fill="var(--accent)">one Cols launch per projection: W · [4 columns]</text>
    <text x="208" y="50" opacity="0.75">wave 0 owns weight row 0: each block once, four dots</text>
    <text x="208" y="80" opacity="0.75">wave 1 owns weight row 1</text>
    <text x="208" y="110" opacity="0.75">wave 2 …</text>
    <text x="208" y="140" opacity="0.75">one pass over the weights serves every sequence</text>
    <text x="540" y="20" font-weight="600">attention, 3 launches</text>
    <text x="550" y="52" opacity="0.75">qkPrepBatch: norm, rope, append</text>
    <text x="550" y="76" opacity="0.75">attnFlashDecodeGqa8Batch over</text>
    <text x="550" y="90" opacity="0.75">the K/V pool, per the row table</text>
    <text x="550" y="114" opacity="0.75">attnReducePackQ8Batch</text>
    <text x="550" y="138" opacity="0.75">[slotBase, pos, count, splits] × 4</text>
    <text x="780" y="20" font-weight="600">tail per row</text>
    <text x="790" y="52" opacity="0.75">norm + pack</text>
    <text x="790" y="76" opacity="0.75">head as a Cols launch</text>
    <text x="790" y="100" opacity="0.75">argmaxRowKernel × 4</text>
    <text x="790" y="124" opacity="0.75">→ tok[row], on device</text>
    <text x="210" y="225" font-weight="600">per launch: the Cols kernel or single rows</text>
    <text x="210" y="241" opacity="0.75">ColsProbe.pays(format, width), probed at load, widths 2 to 8;</text>
    <text x="210" y="255" opacity="0.75">fused q|k|v and gate|up where the in-situ A/B said so</text>
    <text x="570" y="225" font-weight="600">pipelined too (BatchPipe)</text>
    <text x="570" y="241" opacity="0.75">two token buffers, two events and two row tables alternate;</text>
    <text x="570" y="255" opacity="0.75">a change to the set lands the step in flight first</text>
  </g>
  <g stroke="currentColor" stroke-width="1.2" fill="none" marker-end="url(#ba)">
    <line x1="160" y1="90" x2="198" y2="90"/>
    <line x1="500" y1="90" x2="538" y2="90"/>
    <line x1="740" y1="90" x2="778" y2="90"/>
    <path d="M855,150 L855,185 L120,185 L120,152" stroke-dasharray="3 3"/>
  </g>
  <g fill="currentColor" font-size="10" opacity="0.7">
    <text x="300" y="182">next step: embed every row from tok[] on the device</text>
  </g>
</svg>
<figcaption>The batched decode. Four sequences cost close to one in weight traffic because every wave reads its weight row once and serves every column; attention reaches every sequence's cache in one launch through the pool; the tokens never leave the device between steps.</figcaption>
</figure>

### 3.4 Where the launch overhead went

A decode step is about 1,360 launches of a few microseconds each. The
engine's one stream is **deferred** (`QuantKernel.cajeta:56-75`,
`KernelStream.cajeta:101-131` in the runtime): launches are queued and
submitted together at a synchronization, host transfer, event or explicit
flush; a queue whose kernel sequence was seen before is replayed as one
recording (a CUDA graph on NVPTX) with only the changed grids and arguments
patched into the nodes. The pipelined tail flushes rather than synchronizes,
so the host returns to queue the next step while the device runs this one.
Deferral is a no-op on HIP and Vulkan, so there the host pays for every
launch and the pipelining matters more, not less; `CAJETA_XPU_DEFER=0` is
the A/B control. Every fusion in §3.2 and §3.3 exists for the same reason:
one launch fewer, and one pass over the activations fewer.

### 3.5 A mixture-of-experts layer in decode

One row (`forwardRowZeroSync`, `MoeFfn.cajeta:1631-1764`): the norm, pack,
router and top-k in one launch (`rmsnormRouterTopKKernel`, for hidden widths
to 2048 and up to 128 experts); gate and up with the GLU and the pack as
fused indexed launches when both banks are Q4_K slabs; the down projection
fused with the combine onto the residual **and the next layer's input norm
and pack** in its tail (`idDownCombineTail`, `:1736-1744`).

Several rows (`forwardRowsZeroSync`, `:1504-1546`): nine launches a layer
regardless of the row count: the router as one block per expert over all
rows (the expert's weights read once, then from L1), top-k one wave per row,
each row's packed activation replicated once per selected expert, one
**indexed mat-vec per bank** over all `rows × k` selections (gate, up), GLU,
pack, down, and a combine-add. The 30B's four-sequence decode went from
twelve attention launches and a launch per expert to this.

### 3.6 Tokens out

Greedy sampling is the default and happens on the device: the argmax kernel
(`QuantKernel.cajeta:20357-20456`) reduces 1024 elements a workgroup, the
last group reduces the partials, and ties keep the smaller index so that it
equals the host's. A sampled request runs the host sampler on the logits
(`Sampler.cajeta:60-203`): the repetition penalty over emitted tokens,
temperature, a heapsort top-k, a stable softmax, top-p, one xorshift draw,
zero allocations a token. The sink is called before the next step; the stop
rules are a stop token, stop strings searched in the decode of a rolling
16-token tail, and the budget (`Scheduler.cajeta:960-1005`). A finished
request releases its slot (draining a speculative step first) and saves its
full blocks to the store.

## 4. The optimizer, kernel by kernel

Every decision above is made at one of four moments, and the moment decides
what it may read.

<figure class="diagram">
<svg viewBox="0 0 940 240" role="img" aria-label="The four moments an optimization is decided: at compile time the adaptor instantiates class templates at shapes, refuses shapes the body cannot serve, budgets registers, and records registers, spill, shared memory and tier in the manifest; at bind the shape filter prunes members by the device's facts; at load and first use the probes measure and the store remembers, under a build id; per launch the padding, split-K and Cols-or-rows choices read the remembered answers without timing." style="max-width:100%;height:auto;font-family:var(--font-ui);font-size:11px">
  <defs><marker id="oa" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="currentColor"/></marker></defs>
  <g fill="none" stroke="currentColor" stroke-width="1.2">
    <rect x="10" y="20" width="215" height="160" rx="4"/>
    <rect x="250" y="20" width="215" height="160" rx="4"/>
    <rect x="490" y="20" width="215" height="160" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <rect x="730" y="20" width="200" height="160" rx="4"/>
  </g>
  <g fill="currentColor">
    <text x="20" y="40" font-weight="600">compile time</text>
    <text x="20" y="54" opacity="0.6" font-size="10">the adaptor, the manifest</text>
    <text x="20" y="76" opacity="0.8">@Instantiate members at shapes</text>
    <text x="20" y="92" opacity="0.8">@Requires refuses shapes</text>
    <text x="20" y="108" opacity="0.8">@Occupancy budgets registers</text>
    <text x="20" y="124" opacity="0.8">manifest: registers, spill, shared</text>
    <text x="20" y="140" opacity="0.8">bytes, tier (wmma native or not)</text>
    <text x="20" y="156" opacity="0.8">a spill or an unlowered backend</text>
    <text x="20" y="172" opacity="0.8">fails the build by name</text>
    <text x="260" y="40" font-weight="600">bind</text>
    <text x="260" y="54" opacity="0.6" font-size="10">the filter, per problem</text>
    <text x="260" y="76" opacity="0.8">DeviceFacts from the driver:</text>
    <text x="260" y="92" opacity="0.8">registers, shared memory, blocks</text>
    <text x="260" y="108" opacity="0.8">R1 spill, R2 register file,</text>
    <text x="260" y="124" opacity="0.8">R3 shared limit, R4 no resident</text>
    <text x="260" y="140" opacity="0.8">workgroup; each pruned member</text>
    <text x="260" y="156" opacity="0.8">keeps its reason; no survivor</text>
    <text x="260" y="172" opacity="0.8">is a named refusal</text>
    <text x="500" y="40" font-weight="600" fill="var(--accent)">load and first use</text>
    <text x="500" y="54" opacity="0.6" font-size="10">measured, remembered under a build id</text>
    <text x="500" y="76" opacity="0.8">survivors timed in interleaved</text>
    <text x="500" y="92" opacity="0.8">rounds, fastest median (shape.*)</text>
    <text x="500" y="108" opacity="0.8">matrixCoresPay, tileBeatsDeq,</text>
    <text x="500" y="124" opacity="0.8">prefillTilePays; colsPays per width</text>
    <text x="500" y="140" opacity="0.8">mvVariant per shape; the fused A/B</text>
    <text x="500" y="156" opacity="0.8">Id tiles timed in place on real</text>
    <text x="500" y="172" opacity="0.8">tokens</text>
    <text x="740" y="40" font-weight="600">per launch</text>
    <text x="740" y="54" opacity="0.6" font-size="10">no timing</text>
    <text x="740" y="76" opacity="0.8">padding to the tile grain</text>
    <text x="740" y="92" opacity="0.8">split-K to fill the device</text>
    <text x="740" y="108" opacity="0.8">a Cols launch or single rows</text>
    <text x="740" y="124" opacity="0.8">batch admission, graph replay</text>
    <text x="740" y="140" opacity="0.8">the launch checked after every</text>
    <text x="740" y="156" opacity="0.8">submission: seven named</text>
    <text x="740" y="172" opacity="0.8">refusals</text>
  </g>
  <g stroke="currentColor" stroke-width="1.2" fill="none" marker-end="url(#oa)">
    <line x1="225" y1="100" x2="248" y2="100"/>
    <line x1="465" y1="100" x2="488" y2="100"/>
    <line x1="705" y1="100" x2="728" y2="100"/>
  </g>
  <g fill="currentColor" font-size="10" opacity="0.7">
    <text x="10" y="208">What each moment reads: the source and the shape → the device's facts and the manifest → the clock → the remembered answers.</text>
    <text x="10" y="224">The store keys every entry by device geometry and build id: a store copied between machines, or a rebuilt kernel, re-measures rather than believes.</text>
  </g>
</svg>
<figcaption>Four moments, four kinds of evidence. Nothing is decided from a vendor's name: the compiler records what it built, the driver reports what the part is, the clock says what is faster here, and the launch reads the answers.</figcaption>
</figure>

| kernel or decision | compile time | bind | load or first use | per launch |
|---|---|---|---|---|
| the prefill tiles (`QkTile`, `Q6Tile`, `Q5Tile`, `Q2Tile`, `Q8Tile`, `Q3Tile`) | one template, two members (128×128 at 256 threads, 64×64 at 128), `@Requires`, `@Occupancy`; manifest: registers, no spill, shared bytes, `wmma` native | `ShapeFilter` R1 to R6 at `(rows, out, in)`; the 128 member pruned on gfx1151 by the 64 KB shared limit | `ShapeChoice`, five interleaved rounds, remembered as `shape.<family>.MxNxK`; prewarmed at load for every shape the batch can take | padding to the grain, split-K when the grid does not fill the device |
| the widened int8 GEMMs (`*DeqMw8*`) | `@Occupancy` in waves, so the block follows the wave width | the registry | `tileBeatsDeq` once per process; the copies made under `auto` where they pay, inside the widen budget | |
| the wave mat-vecs and their `Pf` twins | lane index `tid % Group.width()`; `@Wave(32)` only where the body needs it | `KernelCap.usable`, width a multiple of 256 | `probeArms`: prefetching, plain or two-row, remembered as `mvVariant-…` | never chunked across launches |
| the fused q\|k\|v and gate\|up launches | | | the in-situ A/B on the real batched step at load | |
| the Cols kernels and their fused segments | | `colsReady` per projection, latched once | the Cols probe per format and width 2 to 8 (`colsPays-…`), each candidate first checked against the host mat-vec | one Cols launch or single rows |
| the decode attention kernels and their `Batch` twins | specialized at ratios 4 and 8, head dimension 128; a batched twin held byte-equal to the per-row kernel by a test | `KernelCap.usable` | | the split count follows the context |
| the prefill attention tiles | the f16 online tile at 45 KB of shared memory against the residual tile's 88 KB | the f16 kernel first where registered, so a test can withhold either | `prefillTilePays` once per process at 128 rows | |
| the Id GEMM families | the same adaptor as the dense tiles | the same filter | timed **in place** on real tokens, one layer a sample, trials held during warm-up | the chunk map |
| the fusions (`qkPrep`, `gluFusedPack`, `rmsnormPack`, `attnReducePackQ8`, `idDownCombineTail`) | chosen by design: one launch and one activation pass fewer each | | | |
| the device embedding and argmax | | `pipelineReady`: what makes the pipeline legal | | |
| every kernel | a spill or an unlowered backend fails the build by name | the registry answers absent, never falls through | an answer is kept only against the build that measured it; one taken under foreign load is used and not stored | the launch is checked after every submission |

The CPU backend runs the same catalog as a target, not an emulator: the
work-item loop vectorizes at the host's SIMD width and the wave verbs become
vector reductions and shuffles. It reports no register or spill count, so
the filter prunes nothing there and what differs is which members are fast:
`matrixCoresPay` answers no (its software tiles run 11 to 21 times slower)
and the dot-product floor serves the prefill (`Linear.cajeta:2825-2830`).

## 5. What changes with the architecture

### 5.1 Model families

The family decides the **shape of a layer** and nothing about kernels. From a
GGUF's `general.architecture` (`ModelConfig.cajeta:167-199`):

| family | what it switches on |
|---|---|
| `llama` | the base layer: RMSNorm, grouped-query attention, rotary embedding, SwiGLU; Llama 2 and 3.x, Mistral; **Mixtral and every Llama-shaped mixture** is this family with `expert_count > 0`, not a name (`:246-251`) |
| `qwen2`, `qwen2vl` | the text stack of Qwen2 |
| `qwen2moe` | routed experts with a **shared expert** and its gate, top-k weights not renormalized |
| `qwen3`, `qwen3moe` | **QK-norm** per head before the rotary embedding (`:240-242`); top-k weights renormalized |
| `bitnet` | ternary weights with **sub-norms** inside the blocks (`:347`) |

From a Hugging Face `config.json`: `LlamaForCausalLM` and `BitnetForCausalLM`
(`:338-344`). Every knob is read from the configuration, never assumed from a
name: head counts and `head_dim`, the norm epsilon, the rotary base and its
scaling (linear, NTK, YaRN, llama3, with the Llama-3.1 frequency table read
from `rope_freqs.weight`), sliding and per-layer windows (`layer_types`),
tied word embeddings (the absence of `output.weight`), the expert count, the
active count, the gating function and whether the weights renormalize, the
shared expert. The plumbing for Gemma's `(1 + w)` norm, four norms a layer
and GeGLU exists and is dormant: no architecture string sets it yet.

<figure class="diagram">
<svg viewBox="0 0 1060 270" role="img" aria-label="One layer with its optional blocks by family: the base path is norm, q k v, rope, attention, o-proj, norm, gate up GLU down; qwen3 inserts QK-norm between the projections and rope; bitnet adds sub-norms inside the attention and feed-forward blocks; a mixture family replaces the feed-forward with router, top-k, experts and, for qwen2moe, a shared expert; windowed layers attend over a ring; tied embeddings reuse the table as the output head." style="max-width:100%;height:auto;font-family:var(--font-ui);font-size:11px">
  <defs><marker id="ya" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="currentColor"/></marker></defs>
  <g fill="none" stroke="currentColor" stroke-width="1.2">
    <rect x="10" y="60" width="96" height="44" rx="4"/>
    <rect x="132" y="60" width="134" height="44" rx="4"/>
    <rect x="292" y="60" width="116" height="44" rx="4" stroke="var(--accent)" stroke-width="2" stroke-dasharray="4 3"/>
    <rect x="434" y="60" width="126" height="44" rx="4"/>
    <rect x="586" y="60" width="116" height="44" rx="4"/>
    <rect x="728" y="60" width="90" height="44" rx="4"/>
    <rect x="844" y="60" width="206" height="44" rx="4"/>
    <rect x="844" y="140" width="206" height="90" rx="4" stroke="var(--accent)" stroke-width="2" stroke-dasharray="4 3"/>
    <rect x="586" y="140" width="150" height="44" rx="4" stroke="var(--accent)" stroke-width="2" stroke-dasharray="4 3"/>
    <rect x="132" y="140" width="290" height="44" rx="4" stroke="var(--accent)" stroke-width="2" stroke-dasharray="4 3"/>
  </g>
  <g fill="currentColor">
    <text x="20" y="78" font-weight="600">norm</text>
    <text x="20" y="93" opacity="0.75" font-size="10">RMSNorm, eps</text>
    <text x="142" y="78" font-weight="600">q, k, v</text>
    <text x="142" y="93" opacity="0.75" font-size="10">GQA counts, head_dim</text>
    <text x="302" y="78" font-weight="600" fill="var(--accent)">QK-norm</text>
    <text x="302" y="93" opacity="0.75" font-size="10">qwen3, qwen3moe</text>
    <text x="444" y="78" font-weight="600">rope</text>
    <text x="444" y="93" opacity="0.75" font-size="10">base, scaling, freq table</text>
    <text x="596" y="78" font-weight="600">attention</text>
    <text x="596" y="93" opacity="0.75" font-size="10">full or windowed</text>
    <text x="738" y="78" font-weight="600">o-proj</text>
    <text x="738" y="93" opacity="0.75" font-size="10">+= residual</text>
    <text x="854" y="78" font-weight="600">norm · gate, up, GLU, down</text>
    <text x="854" y="93" opacity="0.75" font-size="10">SwiGLU; the Gemma forms dormant</text>
    <text x="854" y="158" font-weight="600" fill="var(--accent)">mixture of experts</text>
    <text x="854" y="174" opacity="0.75" font-size="10">expert_count > 0, in any family</text>
    <text x="854" y="188" opacity="0.75" font-size="10">router, top-k, softmax gating,</text>
    <text x="854" y="202" opacity="0.75" font-size="10">renormalized (not qwen2moe),</text>
    <text x="854" y="216" opacity="0.75" font-size="10">shared expert + gate (qwen2moe)</text>
    <text x="596" y="158" font-weight="600" fill="var(--accent)">windows</text>
    <text x="596" y="173" opacity="0.75" font-size="10">sliding, per-layer rings</text>
    <text x="142" y="158" font-weight="600" fill="var(--accent)">bitnet sub-norms</text>
    <text x="142" y="173" opacity="0.75" font-size="10">inside attention and feed-forward; ternary TQ weights</text>
  </g>
  <g stroke="currentColor" stroke-width="1.2" fill="none" marker-end="url(#ya)">
    <line x1="106" y1="82" x2="130" y2="82"/>
    <line x1="266" y1="82" x2="290" y2="82"/>
    <line x1="408" y1="82" x2="432" y2="82"/>
    <line x1="560" y1="82" x2="584" y2="82"/>
    <line x1="702" y1="82" x2="726" y2="82"/>
    <line x1="818" y1="82" x2="842" y2="82"/>
    <path d="M947,104 L947,138" stroke-dasharray="3 3"/>
    <path d="M644,104 L644,138" stroke-dasharray="3 3"/>
    <path d="M200,104 L200,138" stroke-dasharray="3 3"/>
  </g>
  <g fill="currentColor" font-size="10" opacity="0.7">
    <text x="10" y="30">the base layer (llama); the dashed colored boxes are what a family switches on from its configuration</text>
    <text x="10" y="250">tied embeddings: the output head is the embedding table (the absence of output.weight). Adding a family touches ModelConfig (keys, flags),</text>
    <text x="10" y="264">Checkpoint's name table, DecoderLayer / Attention / MoeFfn for a new block shape, CausalLM for a new branch, and the census and oracle files for a new kernel.</text>
  </g>
</svg>
<figcaption>What a model family changes. Every family runs the same kernels on the same routes; it differs in which blocks a layer has and which configuration keys fill them.</figcaption>
</figure>

### 5.2 Weight formats

A checkpoint mixes formats (the Q4_K_M recipe is Q4_K with Q6_K in the value
and down projections of half the layers and the output head), and every
decision in §2.3 is made **per tensor** from its format (`packedTy`): whether
the weight stays packed (`Linear.packedSupported`, `Linear.cajeta:891-914`),
which kernel ids exist for it (`KernelCap.waveMatVecId`, `f32WaveId`,
`batchGemmId`, `KernelCap.cajeta:335-376`), which tile family serves it
(`TcTile.cajeta:76-116`) and whether a Cols kernel exists (Q4_K, Q6_K, Q5_K,
Q8_0, Q2_K, Q3_K). Every format has a host implementation, so a model whose
format no device kernel serves still runs. The IQ codebooks are ggml's grids
byte for byte, uploaded once per process (`IqGrid.cajeta`,
`QuantKernel.cajeta:7101-7111`). A format gains a prefill family by one
`*Tile` class implementing the tile shape, two instantiations in the family
accessor, an availability latch and a route predicate in `Linear`
(`:1491-1523, 2146-2163`); the [models page](llm/models.md) has the
format-by-regime table and the [catalog](catalog-llm.md) every kernel.

### 5.3 Backends

One source serves CUDA, HIP, Vulkan and the CPU. The code never branches on
a vendor's name, with one exception; it asks three kinds of question:

- **Is the kernel here?** `KernelCap.usable(name)` is the registry (does this
  backend hold device code for the kernel whose shared memory fits this
  part) and the library's own defect list keyed `backend:kernel`
  (`KernelCap.cajeta:161-217, 265-271`). A kernel the compiler could not lower
  for a backend is declared `@Unlowered` at build, is absent from the
  registry, and nothing falls through to it. On Vulkan that is every WMMA,
  tile and Id kernel (`QkTile.cajeta:84`, `IdTile.cajeta:94`,
  `WmmaKernel.cajeta:958`); on AMDGPU the one flash kernel over 88 KB of
  shared memory (`AttnKernel.cajeta:2467`); on the CPU the flash attention
  and, by the defect list, the per-format tiles and fused Cols kernels.
- **What does the device report?** `DeviceFacts.current()`: the shared-memory
  limit per block with and without opt-in, registers and threads per
  multiprocessor, the wave size, free memory, what other processes hold.
  This is what prunes the 128×128 tile on gfx1151 (64 KB a block, no opt-in)
  and keeps it on the 4090 (99 KB with opt-in).
- **What is faster here?** `matrixCoresPay`, `tileBeatsDeq`, `prefillTilePays`,
  the Cols probe, the variant probe: measured, remembered per device key.

The exception is `backendIsVulkan()` (`Linear.cajeta:2795-2805`): under RADV
the WMMA epilogue demotes to a software tile that trips the display ring's
watchdog, so Vulkan turns the WMMA, tile and widen routes off and batches
through the f16 cooperative GEMM, the `mmq` tile or the floor.

<figure class="diagram">
<svg viewBox="0 0 1060 330" role="img" aria-label="Where the four backends part for this model: the backend is chosen in a fixed order; on CUDA the int8 tiles are native wmma, the 128 member opts into 99 KB of shared memory, and decode launches are replayed as a graph; on HIP the same tiles are native wave-32 WMMA, the 64 KB block limit prunes the 128 member so the 64 member serves, and launches are issued one by one with the batched pipeline covering the host; on Vulkan the tile and widen routes are off, prefill goes through the f16 cooperative GEMM, the mmq tile or the floor, and there is no event clock; on the CPU the same catalog vectorizes at the host SIMD width, the matrix cores do not pay and the floor serves." style="max-width:100%;height:auto;font-family:var(--font-ui);font-size:11px">
  <defs><marker id="sa" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto-start-reverse"><path d="M0,0 L10,5 L0,10 z" fill="currentColor"/></marker></defs>
  <g fill="none" stroke="currentColor" stroke-width="1.2">
    <rect x="10" y="20" width="150" height="44" rx="4"/>
    <rect x="200" y="20" width="200" height="44" rx="4" stroke="var(--accent)" stroke-width="2"/>
    <rect x="420" y="20" width="200" height="44" rx="4"/>
    <rect x="640" y="20" width="200" height="44" rx="4"/>
    <rect x="860" y="20" width="190" height="44" rx="4"/>
    <line x1="200" y1="114" x2="1050" y2="114" opacity="0.35"/>
    <line x1="200" y1="170" x2="1050" y2="170" opacity="0.35"/>
    <line x1="200" y1="226" x2="1050" y2="226" opacity="0.35"/>
    <line x1="200" y1="282" x2="1050" y2="282" opacity="0.35"/>
  </g>
  <g fill="currentColor">
    <text x="20" y="38" font-weight="600">one source</text>
    <text x="20" y="54" opacity="0.75" font-size="10">chosen: CUDA, HIP, Vulkan, CPU</text>
    <text x="210" y="38" font-weight="600" fill="var(--accent)">CUDA (sm_89)</text>
    <text x="210" y="54" opacity="0.75">the measured reference</text>
    <text x="430" y="38" font-weight="600">HIP (gfx1151)</text>
    <text x="430" y="54" opacity="0.75">unified memory</text>
    <text x="650" y="38" font-weight="600">Vulkan (RADV)</text>
    <text x="650" y="54" opacity="0.75">the portable arm</text>
    <text x="870" y="38" font-weight="600">CPU</text>
    <text x="870" y="54" opacity="0.75">a target, not an emulator</text>
  </g>
  <g fill="currentColor" font-size="10">
    <text x="10" y="92" font-weight="600">prefill GEMM</text>
    <text x="210" y="86" opacity="0.8">int8 tile, native wmma;</text>
    <text x="210" y="100" opacity="0.8">the 128 member at 99 KB opt-in</text>
    <text x="430" y="86" opacity="0.8">int8 tile, native wave-32 WMMA;</text>
    <text x="430" y="100" opacity="0.8">the 64 member (64 KB block limit)</text>
    <text x="650" y="86" opacity="0.8">tiles and widen off; f16 coop</text>
    <text x="650" y="100" opacity="0.8">GEMM, the mmq tile, the floor</text>
    <text x="870" y="86" opacity="0.8">the floor: matrixCoresPay</text>
    <text x="870" y="100" opacity="0.8">answers no (software tiles)</text>
    <text x="10" y="148" font-weight="600">decode launches</text>
    <text x="210" y="142" opacity="0.8">deferred stream, graph replay;</text>
    <text x="210" y="156" opacity="0.8">a step issued in about 1 ms</text>
    <text x="430" y="142" opacity="0.8">one by one; the batched</text>
    <text x="430" y="156" opacity="0.8">pipeline covers the host</text>
    <text x="650" y="142" opacity="0.8">one by one; a declared wave-32</text>
    <text x="650" y="156" opacity="0.8">kernel runs as a virtual wave</text>
    <text x="870" y="142" opacity="0.8">the work-item loop vectorized</text>
    <text x="870" y="156" opacity="0.8">at the host's SIMD width</text>
    <text x="10" y="204" font-weight="600">prefill attention</text>
    <text x="210" y="198" opacity="0.8">f16 online tile (45 KB) and</text>
    <text x="210" y="212" opacity="0.8">the residual tile (88 KB)</text>
    <text x="430" y="198" opacity="0.8">f16 online tile; the 88 KB</text>
    <text x="430" y="212" opacity="0.8">tile is unlowered</text>
    <text x="650" y="198" opacity="0.8">the residual tile where the</text>
    <text x="650" y="212" opacity="0.8">f16 one is held back</text>
    <text x="870" y="198" opacity="0.8">no flash kernel registered;</text>
    <text x="870" y="212" opacity="0.8">host attention</text>
    <text x="10" y="260" font-weight="600">timing</text>
    <text x="210" y="254" opacity="0.8">event clock; CUPTI tier</text>
    <text x="210" y="268" opacity="0.8">for profiles</text>
    <text x="430" y="254" opacity="0.8">event clock; rocprofiler</text>
    <text x="430" y="268" opacity="0.8">tier for profiles</text>
    <text x="650" y="254" opacity="0.8">no event clock: the host</text>
    <text x="650" y="268" opacity="0.8">clock times the probes</text>
    <text x="870" y="254" opacity="0.8">host clock</text>
  </g>
  <g stroke="currentColor" stroke-width="1.2" fill="none" marker-end="url(#sa)">
    <line x1="160" y1="42" x2="198" y2="42"/>
  </g>
  <g fill="currentColor" font-size="10" opacity="0.7">
    <text x="10" y="306">standing (2026-10-24): on CUDA, single-sequence decode at or above llama.cpp, four-sequence 0.89 to 0.97, Q4_K and Q6_K prefill 1.1 to 1.2;</text>
    <text x="10" y="320">on gfx1151 under Vulkan, four-sequence decode at 0.98 of llama.cpp's Vulkan.</text>
  </g>
</svg>
<figcaption>Where the backends part for this model. The source is one on every row; what differs is a fact the device reported (the shared-memory limit, the wave), a tier the compiler recorded, or a mechanism one backend has and another lacks (graph replay, the event clock).</figcaption>
</figure>

What keeps the four honest is the **kernel census**
(`selftest/KernelCensus.cajeta:320-434`): after the suite, every kernel
registered on the active backend must be checked by a test, tracked as a
probe or a plan item, or declared external, and every shipped kernel needs a
row in the oracle map naming its llama.cpp counterpart. A new `@Kernel`
registers itself wherever it lowers and fails both gates until a test checks
its values; a new backend reruns the same gate keyed by its name.

## 6. Reading a run

`--verbose` registers a diagnostic callback and every decision on this page
announces itself once through it (`Diag.cajeta`): `prefill-mode` and
`decode-mode`, `batch-route` and `batch-refused` per projection,
`tile-choice`, `cols-probe`, `mc-probe`, `mv-probe`, `kernel-avail`,
`widen-budget`, `kv-ceiling`, `load-phase`, `sched-phase` and
`decode-batch-phase`. `CAJETA_PROFILER=1` writes a trace that shows the load
as its phases, each prefill chunk as the per-layer sequence of §2.2 with the
tiles the longest spans, and each decode step as a dense run of launches
with the two events per step and the host's wait on the previous event as
its only gap (an armed profiler keeps every launch direct, so the graph
replay is off while tracing). [Measuring](llm/measuring.md) says how a
number is read against llama.cpp and the [benchmarks](../../bench/llm/README.md)
hold the numbers.

## Where to go from here

- The kernel families and how one is chosen, in full:
  [Language models on the device](kernels-llm.md).
- The engine around them: [Inside the engine](llm/inside.md), the
  [API](llm/engine.md), [models and formats](llm/models.md),
  [configuration](llm/configuration.md).
- The substrate: [One kernel, any device](README.md), the
  [scheduler](scheduler.md), [optimization](optimization.md), and the
  contract, [cajeta.xpu](../specification/xpu/CajetaXPU.md).
