# cajeta-ml-gpu — cajeta-ml as a GPU library

Status: approved by Julian 2026-11-06.

## 1. Definition

### 1.1 Purpose
cajeta-ml becomes a GPU library: kernels and the support to run them, general
purpose, with cajeta-llm and cajeta-xgboost as its first two consumers. Both
consumers then use its components in place of their own copies.

### 1.2 Sequence (Julian 2026-11-06)
1. cajeta-ml gains the GPU kernels and kernel support, general purpose, shaped by
   what cajeta-llm and cajeta-xgboost need.
2. When that is done, cajeta-llm and cajeta-xgboost incorporate cajeta-ml's
   components where appropriate, in place of their own copies.

### 1.3 The problem, measured (2026-11-06)
- 1.3.1 cajeta-ml has no device code: 0 `@Kernel`, no `cajeta.xpu` use, and
  `Ops.hasGpu` answers false for all 40 ops in its op table.
- 1.3.2 `Tensor`'s device lowering (`cajeta.math.Ewise`, 27 kernels) covers
  equal-shape elementwise, sums, gather/scatter, pad/crop, sort, cumsum and a
  matmul that runs one 16x16 output tile per workgroup. It has no activations,
  norms, broadcast or axis reductions. Every op allocates its result and syncs
  the stream (35 syncs). It binds a device tensor from element 0 and ignores the
  view's offset and strides.
- 1.3.3 A kernel launch from a body generic over the element type fails to
  compile (`XPU-N02`). About 200 generic `Tensor` ops therefore never reach a
  kernel; given device operands, they download them in place.
- 1.3.4 cajeta-llm wrote its own: 304 kernels, of which 62 are general (dense
  f16 GEMM and mat-vec, attention, rmsnorm, rope, glu, softmax/top-k, argmax,
  copy and conversion) and 12 are a general op with a q8_K-pack epilogue. It
  uses `Tensor` as a storage carrier and works on raw arrays and
  `KernelBuffer`s, because `Tensor` has no offset views on the device, no
  out-param or in-place ops, no stream-ordered ops, no fusion and no quantised
  dtypes.
- 1.3.5 cajeta-xgboost wrote its own: 33 kernels, about 22 of them general
  (segmented histogram, scan, argmax, stable partition, fill/copy/iota,
  bucketize, an exact float reduction that runs on one thread). It copies
  `Tensor` data element by element into arrays to upload them.
- 1.3.6 Both built general kernel infrastructure for themselves: llm's
  `KernelCap` (an availability registry), `TileFamily` over `ShapeChoice`,
  arenas and zero-allocation scratch, and the census and value-check test
  tooling; xgboost's timed tile choice over `Autotune`.

### 1.4 Decisions (Julian 2026-11-06)
- 1.4.1 The general kernels and their support live in cajeta-ml. The stdlib
  changes only where the language or runtime must: kernel launch from generic
  code, `Tensor` on the device, and the availability registry in `cajeta.xpu`.
- 1.4.2 This work goes before the remaining cajeta-llm parity items
  (15.2.19-15.2.21), which wait on the stack. It replaces the earlier order
  ("finish the LLM work first, before working on ml GPU").
- 1.4.3 Device data is `Tensor`, made fit for the purpose (§2, §3). No second
  container.
- 1.4.4 Both consumers adopt the library's components (§9).
- 1.4.5 Each area measures against its own reference (§8.2).
- 1.4.6 Delivery goes foundations first: the compiler and `Tensor` changes (one
  cajeta release), kernel support, primitives, GEMM and epilogues, nn ops
  forward, backward, then adoption.
- 1.4.7 Backward kernels for the whole op table are in this spec, as the last
  library phase.
- 1.4.8 cajeta-ml splits out a kernel package; the consumers depend on it alone
  (§10).

### 1.5 Constraints
- 1.5.1 No consumer regresses. cajeta-llm keeps its measured parity with
  llama.cpp (decode, prefill, memory, every backend shipped), and cajeta-xgboost
  keeps its measured times against upstream XGBoost.
- 1.5.2 cajeta-ml's API holds: its op table was designed so device kernels
  arrive "without API change" (cajeta-ml-v3 spec).
- 1.5.3 A consumer's pin moves only to a released toolchain; library code that
  needs a stdlib change waits for the cajeta release that carries it.

### 1.6 Non-goals
- 1.6.1 GGUF quant formats, MoE routing and the paged KV cache stay in
  cajeta-llm. The library provides the general skeletons they plug into (§5.4,
  §7.4).
- 1.6.2 xgboost's objectives, gain formula and tree bookkeeping stay in
  cajeta-xgboost.

## 2. Kernels launched from generic code (compiler)
- 2.1 When a body generic over the element type launches a kernel, the kernel is
  instantiated for the concrete type at each call site and the launch runs.
  Today this fails with `XPU-N02`.
- 2.2 When a generic kernel is instantiated for a type a backend cannot lower,
  the build names the kernel, the type and the backend.
- 2.3 When `Tensor`'s generic ops have a kernel for their dtype and placement,
  they launch it instead of downloading device operands.

## 3. Tensor on the device (stdlib)
- 3.1 When a device tensor is a view with an offset or strides, a kernel reads
  the view's elements, not the buffer's from element 0.
- 3.2 When an op is given an output tensor, it writes into it without
  allocating; an in-place form exists for the elementwise ops.
- 3.3 When ops are queued on a stream, they run in order without a sync per op;
  the caller syncs when it reads the result on the host.
- 3.4 When a host tensor is moved to the device or back, the copy is one bulk
  transfer, not a per-element loop.
- 3.5 When a view into an arena is taken on the device, it respects the
  device's binding alignment, or the request is refused with the alignment in
  the message.
- 3.6 When an op cannot run on the operands' placement, it fails by name; it
  does not move the caller's tensors.
- 3.7 When a tensor is used per element on the host, the access costs about what
  a raw array access costs (llm measured 12x today).

## 4. Compute primitives (cajeta-ml)
Each primitive runs on every backend shipped, is segmented where noted, and is
value-checked against a host reference.
- 4.1 When a buffer is filled, copied, iota-filled or converted between f32,
  f16, bf16 and int8, one kernel does it.
- 4.2 When values are reduced (sum, min, max, sum of squares), the result is
  deterministic run to run, and an exact form exists for sums whose order must
  not change the bits.
- 4.3 When a prefix scan is asked for, it is available inclusive and exclusive,
  plain and segmented.
- 4.4 When the arg-max or top-k of each row or segment is asked for, ties
  resolve to the first index.
- 4.5 When a histogram is built over segments, the library chooses a shared or
  global variant and its tile by measurement.
- 4.6 When rows are partitioned by a predicate, the partition is stable and
  segmented.
- 4.7 When values are bucketized against sorted edges, each lands in its
  upper-bound bucket.
- 4.8 When elements are gathered or scattered by index, a scatter may add.
- 4.9 When data is sorted, by key or key-value, the sort is stable.
- 4.10 When a matrix is transposed or a tensor permuted on the device, one
  kernel does it.

## 5. Dense linear algebra (cajeta-ml)
- 5.1 When matrices of f32, f16, bf16 or int8 are multiplied, the GEMM uses the
  device's matrix cores where it has them and a tuned tile elsewhere, and its
  speed is within the §8 targets.
- 5.2 When one row multiplies a matrix (decode), a mat-vec kernel serves it.
- 5.3 When a GEMM's K is long, a split-K form with a fix-up is chosen by
  measurement.
- 5.4 When a consumer needs work fused onto a GEMM or a norm (a bias, an
  activation, a residual add, a pack to another format), it supplies an
  epilogue to the library's kernel instead of copying the kernel.
- 5.5 When a consumer's weights are in its own format, it supplies a decode step
  to the library's tile skeleton; the skeleton handles tiling, staging and the
  matrix cores.
- 5.6 When a batch of independent GEMMs is run, one launch serves the batch.

## 6. Neural-network ops (cajeta-ml)
- 6.1 When rmsnorm or layernorm runs, one kernel does it per row.
- 6.2 When rotary position embedding is applied, it supports the common variants
  (llama's, neox's).
- 6.3 When softmax, silu, gelu, glu, relu, tanh or sigmoid runs, a device kernel
  does it.
- 6.4 When attention runs over f16 K and V, flash forms serve prefill and decode,
  including grouped-query heads.
- 6.5 When an embedding row is looked up, a gather kernel does it.
- 6.6 When a cajeta-ml model trains on a device, every op in the op table has a
  device forward and backward, and `Ops.hasGpu` answers true for it.
- 6.7 When a model runs with mixed residency, the op fails by name, as
  `Ops.requireSame` already specifies.

## 7. Kernel support (cajeta-ml, and cajeta.xpu where marked)
- 7.1 (cajeta.xpu) When code asks whether a kernel is usable on the active
  backend, a registry answers by name, with the reason when it is not.
- 7.2 When a kernel has several tile shapes, a family lists them, prunes those
  that do not fit with a reason, and remembers the measured winner per problem.
- 7.3 When a kernel is tuned, the choice is stored per build and device, and a
  run with no store is deterministic.
- 7.4 When a consumer writes its own kernel on the library's skeletons, it
  inherits the tuning, availability and test support.
- 7.5 When a consumer's tests run, it can account for every kernel it registers:
  run and value-checked, a probe, or a tracked skip.
- 7.6 When an inference loop must not allocate per step, the library provides
  arenas and fixed scratch it can reuse.

## 8. Backends and performance
- 8.1 When the library ships, every kernel runs on cuda, hip, vulkan and cpu, or
  declares its targets.
- 8.2 When a library op is measured against its reference on the same box the
  same day, it is equal or better (Julian's parity bar for cajeta-llm). The
  references:
  - 8.2.1 nn ops: torch on ROCm and CUDA.
  - 8.2.2 GEMM: rocBLAS and cuBLAS, and torch for f16 and bf16.
  - 8.2.3 Primitives: CUB and rocPRIM.
  - 8.2.4 The consumers: llama.cpp and upstream XGBoost, as today.
  - 8.2.5 Vulkan, which has no vendor library: llama.cpp's Vulkan backend for the
    ops it shares; otherwise the same op's hip numbers on the same box.
- 8.3 When a consumer switches to a library component, its own measured numbers
  do not regress.

## 9. Adoption by the consumers
- 9.1 When the library is done, cajeta-xgboost replaces its general primitives
  (§4) with the library's.
- 9.2 When the library is done, cajeta-llm replaces its general kernels and
  infrastructure (§5, §6, §7) with the library's, where appropriate.
- 9.3 When a consumer's kernel is a general op with a consumer-specific epilogue
  or decode step, it moves to the library's kernel with its own epilogue or
  decode.
- 9.4 When adoption is done, each consumer's general kernel count falls to what
  it has a measured reason to keep.

## 10. Packaging
- 10.1 When a consumer needs the GPU library, it depends on cajeta-ml's kernel
  package (primitives, GEMM, nn ops, kernel support) and not on the estimators,
  autograd or classical-ML surface.
- 10.2 When cajeta-ml's autograd runs on the device, it uses the kernel package
  like any other consumer.
- 10.3 When the kernel package releases, it does so on its own version, without a
  cajeta release unless it needs a new stdlib.

## 11. Open questions
- 11.1 Which epilogue and decode-step mechanism (§5.4, §5.5): a function
  parameter the compiler inlines into the kernel, or a template the consumer
  instantiates? To be settled in the plan's first kernel-support unit.
