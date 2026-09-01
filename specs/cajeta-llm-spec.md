# cajeta-llm — decoder-only LLM inference engine

## 1. Definition

**1.1 Purpose.** A self-contained LLM inference engine written in Cajeta,
modeled on llama.cpp's architecture: load open weights, build a decoder
graph, run KV-cached incremental decode, sample, detokenize. Target models
are Llama 3.x, Mistral, Qwen2/3, and Gemma 3 (text path), loaded from Hugging
Face safetensors and from GGUF, executing on CPU and on all four XPU backends.
Gemma 3 is included deliberately: it exercises per-layer attention config
(§5) and the windowed ring-buffer cache (§6) that the others do not, so it
keeps those abstractions honest rather than speculative.

**1.2 Why a rewrite and not a binding.** `@Native` marshals scalars,
opaque `pointer` handles, and Cajeta array headers only — no C structs by
value, no callbacks, no varargs
(`docs/specification/buildtool/native-deps-spec.md:41`). llama.cpp's API is
struct- and callback-shaped, so binding it is not expressible in the
language. The engine is rewritten against `cajeta.math.Tensor` and
`cajeta.xpu`.

**1.3 What it consumes, and what it deliberately does not.**
- Consumes `cajeta.math.Tensor` (numpy surface), `cajeta.xpu`
  (`@Kernel`, `KernelBuffer`, `KernelStream`, `CooperativeMatrix`), the
  `dev.cajeta.ml` checkpoint layer (`io/Safetensors`, `io/PtReader`,
  `io/Checkpoints` name reconciliation), `dev.cajeta.codec.protobuf` for raw
  SentencePiece `tokenizer.model` files, `dev.cajeta.codec.json` for tokenizer
  metadata and as the chat-template interpreter's value model (§13.18), and
  `cajeta.collection.ltm` (`LtmPager`, `LtmBPlusTree`) for the block store
  (§13.17).
- Does **not** consume `dev.cajeta.ml`'s `nn.Module.forward` or `GradTape`.
  That stack is training-first and tape-mandatory, `Module` is f32-pinned
  and non-generic (`nn/Module.cajeta:36-41`), and `grad/Ops.cajeta:31`
  returns `hasGpu() == false` with `requireCpu` throwing on any
  device-resident input. Only `Module`'s parameter naming and `state_dict`
  reconciliation are reused.
- Traces to `research-platform-roadmap-spec` §3.1 (tensor surface), §3.2
  (multi-vendor GPU), §8.5 (quantization).

**1.4 Constraints imposed by the language.**
- The integer ladder is 8/16/32/64/128 with no sub-byte and no arbitrary
  width; `float8*`/`float6*`/`float4e2m1` are opaque `iN` storage with no
  arithmetic or conversions (`src/cajeta/type/CajetaType.cpp:597-599`). This
  does **not** block sub-byte weight formats: they are storage, not
  arithmetic — see §10 and §13.5.
- Structured concurrency (`async`/`spawn`) has real limits — fibers pinned to
  their first carrier, ~9–25 µs spawn, a 16-carrier cap. **These do not apply
  to kernel execution.** XPU dispatch uses its own persistent worker pool
  (`runtime/native/cajeta_xpu_dispatch.c:435-454`): a launch is a broadcast
  plus barrier with no per-launch thread spawn, grid blocks chunk across
  `min(gridX, cores)` workers, barriers resolve by fission inside a per-block
  wrapper so blocks stay embarrassingly parallel, and `wave` maps to the host
  SIMD vector. The engine therefore writes kernels once and runs them on any
  backend including CPU.
- Every heap allocation and free registers into one process-global live-set
  table under a single mutex once a second thread exists
  (`runtime/native/cajeta_rt_core.c:867-869`), and the kernel pool makes the
  process multithreaded. Pooling does not remove that lock — nothing in this
  spec does. It removes the *allocations that would take it*: §4.3 allocates
  scratch once at load and reuses it, so a decode step performs no allocation
  and therefore never acquires the mutex. The lock stays untouched rather than
  contended, which makes "zero allocations per decode step" a load-bearing
  invariant rather than a tidiness goal.
- `--release` does **not** switch the live set off. `CompilerMode.h:225,239`
  set `LiveSet::Bounded` for both Release and Fast — still registered, still
  locked. Only `Minimal` (`:254`) sets `LiveSet::Off`, which is the one mode
  that removes the registration from codegen. A shipped inference build that
  assumes release-mode builds skip this is wrong.
- No `unsafe`, no pointer arithmetic, and no way to adopt a foreign address
  as a Cajeta buffer (`docs/specification/lang/MemoryModel.md:37`).

**1.5 Non-goals (v1).**
- No training, fine-tuning, or runtime LoRA application; adapters are merged
  offline.
- No codebook/lattice quantization (the `IQ*` i-quants). Those resolve weights
  through learned grid tables rather than bit-field extraction and are a
  different mechanism from the block quants in §10.
- No cross-process or cross-machine scheduling. In-process continuous
  batching, the paged cache and preemption are **in** v1 — this bullet
  originally excluded them and 14.3 reversed it (§13.13, §13.15).
  `specs/llm-kernel-scheduling-spec.md` still owns the multi-tenant
  orchestrator surface that sits above this engine.
- No concurrent multi-process access to the block store. One engine process
  owns it at a time; `LtmPager` has no locking (§13.17).
- No tensor/pipeline parallelism, no collectives, no multi-device.
- No HTTP serving surface; a CLI and a library API only.
- No MoE routing, no speculative decoding, no ONNX.
- No vision or multimodal input. Gemma 3 at 4B and above pairs the decoder
  with a SigLIP encoder; only the text path is in scope, and the 1B is
  text-only regardless.

## 2. Tensor placement and dtype

`Ewise.cajeta` already proves placement dispatch at 14 call sites
(`arithF32Op:54`, `matmulF32Op:360`, and siblings), but nothing calls it —
`Ewise` has two references repo-wide outside its own file and tests. Roughly
200 `Tensor` ops ignore placement entirely. This section gives the op surface
one placement policy and unpins f32; everything else in this spec depends on
it.

What "one policy" can mean is bounded by the language. A kernel launch needs a
concrete element type, so `Tensor`'s dtype-generic ops cannot dispatch to
kernels at all (2.1). The policy they share is therefore the *guard*, not the
dispatch: every op asks the same question about residency, and answers it by
taking the kernel path (concrete dtype), rejecting (split operands), or tiering
to the host with a note (generic, or no kernel for the dtype). The failure this
prevents is the one worth naming: an op that reads `Storage`'s stale host
mirror while the live data sits on the device, and returns a wrong answer
quietly.

Requirements:

- Placement is decided in one place, not hand-written per op.
- No op reads element data without first establishing where that data lives.
- Tensor ops are generic over floating dtype rather than pinned to f32.
- Host/device coherence has a defined model instead of a convention.

Use cases:

- **2.1** When every operand of an op with a kernel for its dtype is
  device-resident, the op executes as an XPU kernel and its result is
  device-resident. The kernel path is reached through dtype-specific entry
  points (`Ewise.arithF32Op`, `Ewise.matmulF32Op`); a dtype-generic op cannot
  take it, because kernel-name resolution needs a concrete element type and a
  launch from a body parameterized on `E` does not compile (`XPU-N02`). Generic
  ops therefore guard placement and tier (2.7). This is a language limit, not a
  policy choice: it would lift if a concrete element type could be carried
  across a generic boundary.
- **2.2** When an op's operands are split across host and device, it raises a
  located error naming both operands and their residency. It does not
  silently migrate and does not silently download.
  `docs/specification/nucleo/torch-facade-spec.md:3.1.2-3.1.3` already decided
  the direction — device is type-level and a mismatch is a compile error,
  "never a 2 a.m. runtime stack trace." Placement is a runtime flag on
  `Storage` today, so v1 delivers the runtime approximation; silent migration
  would become a semantic consumers depend on and would have to be broken
  when device becomes a type parameter.
- **2.3** When a device-resident tensor is read through a host accessor
  (`get`, `flatGet`, `getAt`), the read faults with a diagnostic rather than
  returning stale host bytes — `Storage.get`/`set` read `host`
  unconditionally today (`runtime/src/cajeta/math/Storage.cajeta:41,46`).
- **2.4** When a tensor moves to the device, its host mirror is released, so
  device residency lowers host RSS. `Storage` keeps both buffers for the
  lifetime of the tensor today.
- **2.5** When a `Tensor<float16>` or `Tensor<bfloat16>` is constructed,
  elementwise ops, matmul, and reductions run at that dtype without widening
  to f32. No half-precision tensor is instantiated anywhere in the repo
  today.
- **2.6** When a GEMM's M, N, or K is not a multiple of 16, the
  cooperative-matrix path handles the tail. The original claim here — that
  `Ewise.cajeta:355` *rejects* such shapes — was wrong, found 2026-08-08 while
  writing the test: it did not reject, it truncated all three dimensions to the
  tile grid and returned a wrong matrix silently. A 17×17×17 GEMM computed
  16×16×16. That is the more serious defect, and it is why this use case is a
  correctness gate rather than a coverage gap.
- **2.7** When no kernel exists for an op at a given dtype, a tiering note
  naming op and dtype is emitted, device-resident operands are brought back to
  the host, and the CPU path runs — mirroring the existing
  `note: [mma-tiering]` convention rather than failing or silently degrading.
  The note is emitted at **runtime**, not by the compiler: which ops tier
  depends on the residency an operand has at the call, which is not knowable
  when the module is compiled. It names op and dtype but not backend — a
  runtime note has exactly one backend in play.
- **2.8** When `argmax`/`argmin` are called with an axis, they reduce along
  that axis; today only a global flat-index form exists
  (`Tensor.cajeta:4125,4098`), which the sampler needs.
- **2.9** When a tensor is host-resident, its behavior and numerics are
  unchanged by this work. The numpy-oracle suite is the regression gate: the
  host path is what every existing consumer runs, and only six call sites
  repo-wide (`FftGpu`, `GeneratorGpu`, two in the ecosystem) use placement at
  all. GPU codegen stays opt-in per `--xpu-backend`
  (`src/cajeta/compile/Compiler.h:182`), so the default host-only build gains
  no compile time and no binary size.
- **2.10** When a tiering note is emitted (2.7), it is emitted once per
  `(op, dtype)` combination, not per call — otherwise a tiered op inside a
  decode loop reports once per layer per token. The dedupe is scoped to the
  compiled module rather than the process, so a fresh compilation starts clean
  and a test observes its own note instead of one an earlier run consumed.

## 3. Weight loading and large-file correctness

Requirements: multi-gigabyte checkpoints load without a full host copy, and
64-bit lengths survive the I/O path.

Use cases:

- **3.1** When a checkpoint larger than available RAM is opened, its tensors
  are memory-mapped and paged on demand. `Safetensors.cajeta:31` calls
  `File.readAllBytes` today, and the stdlib exposes no file mapping.
- **3.2** When a read or an allocation length exceeds 2^31, it is honored.
  `File.read` casts int64 to int32 at
  `src/cajeta/asn/expression/MethodCallExpression.cpp:8388`, where bit 31
  set becomes negative and returns 0 — indistinguishable from EOF — and
  `Storage.cajeta:32` allocates `heap T[(int32) length]`.
- **3.3** When an F16 or BF16 tensor is read, it retains its stored dtype;
  `Safetensors.cajeta:174,184` widen both to f32 today, doubling resident
  size.
- **3.4** When a checkpoint is sharded across files with an index JSON, all
  shards resolve as one logical state dict.
- **3.5** When a tensor's stored dtype has no loader, the error names both
  the tensor and the dtype.
- **3.6** When weights load for device execution, they upload without a full
  host materialization of the model.
- **3.7** When `File.writeAllBytes` receives a struct field or array element
  as its data argument, the correct bytes are written. `loadArrayDataPtr`
  handles only `AllocaInst` today
  (`MethodCallExpression.cpp:3400-3408`), so a GEP argument passes the slot
  address; the equivalent fix already landed for String arguments at
  `MethodCallExpression.cpp:386`.
- **3.8** When a path is built at runtime by concatenation or substring, it
  is NUL-terminated before reaching the native layer.

## 4. Model definition and the tape-free forward path

Use cases:

- **4.1** When weights bind to a model, parameter names follow torch's
  dotted `state_dict` convention, so a Hugging Face checkpoint loads without
  a bespoke rename table — reusing `nn/Module.cajeta:29-32` and
  `io/Checkpoints.cajeta` (`torchToCajeta`, `transposeLinearWeights`).
- **4.2** When a forward pass runs, no autograd tape is allocated and no
  intermediate outlives its last use; peak activation memory is O(one layer),
  not O(all activations). `Module.predict` builds a full tape today
  (`nn/Module.cajeta:117`).
- **4.3** When a decoder layer completes, its scratch buffers are returned to
  a reusable pool rather than freed and reallocated per layer — every
  allocation contends the global live-set mutex (§1.4).
- **4.4** When a model directory is opened, the decoder stack is constructed
  from `config.json` (HF) or from GGUF metadata.
- **4.5** When an architecture is not supported, the error lists the ones
  that are.
- **4.6** When a model is loaded onto a device, all its parameters are
  device-resident before the first forward pass.
- **4.9** When prefill runs, its scratch is carved from a single block sized to
  the **actual** sequence length, not from a pool sized for maximum context.
  Buffers are non-owning slices of that block — no copy, one allocation per
  prefill rather than one per intermediate.
- **4.10** When a scratch slice is taken, it borrows: the parent owns the
  storage and the slice never frees it. Host-side this needs a borrowing
  `Storage` constructor — `Storage` has exactly one constructor today and it
  unconditionally does `heap T[(int32) length]` (`Storage.cajeta:29-34`).
  Device-side `KernelBuffer.slice(offset, count)` already implements precisely
  this, with an `owned` flag so the drop chain cannot free a parent through a
  view; the host mechanism mirrors it rather than inventing a second model.
- **4.8** When a config declares `head_dim` explicitly, it is used rather than
  inferred as `hidden_size / n_heads`. Gemma 3 declares 256, which does not
  equal that quotient, and Llama 3.2 declares it too.
- **4.11** When a checkpoint's architecture is a **Qwen2 family** decoder
  (`qwen2`, `qwen2vl`), it loads and runs through the same decoder stack as
  Llama: RMSNorm, GQA, SwiGLU MLP, rotary embeddings. The family differs in
  three things and nothing else the text path touches — a per-architecture
  GGUF key prefix (§9.6), biases on the q/k/v projections (4.12), and
  multi-section rotary metadata that a text-only sequence collapses to
  ordinary RoPE (4.13).
- **4.12** When an attention projection carries a **bias** tensor
  (`attn_q.bias`, `attn_k.bias`, `attn_v.bias`), it is added after the
  projection, on both the host path and every device route. A model whose
  biases are silently dropped still produces fluent text, so this is checked
  against reference logits, never by reading output.
- **4.13** When rotary metadata declares `rope.dimension_sections`
  (Qwen2-VL's M-RoPE), a **text-only** sequence uses the ordinary rotary
  path: the sections select which of the (t, h, w) position components feeds
  each frequency band, and for text all three are the token position, so the
  rotation is identical. Image and video positions are out of scope for the
  text path and the loader says so rather than pretending.

- **4.7** When a config declares `tie_word_embeddings`, the output projection
  reuses the input embedding matrix rather than expecting a separate tensor.
  Llama 3.2 1B/3B and several Qwen sizes tie, and the 1B is the natural parity
  fixture, so this is load-bearing from the first model.

## 5. Transformer primitives

None of these exist in `cajeta-ml` today; the shipped
`TransformerDecoderLayer` is post-norm with ReLU and dense MHA, which cannot
express a Llama-family decoder.

**Attention configuration is per layer, not per model.** A layer carries its
own attention window (finite or infinite), RoPE base and scaling, and
`n_kv_heads`; the model config supplies defaults that layers inherit. Uniform
models (Llama, Qwen) express as every layer sharing the default, so there is
no special case. This is not speculative generality: Mistral 7B v0.1 windows
every layer at 4096, and the local/global alternation in Gemma 3, Gemma 4 and
GPT-OSS varies both the window *and* the RoPE base across layers (10k local,
1M global). A model-level `sliding_window` field would have to be torn out to
support any of them.

Use cases:

- **5.1** When RMSNorm is applied, it normalizes by root-mean-square over the
  last axis with a learned scale and no mean subtraction.
- **5.2** When rotary position embeddings are applied at position `p`, query
  and key head dimensions rotate pairwise against **that layer's** theta base.
- **5.3** When a layer declares RoPE scaling — linear, NTK, YaRN, or the
  `llama3` type used by Llama 3.1 and later — the declared scaling is applied.
- **5.4** When the gated MLP runs, it applies the config's declared
  activation: `down(silu(gate(x)) * up(x))` for Llama, Mistral and Qwen
  (SwiGLU), and the `gelu_pytorch_tanh` form for Gemma 3 (GeGLU).
- **5.5** When `n_kv_heads < n_heads`, K and V heads are shared across query
  groups without materializing repeated K/V tensors.
- **5.6** When attention runs during prefill, a causal mask is applied; when
  it runs during single-token decode, no mask is constructed.
- **5.10** When a model declares QK-norm, RMSNorm is applied to the query and
  key projections before RoPE. Gemma 3 uses this in place of Gemma 2's
  attention and final logit soft-capping, which it removed — implementing
  soft-capping for Gemma 3 would be wrong.
- **5.11** When a block declares post-norms as well as pre-norms, all four
  RMSNorms per block are applied — input, post-attention, pre-feedforward and
  post-feedforward. Llama-shaped models declare two; Gemma 3 declares four.
- **5.12** When Gemma-style RMSNorm runs, its weights are zero-centered and
  scale by `(1 + weight)` rather than by `weight`. Applying the Llama form to
  Gemma weights produces plausible, wrong output rather than an error.
- **5.13** When a model declares a query scale, attention uses it instead of
  the default `1/sqrt(head_dim)`.
- **5.9** When a layer declares a finite attention window `w`, query position
  `i` attends only to key positions `j` where `i - j < w`, in **both** prefill
  and decode. An implementation that windows prefill but not decode produces
  plausible, subtly wrong output only past `w` tokens — it passes every
  short-prompt test — so §12 gates it with a prompt longer than the window.
- **5.7** When attention softmax runs, accumulation is f32 regardless of the
  tensor's storage dtype.
- **5.8** When attention runs, scores are never materialized for the full
  `(heads, T, T)` at once beyond a bounded tile budget.

## 6. KV cache, scheduling, and the block store

The cache, the scheduler that allocates it, and the store that evicted blocks
fall into are one subsystem: 13.13 makes the cache a shared pool, which means
someone must decide who gets blocks, and 13.16 makes eviction a write rather
than a discard. They are specified together because they cannot be designed
apart. 6.1–6.18 are the cache, 6.19–6.25 the scheduler, 6.26–6.32 the store.

Use cases:

- **6.1** When a prompt is prefilled, K and V for every layer and position
  are computed once and written into the cache.
- **6.2** When a token is decoded, only the new position's K and V are
  computed and appended; earlier positions are read from the cache.
- **6.3** When the cache is allocated, unwindowed layers draw from a pool of
  fixed-size blocks shared by every sequence, and windowed layers keep a
  per-sequence ring sized to the window (§13.13). One block holds
  `2 × n_kv_heads × head_dim × block_tokens` at the model's dtype. The pool is
  allocated once and is device-resident when the model is. Sizing stays per
  layer because that is where the saving lives: Mistral v0.1 declares
  `max_position_embeddings: 32768` against a 4096 window, so windowed layers
  need an eighth of the entries a uniform cache would allocate.
- **6.4** When no free block remains, the scheduler declines to admit a
  waiting sequence or preempts a running one (§13.15); it does not fail the
  call. A sequence exceeding an unwindowed layer's configured maximum context
  still fails with an explicit error. Windowed layers never fail — they evict
  by wrapping (6.8).
- **6.8** When a layer is windowed, its cache is a ring buffer: absolute
  position `p` occupies slot `p mod w`, and writing position `p` overwrites
  `p - w`, which is out of the window by construction.
- **6.9** When decode reads a windowed layer's cache, it reads the live range
  as **two** contiguous segments when the window has wrapped, and one when it
  has not.
- **6.10** When RoPE is applied to a windowed layer, it uses the **absolute**
  position `p`, never the ring slot `p mod w`. Rotating by the slot index is
  the defining bug of this design and is silent.
- **6.11** When a prompt longer than a layer's window is prefilled, cache
  writes for that layer stay bounded by the window — early positions are
  overwritten as prefill proceeds rather than requiring full-length storage.
- **6.5** When decode runs, attention reads the cache in place — the cache
  is never copied per step.
- **6.6** When a generation completes, the cache resets for reuse without
  reallocation.
- **6.7** When a cache is created for a model on a device, its allocation is
  the block pool plus the per-sequence windowed rings, never per-step
  allocations.
- **6.12** When a sequence is admitted, it receives a block table — an ordered
  list of block indices per layer — and blocks are appended as generation
  crosses block boundaries.
- **6.13** When attention runs over a batch, it reads through each sequence's
  block table using cumulative-sequence-length indexing, so sequences of
  different lengths share one launch without padding to the longest.
- **6.14** When two sequences share a prefix, they share the blocks covering
  it rather than each holding a copy. A block is freed when the last sequence
  referencing it releases it.
- **6.15** When a shared block would be written, it is copied first, so one
  sequence's continuation never mutates another sequence's history. This is
  the defining bug of block sharing and it is silent — the corrupted sequence
  simply produces plausible wrong tokens.
- **6.16** When a sequence is preempted, its blocks are written to the store
  (§13.16) and returned to the pool.
- **6.17** When a preempted sequence resumes, its blocks are read back into
  freshly allocated pool blocks and its block table is rebuilt. Block indices
  are not stable across a preemption.
- **6.18** When a request arrives, the longest cached prefix is looked up in
  the store and its blocks are adopted rather than recomputed; only the
  uncached suffix is prefilled.

- **6.19** When a sequence waits for admission, it is admitted once enough
  free blocks exist for its uncached suffix plus a decode headroom margin —
  admitting a sequence that immediately preempts something is churn, not
  progress.
- **6.20** When a step runs, the batch is assembled from whatever is runnable
  at that moment — some sequences on a prefill chunk, some on a decode token —
  rather than draining one batch before the next is formed.
- **6.21** When a long prompt is prefilled, its chunks are scheduled alongside
  in-flight decodes, so no decode waits for the whole prompt (§13.14).
- **6.22** When free blocks are exhausted, the scheduler preempts a running
  sequence by a stated policy and its blocks are written to the store (6.16).
- **6.23** When blocks free, preempted sequences resume in an order that
  cannot starve any one of them indefinitely.
- **6.24** When a sequence completes, its blocks are released and a waiting
  sequence may be admitted within the same step.
- **6.25** When scheduling runs, its decisions are observable — admitted,
  preempted, resumed and queued counts are exposed for §12.16.

- **6.26** When a block is stored, its key is a hash chained from the previous
  block's key, so a key identifies the whole prefix ending at that block
  rather than only its own tokens.
- **6.27** When a key is computed, it incorporates model identity, dtype and
  quantization. Serving one model's blocks to another would be silent and
  catastrophic, and a bare token hash would do exactly that.
- **6.28** When a token sequence is looked up, the store returns the longest
  cached prefix as a block list, walking block-aligned keys in order until a
  miss.
- **6.29** When the process restarts, index and data files reopen and
  previously stored prefixes are still served (§12.13).
- **6.30** When the store exceeds its configured size, the coldest entries are
  evicted by walking the index in order — the ordered-traversal requirement
  that §13.17 adds to `LtmBPlusTree`.
- **6.31** When a stored block is read back, a torn or partial write is
  detected and treated as a miss. Corrupt bytes are never served as cache.
- **6.32** When the store is disabled by configuration, preemption falls back
  to recompute-on-resume and every other behaviour is unchanged. The store is
  a performance tier, not a correctness dependency.

## 7. Tokenizer

Both target tokenizer families are BPE at the merge step; they differ in
pre-tokenization and in how unknown bytes are handled. The tokenizer is
therefore built as a **component pipeline** — normalizer, pre-tokenizer,
model, decoder — rather than as two parallel implementations. Llama 3.x and
Qwen2/3 are ByteLevel + BPE; Llama 2 and Mistral (v0.1–v0.3) are
SentencePiece, which in this pipeline is Metaspace + BPE + byte fallback.
The merge loop is shared, so the second family costs the components, not a
second stack.

Three vocabulary sources feed the same pipeline. GGUF normalizes the
vocabulary into metadata (`tokenizer.ggml.model`, `.tokens`, `.scores`,
`.merges`, `.token_type`, `.pre`). Hugging Face repos ship a converted
`tokenizer.json` declaring the components explicitly. And a repo that ships
only SentencePiece's raw `tokenizer.model` is readable too, via
`dev.cajeta.codec.protobuf` — so no repo layout is excluded.

Use cases:

- **7.1** When a Hugging Face `tokenizer.json` is loaded, its declared
  normalizer, pre-tokenizer, model (vocab + merges), decoder, added tokens,
  and special tokens parse.
- **7.2** When text is encoded, the token ids match the reference tokenizer
  exactly across a fixture corpus, for both families.
- **7.3** When the pre-tokenizer is ByteLevel, bytes map through the GPT-2
  byte alphabet before merging.
- **7.4** When the pre-tokenizer is Metaspace, spaces become `U+2581` and a
  dummy prefix is prepended when the tokenizer declares it.
- **7.5** When a piece is absent from the vocabulary and the tokenizer
  declares byte fallback, the bytes encode as `<0xXX>` tokens.
- **7.6** When a GGUF file embeds a tokenizer, `tokenizer.ggml.model` and
  `tokenizer.ggml.pre` select the pipeline components and the vocabulary comes
  from the GGUF arrays — no external file and no protobuf.
- **7.7** When tokens decode, they round-trip to the original bytes for both
  families, including sequences that are not valid UTF-8.
- **7.8** When a JSON string is read, a decoded-string accessor returns its
  represented characters, including `\uXXXX`. This is **additive**: verbatim
  escapes are a deliberate codec-wide design supporting zero-copy byte
  comparison, documented at `JsonValue.cajeta:125` and
  `JsonIndex.cajeta:28,152,355` ("a key with JSON escapes won't match a
  literal"), not an oversight. `currentBytes()` semantics are unchanged and
  changing them is out of scope.
- **7.9** When a tokenizer declares a model type outside the supported set,
  the error names the type. Unigram (UGM) and WordPiece (WPM) are out of
  scope — no target model uses them.
- **7.10** When a repo ships only a raw SentencePiece `tokenizer.model`, its
  `ModelProto` is decoded with `dev.cajeta.codec.protobuf` — iterating the
  repeated `pieces` field for each piece's string, score, and type. Use
  `ProtobufCursor` (`fieldCount`/`fieldNumberAt` for repeated fields), not the
  typed `Protobuf.parse<T>` facade, which is a documented stub until the
  cajeta-two `ProtobufSynthesizer` lands (`Protobuf.cajeta:44-50`).
- **7.11** When a chat model is prompted, the prompt is rendered by
  interpreting the model's own `chat_template` — from `tokenizer_config.json`
  for HF repos, from `tokenizer.chat_template` for GGUF (§13.18).
- **7.12** When a template uses a construct outside the supported Jinja
  subset, the error names the construct and the template line, rather than
  rendering something subtly wrong.
- **7.13** When a caller has already rendered a prompt, it can be passed
  directly and the template is not consulted. This entry point is permanent
  API surface, not a v1 stopgap.
- **7.14** When a template renders, its output matches `transformers`'
  `apply_chat_template` byte for byte on the fixture corpus, across all four
  target families and including a tool-call template.

## 8. Sampling and generation

Use cases:

- **8.1** When temperature is zero, the next token is the argmax over the
  vocabulary axis.
- **8.2** When temperature is non-zero, logits divide by it before softmax.
- **8.3** When top-k is set, sampling is restricted to the k highest logits.
- **8.4** When top-p is set, sampling is restricted to the smallest set whose
  cumulative probability reaches p.
- **8.5** When a repetition penalty is set, previously emitted tokens have
  their logits penalized.
- **8.6** When the sampler is given **fixed logits** and a fixed seed, it
  produces the same token selection every time. This is asserted at the
  sampler, not end to end: it tests the RNG and the top-k/top-p/penalty
  arithmetic, which are reproducible, without depending on the model's
  floating-point output, which is not (§12 preamble).
- **8.7** When an EOS token, a stop string, or the token budget is reached,
  generation halts and reports which condition fired.
- **8.8** When generation runs, each token is delivered through a streaming
  callback before the next step begins.

## 9. GGUF

Use cases:

- **9.1** When a GGUF file is opened, its magic, version, tensor count,
  metadata key-value block, and tensor directory parse.
- **9.2** When a GGUF tensor carries any type in §10's table, it loads.
- **9.5** When a file's tensors carry **different** quantization types, each is
  decoded by its own type. Names like `Q4_K_M` and `Q2_K_S` are
  `llama-quantize` *recipes*, not file formats: the ggml type is recorded
  per tensor, and the tool assigns sensitive tensors a higher-precision type
  than the recipe's headline name. Type dispatch is therefore per tensor,
  never per file.
- **9.3** When GGUF metadata declares architecture and hyperparameters, they
  populate the model config without a separate `config.json`.
- **9.4** When a GGUF quantization type is not supported, the error names the
  type and the tensor.
- **9.6** When GGUF metadata is read, hyperparameter keys are looked up under
  the **file's own** `general.architecture` prefix (`llama.*`, `qwen2vl.*`),
  not a hard-coded one. The prefix is data in the file; treating it as a
  constant is what makes a second architecture look like a rewrite.

## 10. Quantized execution

Sub-byte weights are a **storage** format, not an arithmetic one. Every format
below is unpacked by bit-field extraction into int8 or f16 and multiplied
there, exactly as the reference implementation does — nothing multiplies two
4-bit or 2-bit values. No new scalar type is required, and adding one would
not help: LLVM rounds array element store size up to a byte, so an `int4[]`
would occupy one byte per element and lose the packing that is the entire
point (only vectors like `<8 x i4>` bit-pack).

**The whole k-quant family is required, not just the headline types.** A file
built as `Q4_K_M` stores `output.weight` and other quality-sensitive tensors at
`Q6_K`, some at `Q5_K`, and the rest at `Q4_K`; a `Q2_K` build likewise mixes
in higher-precision types for sensitive tensors. `llama-quantize` has a
`--pure` flag precisely because mixing is the default, and distributed
community weights are never `--pure`. Supporting `Q4_K` alone would therefore
fail to load a single real `Q4_K_M` file. Scope is the family:

| Format | Block | Weight bits | Per-block metadata |
|---|---|---|---|
| Q8_0 | 32 | 8 | f16 scale |
| Q4_0 | 32 | 4 | f16 scale |
| Q5_0 | 32 | 5 | f16 scale, 32 high bits in a u32 |
| Q6_K | 256 super-block, 16 × 16 | 6 | 8-bit scales, f16 `d` |
| Q5_K | 256 super-block, 8 × 32 | 5 | 6-bit scales + mins, f16 `d` and `dmin` |
| Q4_K | 256 super-block, 8 × 32 | 4 | 6-bit scales + mins, f16 `d` and `dmin` |
| Q3_K | 256 super-block, 16 × 16 | 3 | 6-bit scales, f16 `d` |
| Q2_K | 256 super-block, 16 × 16 | 2 | 4-bit scales + mins, f16 `d` and `dmin` |

Use cases:

- **10.1** When a Q8_0 tensor participates in a GEMM, it is consumed as int8
  with its block scale rather than dequantized to a full f32 tensor.
- **10.2** When a tensor in any listed block format participates in a GEMM, its
  weights are unpacked by shift-and-mask into int8 or f16, scaled by the block
  scale (and offset by the block min where the format carries one), and
  multiplied at that width.
- **10.3** When a k-quant is unpacked, its sub-block scales and mins are
  themselves unpacked from their packed 4-, 6-, or 8-bit fields.
- **10.4** When the backend exposes an int8 cooperative-matrix configuration,
  the GEMM uses it — `v_wmma_i32_16x16x16_iu8` on RDNA3, the SPIR-V
  cooperative-matrix integer path on Vulkan.
- **10.5** When no matrix-core integer path exists, the GEMM uses DP4a via
  `idotWiden`, and a portable widen-multiply-reduce otherwise.
- **10.6** When a quantized GEMM selects a tier, a note records which one.
- **10.7** When unpacking runs, it uses a vectorized path where the SIMD
  surface supports it and a scalar shift-and-mask path otherwise; both produce
  identical results. Correctness does not wait on the SIMD work.
- **10.8** When a dequantized tensor is compared against the reference
  implementation's dequantization of the same block, the values match exactly.
- **10.9** When an unsupported quantization type is requested, the error names
  the type rather than failing obscurely.
- **10.10** When weight bits do not divide a byte evenly (Q3_K at 3 bits,
  Q5_K at 5, Q6_K at 6), the extraction spans byte boundaries correctly.
- **10.11** When a legacy (non-K) block format appears **inside** a k-quant
  file, it decodes by its own type like any other (9.5). Real recipes mix
  them: Qwen2.5-VL-72B-Instruct-Q4_K_L carries `ffn_down` as Q8_0 on half its
  layers and Q5_0 on the other half, beside Q4_K, Q5_K and Q6_K elsewhere.

- **10.12** When a batched (prefill) GEMM runs on a matrix-core backend,
  **every** supported quantization takes it — not a privileged subset. The
  f16 cooperative-matrix kernel's compute half is format-blind, so a format
  contributes only an unpack; a format whose ggml block stride is not a
  multiple of four is repacked once, on device, into a dword-addressable
  stride before its first GEMM.
- **10.13** When a weight tile is staged for a cooperative-matrix GEMM, the
  accumulator count per subgroup is chosen so the kernel does not spill.
  A tile that spills moves accumulator state through memory on every
  k-iteration and costs far more than the arithmetic intensity a larger
  tile buys — measured 2026-08-26 at 55-70% of the kernel's throughput on
  gfx1151, invisible to wall-clock A/B and named immediately by the
  shader's register statistics.

## 11. Surface

Use cases:

- **11.1** When `cajeta-llm run --model <path> --prompt <text>` is invoked,
  it loads, generates, and streams to stdout.
- **11.2** When `--device` names a backend, execution uses it; when it is
  absent, the runtime dispatcher's order applies (CUDA, HIP, Vulkan, CPU).
- **11.3** When `--ctx`, `-n`, `--temp`, `--top-k`, `--top-p`, `--seed`, or
  `--repeat-penalty` are supplied, they configure the corresponding behavior
  in §6 and §8.
- **11.4** When the engine is used as a library, model loading, generation,
  and the token callback are reachable without the CLI.
- **11.5** When `--max-seqs`, `--block-size` or `--chunk` are supplied, they
  configure the scheduler and cache of §6.19–6.25.
- **11.6** When `--kv-store <path>` is supplied, the block store is enabled at
  that path; `--kv-store-size` bounds it and `--no-kv-store` disables it,
  falling back to recompute-on-resume (§6.32).
- **11.7** When the library submits more than one request concurrently, they
  are batched by the same scheduler the CLI uses — the CLI is one caller of
  the batching API, not a separate single-sequence path.

### 11.8 Diagnostics are records pushed to a registered callback

*Added 2026-08-30 (Julian): diagnostics as "a collection of records", with
"the consumer can decide how to publish those or use them"; then, rather
than caching them, "require a call-back as part of registration with the
host … and not have to cache them until a chat response goes out".*

`Diag` today pushes formatted TEXT into a global sink. That cannot
attribute a line to a session, so with several sequences in flight the
stream is uncorrelated — the case a host serves.

- **11.8.1** When a host registers with the engine, it may supply a
  diagnostics callback. With none supplied the engine records nothing.
- **11.8.2** When the engine takes a routing or scheduling decision worth
  observing, it builds a structured record — category, session, typed
  fields — and hands it to the callback. It does not format a string, and
  it does not store the record.
- **11.8.3** The engine holds NO buffer. Buffering is a consumer policy —
  how much history, per session or global, what to drop — and belongs
  where that policy is known. A consumer wanting retrospective queries
  keeps its own ring.
- **11.8.4** The callback runs on the ENGINE's thread, mid-generation. It
  must be cheap: take the record and return. Formatting, I/O and
  forwarding belong on the consumer's own thread, or they become latency
  on every token.
- **11.8.5** When a callback throws, the turn survives. A consumer's
  logging fault is not a generation fault.
- **11.8.6** The engine chooses no logging backend. It emits records; a
  consumer that wants them in a log logs them.
- **11.8.7** Records carry the session they belong to, so a consumer can
  attribute, filter, or forward a session's diagnostics to the client that
  owns it.

## 12. Correctness and performance gates

**No gate asserts bitwise reproducibility.** Greedy decoding is deterministic
in exact arithmetic, but floating-point reduction order varies with batch
shape, kernel selection, atomics ordering, and backend, so identical output
across runs or across backends is not a property the implementation can
promise. Gates compare against a reference **within tolerance**, and compare
token sequences by agreement rate rather than by equality. Determinism
assertions must not be reintroduced.

Use cases:

- **12.1** When a supported model runs a fixed prompt on the **unquantized**
  f16/bf16 path, its final-layer logits match a Hugging Face `transformers`
  reference running at fp32 on CPU, at a pinned model revision, on all three
  detectors of §13.21 — cosine similarity, top-1 agreement, and softmax KL.
  The numeric bar is derived in Unit 11 (§13.21).
- **12.2** When the tokenizer encodes the fixture corpus, output matches the
  reference exactly.
- **12.3** When temperature is zero, repeated runs agree with each other on at
  least **99%** of tokens, and every divergence sits at a position where the
  top two logits are within **1e-3**. Both clauses are required: the rate
  alone would pass a systematic bug that flips one token in two hundred at
  non-tie positions (§13.22).
- **12.4** When the same model runs on two backends, logits agree within
  tolerance and greedy output meets the same agreement rate.
- **12.5** When a model is loaded and freed repeatedly, resident memory
  returns to baseline.
- **12.6** When a windowed model runs a prompt longer than its window, output
  matches the reference. This gates §5.9 and §6.8–6.11: every windowing bug is
  silent under short prompts, so no shorter fixture can catch them.
- **12.7** When Gemma 3 and a Llama-shaped model both run, each matches its own
  reference — the same code path serves a four-RMSNorm windowed 5:1 stack and
  a two-RMSNorm uniform one.
- **12.8** When decode throughput is measured on a given model, quantization
  and backend, it reaches **60%** of `llama.cpp`'s throughput on the same
  hardware and configuration (§13.20). Batch 1 and a representative batched
  shape are both measured — 13.13 makes batch 1 one operating point rather
  than the only one that runs.
- **12.9** When prefill throughput is measured under the same conditions, it
  reaches **50%** of `llama.cpp`'s. It is measured at the chunk size the
  scheduler actually uses (§13.14), not as an unbounded single pass.
- **12.10** When throughput is reported, the measurement records model,
  quantization, backend, context length, batch size, **compiler mode and
  live-set setting**. Release and Minimal differ in both locking and bounds
  checking, so a number without them is not comparable — and that is the axis
  along which a benchmark most easily flatters itself.
- **12.11** When resident memory is measured for a loaded model, it is within
  **1.25×** of `llama.cpp`'s for the same model and quantization — throughput
  parity bought with several times the memory is not parity.
- **12.12** When a request is served from cached prefix blocks, its logits are
  identical to the same request computed from cold, within §12.1's tolerance.
  A prefix cache that returns subtly wrong blocks is worse than no cache, and
  the failure is invisible without this gate.
- **12.13** When the process restarts, the block store reopens and previously
  cached prefixes are still served. A store whose durability is never
  exercised across a restart is not durable.
- **12.14** When a sequence is preempted mid-generation and later resumed, its
  output matches an uninterrupted run of the same request. This gates §13.15
  and §13.16 together: eviction, store round-trip, and resume.
- **12.15** When a quantized model runs, its perplexity on a fixture corpus is
  within a stated delta of the same model unquantized — §12.1's logit
  tolerance does not apply to quantized weights and must not be asserted
  against them (§13.21).
- **12.16** When throughput is reported for a run that preempted, the report
  records eviction count, bytes written to the store, and cache hit rate
  alongside the §12.10 fields. Throughput measured on a warm prefix cache is
  not comparable to a cold one, and SSD endurance is rate-dependent (§13.16).

## 13. Decisions

All eight opened questions were resolved 2026-08-08. Two resolutions reversed
the recommendation they were filed with; both are marked.

- **13.1 Mixed host/device operands — reject, do not migrate.** *(Reverses the
  filed recommendation.)* `torch-facade-spec.md` §3.1.2–3.1.3 already decided
  that device is type-level and a mismatch is a compile error. Silent
  migration would contradict that and would become a semantic consumers rely
  on, which then has to be broken when device becomes a type parameter.
  Placement is a runtime flag today, so §2.2 delivers a located runtime error
  as the closest available approximation. Making device a type parameter is
  out of scope here and belongs to the façade/roadmap track.
- **13.2 Placement dispatch lands in stdlib `cajeta.math`,** not an
  inference-private path.
  Measured 2026-08-08: only six call sites repo-wide use placement, GPU
  codegen is opt-in at build time, and dispatch costs one branch per op —
  so the host path is untouched (2.9). The two existing consumers already
  follow the stricter residency discipline their docstrings state, and both
  currently sit on a silent stale-host read that 2.3 converts to a fault.
  Residual cost: `FftGpu.cajeta:70,93` round-trips `cpu()`/`gpu()` and gains
  one host reallocation.
- **13.3 New repo `cajeta-llm`, namespace `dev.cajeta.llm`.** Matches
  `cajeta-ml`/`dev.cajeta.ml` and `cajeta-http`/`dev.cajeta.http`, and matches
  the namespace policy: `cajeta.*` is the embedded stdlib and is never
  published; `dev.cajeta.*` is every external published library. The stdlib
  units (plan Units 1–4) stay in the `cajeta` repo; the engine (Units 5–14)
  lives in the new one. The plan therefore spans two repositories.
- **13.4 An owning `MappedFile` type, not a bare `File.map()` view.** The
  mapping must be unmapped at a defined point and the language has no `unsafe`
  and no foreign-buffer adoption (§1.4), so the mapping needs an owner with a
  drop. `MappedFile` owns the mapping and lends a borrowed byte view,
  following `KernelBuffer`'s RAII precedent — constructor allocates, scope
  exit releases.
- **13.5 Q4_0, Q4_K, and Q2_K are in v1 (revised 2026-08-08).** The original
  deferral was wrong and is withdrawn: it conflated *storage* support with
  *arithmetic* support. Sub-byte quantized weights are never multiplied at
  their stored width — they are unpacked to int8 or f16 first — so no
  sub-byte scalar type is needed, and no LLVM change is needed either. LLVM
  has always had arbitrary-width integers, and Cajeta already constructs `i4`
  at `CajetaType.cpp:603`. What the fast path wants is SIMD `tableLookup`
  (pshufb) and `widen`/`narrow`, unchecked at `agents/cajeta/Simd-plan.md:142`
  and siblings — a SIMD gap, not a type-system gap. Correctness therefore
  ships on scalar shift-and-mask with no language change (§10.7), and the
  SIMD work is a throughput follow-on.
  Native int4 *matmul* stays out: `AmdgpuKernelLowering.cpp:829-948` wires
  only the iu8 path, and the reference implementation does not compute at
  4 bits anyway. The `IQ*` i-quants stay out (§1.5) — codebook resolution is
  a different mechanism. Roadmap §8.5 → `quantization-spec` remains the home
  for the wider PTQ/QAT surface; its autodiff dependency (§4.1) applies to
  QAT, not to inference, so plan Unit 13 does not wait on it.
- **13.6 Llama 3.x first.** Mistral and Qwen2/3 reuse the same decoder shape,
  so one architecture covers most of the target weights.
- **13.7 JSON escape handling is additive, and is not a defect.** Verbatim
  escapes are a deliberate codec-wide design supporting zero-copy byte
  comparison, documented at `JsonValue.cajeta:125` and
  `JsonIndex.cajeta:28,152,355`. Changing `currentBytes()` would alter
  key-matching semantics across every stdlib JSON consumer. §7.6 therefore
  adds a decoded-string accessor and changes no existing behavior, so it stays
  in this spec. **`JsonWriter` is separate and is a genuine defect** —
  `writeStringRaw` (`JsonWriter.cajeta:338-355`) escapes only `"` and `\`, so
  any byte in 0x00–0x1F produces invalid JSON. That gets its own defect spec
  (13.8).
- **13.8 The stdlib defects get their own specs, per house convention.**
  `instanceof-interface-lhs-spec.md` and siblings set the pattern: a defect
  found while implementing other work is filed as `<name>-spec.md` opening
  `# <name> — defect` / "Found <date> implementing <work> <unit>". Three
  qualify here and are prerequisites of this spec:
  `writeallbytes-field-arg` (the `AllocaInst`-only `loadArrayDataPtr` at
  `MethodCallExpression.cpp:3400-3408`, where the String path was already
  fixed at `:386`), `runtime-path-nul-termination`, and
  `jsonwriter-control-byte-escaping`. They remain as plan items (Units 4, 9)
  until those specs are authored, so nothing is lost in the interim.

- **13.9 Shipped builds use `--live-set=bounded`; `off` is a measurement tool,
  not a shipping configuration.** The flag is independent of compiler mode, so
  `--release --live-set=off` exists — but `CompilerMode.h:39` states the price
  plainly: "no tracking; aliased fields will double-free." That trades a
  memory-safety guard for an allocation-path speedup the decode loop does not
  benefit from once 13.10 holds. Use `off` to measure the lock's cost, never to
  ship.
- **13.10 The zero-allocation invariant is hard for the forward pass and the
  sampler, bounded at the text-emission boundary.** The distribution decides
  it: a 32-layer forward runs several hundred tensor ops per token, and those
  allocations land while the kernel-pool workers are live — peak count and
  peak contention. The sampler is one to three per token between launches, but
  its scratch is fixed-size once `k` is known, so pooling it is nearly free and
  it forecloses "someone added a buffer inside the loop" regressions.
  Detokenization emits one `String` per token by nature; forcing zero there
  means byte-buffer APIs that fight the language for no measurable gain.
  Keep this in proportion — it prevents giving throughput away, it does not
  create any. The dominant cost is bandwidth reading and unpacking quantized
  weights (§14.6).
- **13.11 Live-set sharding is not a dependency.** *(Resolves §14.5.)*
  "Sharding" here is lock striping within one process — splitting the single
  global table and mutex into K hash-indexed buckets so threads allocating
  unrelated pointers stop contending. It is unrelated to distribution across
  machines, which is out of scope (§1.5). Because 13.10 keeps the decode loop
  from acquiring the lock at all, striping it changes nothing on the hot path;
  it would only speed model load, which happens once. The parked work stays
  parked.

- **13.12 The no-copy prefill arena uses borrowed `Storage`/`KernelBuffer`
  slices, not `view`.** `view` was considered and rejected on inspection: a
  view cannot hold class references (`Views.md:29,36`), its `T[]` fields are
  `i32` length-prefixed byte runs rather than `{i64 count, data}` arrays, and
  element-array views are frame-arena-backed and may not escape the
  constructing frame (`CAJETA_ERROR_VIEW_ELEMENT_ARRAY_OWNING`). A view
  therefore cannot hold or yield a `Tensor`, and its `i32` prefix would cap a
  field at 2^31 elements. Views remain the right tool where their contract
  fits — GGUF header and metadata parsing (§9), which is exactly "field
  offsets determined by the type alone."
  Scope: the arena is for **prefill**, where buffer sizes track sequence
  length. Decode keeps the fixed pool of §4.3 — its per-token allocation is
  already zero, so carving one block instead of reusing five buys nothing.

- **13.13 Batched from the start, and the cache is paged.** *(Resolves §14.3.)*
  §6 assumed a single sequence without saying so. It serves many, and the KV
  cache is a pool of fixed-size blocks with a per-sequence block table rather
  than one contiguous buffer per layer. Contiguous per-sequence slots were
  rejected: every slot pays worst-case length, so eight slots against a 32k
  declared context allocate for 32k whether the sequences are 200 tokens or
  not — which is the fragmentation batching exists to avoid. Windowed layers
  keep the §6.8 ring buffer; their footprint is already bounded and constant,
  so paging them buys nothing and costs an indirection.
  Consequences: the attention kernel takes ragged batches and needs
  cumulative-sequence-length indexing, not a rectangular `[B, H, S, D]`
  layout; §6.3's sizing gains a sequence dimension; §6.8's ring becomes per
  layer *and* per sequence; and §12.8's batch-1 measurement becomes one gate
  among several rather than the only shape that runs.

- **13.14 Prefill is always chunked, and decode is the same path at N=1.**
  *(Resolves §14.7.)* A prefill that runs as one indivisible pass
  head-of-line-blocks every in-flight decode, which defeats 13.15's
  iteration-level scheduling; chunking is what lets them interleave. It also
  collapses the design to a single "process N tokens for sequence S" step —
  prefill is N=chunk, decode is N=1 — instead of two code paths that must be
  kept consistent, and it is the same mechanism §6.11 already requires to keep
  windowed prefill inside the window. A threshold-triggered hybrid was
  rejected: it leaves the rarely-taken path untested, and that path is exactly
  where §6.10 and §6.11's silent windowing bugs live.

- **13.15 Continuous batching with preemption.** Sequences are admitted,
  scheduled at iteration granularity, and evicted under memory pressure — the
  full `ML_INFER` shape of `llm-kernel-scheduling-spec`, and the largest
  single piece of scheduling work in the plan. Static batching was rejected
  for head-of-line blocking. Admission-without-eviction was considered and
  rejected because 13.16 makes eviction cheap enough that the simplification
  does not pay for the capability it gives up.

- **13.16 Evicted blocks go to a file-backed, prefix-keyed block store, not to
  recompute.** The standard advice is to recompute an evicted prefix, on the
  reasoning that recompute reuses the prefill path and so pipelines with
  already-scheduled work. The arithmetic does not support it here. KV cache is
  cheap to move and expensive to make: Llama-3.1-8B holds
  2 × 32 layers × 8 KV heads × 128 dim × 2 bytes = **128 KB per token**, while
  regenerating that token's KV costs a full prefill pass of roughly 16 GFLOP.
  On the 13.20 reference machine that is ~2 ms per token to recompute against
  ~38 µs to write and read back over NVMe — a 4096-token conversation costs
  ~8.2 s to recompute and ~156 ms to reload. There is no crossover point:
  NVMe's fixed latency is ~100 µs, which recompute burns in a twentieth of one
  token.
  Blocks are keyed by prefix hash rather than owned by one sequence, so the
  store serves any request sharing that prefix and survives process restart.
  That turns eviction storage into a prompt cache, which is the dominant cost
  in agent workloads that resend a system prompt every turn.
  Two consequences. The OS page cache becomes the RAM tier at no cost —
  evictions resumed quickly never reach the disk — so no hand-managed pinned
  host tier is built. And SSD write endurance becomes rate-dependent: at one
  eviction per minute a 1200 TBW drive lasts years, at one per second it does
  not, so eviction rate and bytes written are reported alongside throughput
  per §12.10.

- **13.17 The block store builds on `LtmPager` and `LtmBPlusTree`.**
  `cajeta.collection.ltm.LtmPager` is already a database-style buffer pool — a
  fixed frame pool over a paged file with pin/unpin, dirty tracking, LRU
  victim selection and write-back on eviction or flush. That is the machinery
  the block data file needs. It is typed to `LtmBPlusTreeNode<K,V>`, so reuse
  means separating frame management from node deserialization and teaching it
  to page into a `KernelBuffer`.
  `LtmBPlusTree` is the durable index from block key to file extent. The index
  is **not** on the hot path: it is consulted on admission, eviction and
  resume, never per decode step, and admitting a 4096-token prompt is ~256
  lookups at ~0.5 ms resident against the ~156 ms of block I/O it guards. It
  is chosen for durability and ordered traversal, not for speed. An in-memory
  hash map — vLLM's chained block hashes, SGLang's radix tree — cannot survive
  restart without reimplementing a log and a recovery path, and cannot do the
  ordered scans that LRU sweeps and restart enumeration need. B+tree write
  amplification does not bite here: a key plus extent is ~32 bytes against
  2 MB of block data.
  Three stdlib gaps must close and are units of this plan. `LtmBPlusTree` has
  **no delete** — the docs defer it; this is tombstones plus compaction, not
  page merge and underflow rebalancing. It has **no ordered scan or
  iteration** — the in-memory sibling has `keysInOrder`, the LTM variant does
  not. And `Encoder.encode` returns a fresh `heap int8[]` per call, so lookups
  allocate; that one is tidiness rather than a blocker, because §4.3's
  invariant governs the decode step and the decode step never touches the
  index. `order` and `pageSize` are set for a disk fanout — roughly 120
  entries per 4 KB page — not the in-memory default of 32.
  `cajeta.nucleo.frame.BPlusIndex` is **not** used: bulk-loaded, immutable
  after build, numeric-key-only, built for columnar predicate pushdown.

- **13.18 Chat templates are interpreted, not tabulated.** *(Resolves §14.1.)*
  A Jinja subset scoped to the chat-template dialect, reading `chat_template`
  from `tokenizer_config.json` and from GGUF `tokenizer.chat_template`. A
  table of hand-written templates was rejected on evidence: `llama.cpp`
  shipped exactly that, an if-else chain over detected template names, and
  added `--jinja` to replace it once tool-call templates arrived; Ollama
  maintains its own table and mis-renders new models for the same reason.
  Compatibility comes from interpreting the artifact the model ships, and that
  artifact is Jinja.
  The scope is the dialect, not the language: `for`, `if`/`elif`/`else`,
  `set`, `{{ }}`, whitespace control, macros, a dozen filters (`tojson`,
  `trim`, `join`, `default`, `length`, `lower`, `replace`), a few tests
  (`is defined`, `is none`, `is string`, `is mapping`), `.items()`/`.get()`,
  slicing, `~`, `raise_exception()`, `strftime_now()`. Two things make it
  cheap here: the value model is JSON-shaped, so `dev.cajeta.codec.json`'s
  `JsonValue` is the interpreter's value type, and the grammar is ANTLR4 like
  the rest of the toolchain. The pre-rendered-prompt entry point stays as
  permanent API surface for templates that do something exotic. No table ships
  alongside it — two renderers that can disagree is worse than one that fails
  loudly.

- **13.19 The SIMD `tableLookup`/`widen`/`narrow` items are owned by this
  plan.** *(Resolves §14.6.)* They move out of `agents/cajeta/Simd-plan.md`
  (8 of 28 complete) and become a unit here. §12.8's throughput gate applies
  to whichever backend runs (14.2), so v1 cannot ship without them, and owning
  them means they cannot slip behind that plan's other priorities.

- **13.20 Throughput gates and the reference configuration.** *(Resolves
  §14.8.)* Decode reaches **≥60%** of `llama.cpp`'s throughput and prefill
  **≥50%**, with resident memory within **1.25×**, measured on
  Llama-3.1-8B-Instruct Q4_K_M, gfx1151 / ROCm 7.2.2, 4096-token context,
  Release build with `LiveSet::Bounded`. A first version reaching roughly two
  thirds of a mature hand-tuned engine is defensible — `llama.cpp` has years
  of per-arch intrinsics in its quant unpacking that Unit 13 will not match
  immediately — while a bar much lower would pass a build with obvious wins
  still on the table. The `llama.cpp` baseline for that exact configuration is
  measured and recorded as a plan task before Unit 11; the fractions are
  meaningless against an unmeasured denominator.
  **Measured 2026-08-20** (tools/baseline/results/BASELINE-20260820.md,
  llama.cpp 5306f4b, ROCm 7.2.53150, gfx1151): prefill pp4096
  **794.24 tok/s**, decode tg128 **39.57 tok/s** at batch 1; batched
  4×(512pp+128tg): PP 1291.85 / TG 107.40 tok/s; peak RSS **5,201,728 kB**.
  The §12.8/12.9/12.11 denominators are therefore: decode ≥ 23.7 tok/s,
  prefill ≥ 397 tok/s, RSS ≤ 6,502,160 kB at this configuration.

- **13.21 §12.1 pins the parity metric; Unit 11 derives the threshold.**
  *(Resolves §14.4.)* Measured against Hugging Face `transformers` at **fp32
  on CPU** at a pinned model revision, so the reference contributes no noise
  of its own. Three complementary detectors rather than one brittle number:
  cosine similarity on the final-layer logit vector, top-1 token agreement,
  and KL divergence of the softmax. Elementwise relative error was rejected —
  a single outlier in the distribution tail fails a run that is behaviourally
  identical, and the tail is where floating-point noise concentrates.
  The numeric bar is set from an observed run in Unit 11 and written back
  here. Inventing a tolerance before anything has been measured is guessing,
  and a guessed gate is either vacuous or spuriously red.

  **MEASURED 2026-08-21** — Llama-3.1-8B-Instruct, f32 on CPU, 24-token
  prompt, against the pinned `transformers` fp32 reference
  (`tools/parity/run-parity.sh`):

  | detector | observed | THRESHOLD |
  |---|---|---|
  | cosine similarity | 1.0 | **>= 0.99999** |
  | softmax KL | 1.87e-10 | **<= 1e-6** |
  | top-1 agreement | agree | **required, exact** |
  | greedy agreement (32 tokens) | 1.0, 0 divergences | **>= 0.99, divergences only where the top-2 gap < 1e-3 (13.22)** |

  The thresholds sit four-plus orders above the observed noise and well
  below anything a real defect would produce: a wrong weight, a transposed
  projection or a mis-decoded block moves these by orders of magnitude, not
  by 1e-10. They gate the UNQUANTIZED path only — see the paragraph below.
  This gates the **unquantized** f16/bf16 path. Quantized execution cannot
  meet the same bar by construction and is gated separately, by perplexity
  delta against the unquantized engine — `llama.cpp`'s own methodology.

- **13.22 §12.3 requires 99% greedy agreement with tie-only divergence.**
  *(Resolves §14.9.)* At least 99% per-token agreement across repeated runs at
  temperature zero, and every divergence must sit at a position where the top
  two logits are within 1e-3. The tie clause carries the weight: the rate
  alone would pass a systematic bug that flips one token in two hundred at
  non-tie positions. 13.13 makes run-to-run variance real rather than
  theoretical — a sequence's batch neighbours differ between runs depending on
  arrival timing, so its reduction shapes genuinely change and a same-backend
  equality gate would be unsound. Checking this requires logging the top-2
  logit gap at each divergence point.
- **13.23 Dtype-generic ops guard placement and tier; they do not dispatch to
  kernels.** *(Amends §2.1, established 2026-08-09 during Unit 1.)* The
  original reading of §2 was that `Tensor`'s op surface would route to the
  `Ewise` kernel seam. It cannot: kernel-name resolution needs a concrete
  element type, so a launch from a body parameterized on `E` fails codegen with
  `XPU-N02: launch receiver is not a kernel name` — even though the identical
  call from a non-generic method compiles and runs. Established by building it,
  not by inspection.
  Two plausible explanations were tested and are wrong, recorded so they are
  not retried: it is not an ownership/retyping hazard (the safe inline-cast
  idiom exists, and allocating the output as `Tensor<E>` avoids the question),
  and `[[cajeta-kernel-no-generic-monomorph]]` forbids *declaring* generic
  kernels, not calling a concrete one from a generic caller.
  So generic ops guard and tier (2.7), and kernels stay reachable from
  concrete-dtype code. The cost to this project is small: the forward pass
  calls concrete f32/f16 paths, so the engine never depends on the generic
  surface routing. Lifting this needs a compiler change — carrying a concrete
  element type across a generic boundary — which is out of scope here.

- **13.24 No CI runs the gate today; the gate is manual and recorded.**
  *(Recorded 2026-08-20, plan 15.3.1.)* The repository has no workflow files:
  nothing runs `run-tests.sh` or the §12 parity/throughput instruments on
  push. The gate is operated by hand — `run-tests.sh` for the unit suite,
  `tools/baseline/run-baseline.sh` for the denominator,
  `tools/parity/run-parity.sh` for §12.1 — and each run leaves its record
  under `tools/baseline/results/` (dated) or the plan. When CI lands, it
  should run exactly these three entry points.

- **13.25 Architectural constraints for future weight editing and
  multiplexed training.** *(Recorded 2026-08-21. Not scope for the current
  release — these are the choices that are cheap NOW and expensive to
  retrofit, so they weigh on decisions taken before then.)*

  The intended direction: behaviour change at token granularity does not
  need every parameter updated. The addressable surface is the FFN's
  key/value memory — up/gate as pattern keys, down as the values summed
  and softmaxed into the output distribution — plus attention's K/V. Both
  are small relative to the model and both are already structurally
  separate in this engine. A separate design will cover it; these are the
  properties the engine must not lose in the meantime.

  - **13.25.1 One multiplication seam.** Every projection multiplies
    through `Linear.matvecInto` and nothing bypasses it. It exists today
    to dispatch packed-vs-f32; it is also the single place a delta, an
    adapter, or an edited key matrix can be applied. A call site that
    reaches into `.weight` directly forfeits that, so none may.
  - **13.25.2 Base weights are read-only; edits live in a separate
    layer.** The packed/quantized path (and mmap-backed storage after it)
    makes base weights compact and shared, so they cannot be mutated in
    place. Any edit is a distinct, small, mutable object applied at
    13.25.1's seam. This is forced by the memory design and is also what
    makes rollback and shadow-promotion possible at all.
  - **13.25.3 Weights are addressable by name at runtime.** An edit
    targets "layer N's FFN value matrix", so modules must be enumerable
    and nameable, not anonymous fields. `dev.cajeta.ml`'s
    `Module`/`Parameter`/`StateDict` shape is the obvious target, and it
    would also let ml's optimizers operate on engine weights without a
    conversion layer.
  - **13.25.4 The weight set carries a version.** A request pins a
    version for its whole generation, so concurrent editing cannot make a
    single response incoherent mid-stream. A counter costs nothing now
    and is invasive to add once requests are in flight against mutable
    weights.
  - **13.25.5 A step is not necessarily a generation step.** `stepSeq`
    and the scheduler must not assume every admitted unit of work
    produces a sampled token. Training, evaluation and distillation are
    additional JOB CLASSES for the same continuous-batching machinery —
    the scheduler already arbitrates competing sequences with preemption
    and resumption, which is the hard part.
  - **13.25.6 The arena must permit pinning.** Activations are recycled
    aggressively (6.1.6). Any learning step needs specific activations to
    outlive the step that produced them, so the pool's design must not
    make pinning impossible, even though nothing pins today.
  - **13.25.7 The KV cache is the key/value store.** Paged KV plus the
    prefix block store already separate key/value STATE from weights, and
    already support adoption across requests. Interventions in key space
    belong there rather than in a new subsystem.

  Rationale for recording it now: the reuse that makes serving-plus-training
  worthwhile — a training step riding the KV cache and activations a served
  request already computed — is only available to a runtime that owns both.
  That is an argument for keeping these seams open, not for building them
  yet.

  **13.25.8 METATUNING is a distinct phase, and the seams above serve it
  too.** Editing weights is the least interesting form of adaptation
  available here. A model's behaviour is also determined by what is
  RETRIEVED and how the retrieved evidence is turned into a distribution,
  and both are intervenable at inference time without touching a weight:

  - **Moderated KV.** A learned network sits over the KV cache and
    conditions what is written, kept, or returned — biasing key match,
    scaling or gating value contributions, re-weighting by position or
    provenance. The paged KV cache and block store (13.25.7) are already
    the state store this would moderate.
  - **Moderated output distribution.** A learned network biases the
    logits before sampling — the last point where behaviour is decided
    and the cheapest place to intervene, since it touches one vector per
    token rather than any weight.

  Both are meta-level: a small network adapting how a frozen LLM is USED,
  rather than what it contains. Three properties make this attractive.
  The adapted parameters are tiny and separate from the base weights, so
  13.25.2 holds trivially. The base model is untouched, so rollback is
  discarding a side model rather than restoring a checkpoint. And the
  intervention points — KV write/read and pre-sampling logits — are places
  the engine already owns, not new plumbing.

  Sequencing: metatuning is a PHASE AFTER the multiplexed harness, because
  every form of it needs the same thing — a learning job sharing the
  serving runtime, with versioned state a request can pin (13.25.4) and a
  step that is not a generation step (13.25.5). It gets its own spec; this
  entry exists so the seams are not closed before it is written.

  Open, to settle in that spec: what supplies the training signal (the
  same label problem as any online tuning); whether a metatuner may change
  outputs during the request that trained it or only after a promotion
  boundary; and how to evaluate a system whose whole purpose is to change
  outputs, given the expert cache's much easier "outputs must not change"
  guarantee does not apply.

## 14. Open questions

**All items resolved 2026-08-08.** §13 records the decisions; this section is
kept as the audit trail of what was asked and where each answer landed. New
questions are appended here as they arise.

- **14.1** *(Resolved 2026-08-08 — see §13.18.)* Chat templates are
  interpreted as a Jinja subset, not tabulated. Compatibility comes from
  reading the artifact the model ships.
- **14.2** *(Resolved 2026-08-08.)* CPU is a **first-class target**, not an
  oracle. The runtime dispatcher already selects CUDA → HIP → Vulkan → CPU at
  startup, so GPU-when-available with CPU fallback is the shipped behavior;
  the §12.8–12.11 throughput gates apply to whichever backend runs.
- **14.3** *(Resolved 2026-08-08 — see §13.13.)* Batched from the start, with
  a paged block cache. The follow-on decisions it forced are recorded
  alongside it: scheduling scope (§13.15), eviction policy (§13.16), and the
  store that policy rests on (§13.17).
- **14.4** *(Resolved 2026-08-08 — see §13.21.)* The metric is pinned now —
  cosine similarity, top-1 agreement and softmax KL against `transformers` at
  fp32 — and the numeric bar is derived from a measured run in Unit 11 and
  written back into §13.21.
- **14.5** *(Resolved 2026-08-08 — see §13.11.)* No dependency; pooling keeps
  the lock off the hot path and the parked sharding work stays parked.
- **14.6** *(Resolved 2026-08-08 — see §13.19.)* Pulled into this plan. With
  CPU a first-class target (14.2) these stop being a throughput follow-on and
  become the main CPU performance lever: batch-1 decode is
  memory-bandwidth-bound — the work is reading and unpacking quantized
  weights, not matrix arithmetic — so vectorized nibble unpacking is most of
  the CPU story, and `wave` maps to AVX-512/AVX2 lanes on that path.
- **14.7** *(Resolved 2026-08-08 — see §13.14.)* Always chunked, and decode is
  the same path at N=1. Required by 13.15's iteration-level scheduling rather
  than merely an activation-memory optimization.
- **14.8** *(Resolved 2026-08-08 — see §13.20.)* Decode ≥60%, prefill ≥50%,
  resident memory within 1.25×, on Llama-3.1-8B-Instruct Q4_K_M at gfx1151 /
  ROCm 7.2.2, 4096-token context, Release with `LiveSet::Bounded`. The
  `llama.cpp` denominator is measured and recorded before Unit 11.
- **14.9** *(Resolved 2026-08-08 — see §13.22.)* 99% per-token agreement, with
  divergence permitted only at near-tie positions — top-2 logits within 1e-3.
- **14.10** **Withdrawn 2026-08-08 — the claim was wrong.** It asserted that
  the fiber scheduler blocked CPU throughput parity. Kernel dispatch does not
  use fibers: `cajeta_xpu_dispatch.c:435-454` is a persistent worker pool with
  a broadcast-plus-barrier launch and no per-launch spawn, which is the same
  execution shape as `llama.cpp`'s thread pool. The real CPU risks are
  narrower and are tracked elsewhere: kernel quality — whether the `wave`
  lowering emits AVX-512/AVX2 comparable to hand-written intrinsics — and the
  vectorized unpack path (14.6). The live-set mutex stays off the decode loop
  only while §4.3's pooling holds, so that requirement is load-bearing rather
  than an optimization.

## 15. Mixture of Experts

Appended rather than inserted: `12.3`, `13.21`, `13.22` and their
neighbours are referenced by outline id from the plan, from test names and
from code comments, so renumbering the existing sections to slot this in
by topic would break every one of them.

**Definition.** An MoE decoder replaces the dense FFN with a *router* and a
bank of *experts*. Each token is scored against every expert, a small
number are selected, and only those run. `Qwen3-Coder-30B-A3B` on this box
declares 128 experts with 8 used per token — **6.25% of the FFN weights
touched per token**, which is the entire point of the architecture and the
thing an implementation is allowed to get wrong only by being slow.

Scope decided with Julian 2026-08-31: the seam is **general** (every gating
variant the ggml metadata can express, not just Qwen3's), the first working
run is on the **host**, and restoring batched prefill for QK-norm models is
**in** this arc rather than deferred.

### Structure

- **15.1** When a checkpoint declares an MoE architecture, the layer's FFN
  is built from a router plus expert banks rather than three dense
  projections, and the dense path is unchanged for non-MoE models.
- **15.2** When expert weights arrive as one rank-3 slab per projection
  (`ffn_gate_exps`, `ffn_up_exps`, `ffn_down_exps`), each expert's rows are
  addressed in place; no per-expert copy is made at load. The loader
  rejects rank 3 today (`Linear.bind`, two sites), and it rejects it
  loudly, which is the only reason this is a feature request and not a
  corruption report.
- **15.3** When an expert slice is taken from a quantized slab, it begins
  on a block boundary. Qwen3's widths (768, 2048) are multiples of 256 and
  so satisfy this; a model whose expert width is not is rejected with the
  width named, never silently mis-sliced.
- **15.4** When an architecture is MoE but unsupported, the error names the
  architecture AND which of its features are unimplemented, so the gap is
  actionable rather than a flat refusal (§4.5 for the general case).

### Routing

- **15.5** When the router runs, it scores every expert for every token and
  selects exactly `expert_used_count` of them.
- **15.6** When routing weights are combined, the gating function, the
  weight normalisation and the weight scale are READ FROM METADATA, never
  assumed from the architecture name. Two models with the same arch string
  and different gating are otherwise indistinguishable until the output is
  wrong.
- **15.7** When a model declares a router bias (`exp_probs_b`), it is added
  to the scores **before** top-k selection and excluded from the combining
  weights — the aux-loss-free balancing shape.
- **15.8** When gating is sigmoid rather than softmax, selection and
  normalisation follow the sigmoid convention.
- **15.9** When a model declares shared experts, they run for every token
  in addition to the routed ones, and their output is summed before the
  residual add.
- **15.10** When `expert_weights_scale` is present, it multiplies the
  combined expert output.

### Sparsity — the requirement that makes it MoE

- **15.11** When a token is processed, the weights READ are those of the
  selected experts and the shared experts only. An implementation that
  evaluates all experts and masks the result is correct and is NOT
  acceptable: at 8-of-128 it does 16x the work the architecture exists to
  avoid.
- **15.12** When prefill batches N tokens whose selections differ, the
  dispatch groups tokens by expert so each expert's weights are read once
  per batch, not once per token.

### Every variant is exercised, or it is not shipped

- **15.13** When a gating variant is implemented, a model or fixture that
  EXERCISES it is named alongside it. Generality bought without an
  exercising artifact is untested code by construction — the failure mode
  §13.24's manual gate and the 2026-08-23 vacuously-green amdgpu suite both
  record. The mapping is part of this spec:

  | variant | exercised by |
  |---|---|
  | softmax + top-k + weight-norm | Qwen3-Coder-30B-A3B (on disk) |
  | no weight-norm (top-2) | Mixtral-8x7B |
  | shared experts | Qwen1.5-MoE-A2.7B (also no-norm; 4-of-60) |
  | sigmoid gating, router bias, weight scale | **GLM-4.5-Air** — witness identified 2026-08-31 (verified: `e_score_correction_bias` on all 46 MoE layers, `routed_scaling_factor`, shared expert, dense first layer). ~65 GB at Q4_K_M against 122 GB RAM. Needs the `glm4moe` arch mapping; implementation still waits on that arc being scheduled. |

  A variant whose row has no artifact is deferred, not written — and the
  deferred row above is this rule APPLIED, not an oversight. The first
  draft named DeepSeek-V2-Lite as the witness for all three; its config
  refutes that twice (`scoring_func: softmax`, no router bias) and adds a
  blocker (`kv_lora_rank: 512` — MLA, a compressed-KV attention that is
  its own architecture work, not a gating knob). The models that DO carry
  sigmoid+bias are DeepSeek-V3-class (671B) and peers this box cannot
  hold. The SEAM is shaped so those variants are a small addition
  (gating-function enum, optional bias tensor, scale scalar read from
  metadata); their implementations wait for a witness.

### QK-norm (in this arc, separate subject)

- **15.14** When a model declares per-head q/k norms (`attn_q_norm`,
  `attn_k_norm`), batched prefill still engages. `batchReady` rejects
  `qNorm != null` today, so every Qwen3 model — MoE or dense — falls off
  the batched device path for reasons unrelated to its FFN.

### Gates

- **15.15** When an MoE model runs greedily, its first N tokens match
  `llama.cpp`'s for the same file, same context (the 20.1.6 shape).
- **15.16** When sparsity is measured, expert weight bytes read per token
  are within a small factor of `expert_used_count / expert_count` of the
  dense equivalent. Asserted on the MECHANISM — a byte or launch count —
  never on a wall-clock, which cannot separate "ran sparsely" from "ran
  densely on a fast day".

### Post-approval additions (review pass, 2026-08-31)

Added after §15's approval; each one was found by reading the witness
files' actual bytes rather than their model cards.

- **15.17** When a checkpoint uses the LEGACY split-expert layout
  (`ffn_gate.0.weight` … one tensor per expert, pre-2024-04 llama.cpp),
  it is rejected with an error naming the fix: a merged-slab requant of
  the same model. Measured: TheBloke's Mixtral Q4_K_M (2023-12) is this
  layout; mradermacher's (2024+) is the merged form. Supporting both
  layouts would double every dispatch test for files nobody produces
  anymore.
- **15.18** When a shared expert carries its own gate
  (`ffn_gate_inp_shexp` — qwen2moe's learned sigmoid on the shared
  expert's output), that gate is applied. 15.9's plain "run and sum" is
  Qwen3's shape; the Qwen1.5 witness on disk REQUIRES the gated form, so
  an implementation of 15.9 alone would bind the witness and produce
  wrong output.
- **15.19** *(Rewritten 2026-08-31 — the first version deferred paging
  "until hardware that needs it exists", which answered the wrong
  question: the pressure is model CAPACITY, not device type. A dozen
  30 GB experts is 360 GB against this box's 122 GB of RAM, and that is
  DeepSeek-V3-class — a family 15.13 already anticipates.)*

  Expert residency is TIERED, and the v1 tier must not foreclose the v2:

  - **v1 — the OS pages, because the loader already mmaps.** GGUF loads
    through `MappedFile`, and 15.2 addresses expert rows in place, so an
    expert that never fires is a page range that is never touched. A
    bigger-than-RAM MoE therefore LOADS AND RUNS on the host path with
    zero new machinery; the page cache is the eviction policy. The
    load-bearing constraint — the reason this works and the thing that
    must not regress: **nothing may eagerly materialize a full expert
    slab.** No load-time host repack of all experts, and device uploads
    per SELECTED expert group only, never per slab. One eager pass over
    360 GB at load turns "slow on cold experts" into "does not start".
  - **The floor, stated so nobody is surprised:** a token's cost is
    bounded below by (bytes of cold selected experts) / (storage
    bandwidth). Worst-case all-cold on the 360 GB hypothetical at NVMe
    speed is seconds per token; the real number depends on routing
    concentration, which is measured, not guessed — 15.20's utilization
    records exist to answer exactly this BEFORE a cache is built.
  - **The sizing envelope, derived 2026-08-31** from bytes/param
    measured on the local 8B quant ladder (Q4_K_M 0.61, Q3_K_M 0.50,
    Q2_K 0.40 B/param) and the measured 205 GB/s decode read rate:
    resident capacity is ~180B total params at Q4 (~275B at Q2);
    usable decode needs activated ≤ ~22B (10+ tok/s at moderate
    context). At the full 128k context the f32 KV read (~49 GB/token on
    a 94-layer model) dominates every MoE equally — expert sparsity
    buys nothing on attention, which is Unit 31's subject, not this
    section's. Qwen3-235B-A22B sits at the corner of both caps and is
    the largest usable MoE this box supports.
  - **v2 — a managed expert cache** (pinned hot set, router-guided
    prefetch, per-session affinity): a real subsystem, specified when
    v1's measured hit rate on a real oversized model says what it must
    do. The router knows a layer's selections before that layer's FFN
    runs, and 15.20 knows a session's expert affinity across turns —
    those two facts are the prefetch seam, and they are why v2 is an
    innovation surface rather than a port of someone else's LRU.
    *(2026-09-01: specified — `moe-expert-cache-spec.md` owns the
    mechanism from here. Its T1 — first-touch device-resident slots,
    budget-capped, evictionless, carrying the device decode dispatch —
    was motivated by units 28/29 measuring the per-group re-upload at
    ~40x of prefill; its T2 holds the policy half, still gated on
    15.20's records as this bullet required.)*
- **15.20** When routing runs under a host that observes the engine
  (§11.8's diagnostics records), per-request expert utilization is
  emittable as records — which experts fired, how concentrated the
  distribution was. This is the seam cabra-side innovation builds on
  (per-session routing behavior, expert-affinity scheduling) and it
  costs one record type now versus a redesign later.
