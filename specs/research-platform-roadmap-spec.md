# Spec: Cajeta research-platform capability roadmap (`research-platform-roadmap`)

## 1. Definition

### 1.1 Purpose
This is a **roadmap spec** — a master index, not an implementable feature. It
enumerates the capabilities Cajeta must hold to be a platform that ML / scientific
researchers will *actually use*, measured against the PyTorch/JAX baseline ("above
and beyond torch"), and it **sequences** them by dependency so each can be promoted,
one at a time, into its own implementable `docs/specs/<name>-spec.md` and then an
`agents/<name>-plan.md`. It records, per capability: **what it is**, the **research
necessary**, the **benefits & applications** it serves, its **dependencies**, and
**Cajeta's status today**.

The list was compiled 2026-06-29 from the PyTorch/JAX table-stakes surface plus two
web-research sweeps (8 agents) into the open frontier — see
[[project_differentiable_safe_gpu_wedge]] for the differentiator findings.

### 1.2 Guiding principle — own the bottom, build the middle, lead at the top
Mapping Cajeta's current state against the capability set reveals a clear shape:
**Cajeta owns the bottom and reaches the top, but is missing the middle.** It is
strong on the hard substrate most frameworks bolt onto an interpreter — compiler +
op fusion, custom `@Kernel` authoring, multi-vendor GPU (AMDGPU/SPIR-V/NVPTX today;
**Apple/Metal pending** — §3.2), numerical core, RNG. It is missing almost the entire
**training stack** — autodiff,
modules, optimizers, distributed, data loading. Consequence:

> **Table stakes gate adoption; differentiators gate mind-share.** No amount of
> deterministic-scatter brilliance matters to a researcher who cannot take a
> gradient, batch with `vmap`, train on 8 GPUs, or load a HuggingFace checkpoint.

Therefore the differentiators are **slotted into** the table-stakes path (built as
properties of the substrate they ride on), never sequenced ahead of it.

### 1.3 Scope
- **In:** the 20 must-have capabilities + the frontier differentiators; their
  research needs, benefits, dependencies, and current status; the dependency-ordered
  phasing; the child-spec index (§9).
- **Out:** implementation detail, APIs, test plans, acceptance criteria — those live
  in each **child spec** (authored later via the **design** skill) and its plan.
- **Out:** committing dates or staffing; this orders work, it does not schedule it.

### 1.4 How this roadmap is consumed
Each capability subsection below names a **child spec** (`→ <name>-spec`). When work
on a capability begins: (1) author `docs/specs/<name>-spec.md` collaboratively via the
**design** skill (this roadmap supplies its seed: capability, research, benefits,
deps); (2) derive `agents/<name>-plan.md`; (3) build via the **implement** skill. This
roadmap is the parent that the child specs trace back to; update it when sequencing or
status changes.

### 1.5 Non-goals
- Not a re-implementation of PyTorch — parity is the *floor* for table stakes, not the
  goal. Where Cajeta's compiler-first, borrow-checked, multi-vendor nature changes the
  design, the child spec says so.
- Not a promise to build every frontier bet (§8); those are optioned, not committed.

## 2. Sequencing model

Six phases. **Phase 0** finishes the substrate Cajeta already holds. **Phase 1** is the
keystone (autodiff) every later ML capability hangs off. **Phases 2–4** are the
table-stakes training stack and workflow that cross the "usable for research"
threshold. **Phase 5** is the differentiator frontier, built on the now-complete
substrate. The **critical spine** is: `tensor surface → autodiff → modules/optimizers →
distributed → interop`. Differentiators attach to the spine at the points noted.

A capability is tagged **[HAS]** (held, may need completion), **[PARTIAL]**, or
**[MISSING]**. Tags reflect state at 2026-06-29 and are re-validated when its child spec
opens.

## 3. Phase 0 — Substrate completion (held strengths, finish the gaps)

### 3.1 N-d tensor: complete the numpy surface  [PARTIAL]  → `tensor-numpy-surface-spec`
- **Capability:** broadcasting, advanced/boolean indexing, `einsum`, stride/view
  semantics, NEP-50 dtype promotion across the full `DType` set (incl. fp8/fp6/bf16).
- **Research:** view-aliasing under the borrow model (when a view is a borrow vs. a
  copy); broadcast/indexing codegen that stays fusable; promotion-table completeness.
- **Benefits & applications:** the lingua franca every downstream op, layer, and port
  assumes; without it nothing else is ergonomic. Serves all of ML + scientific code.
- **Depends:** existing `Tensor`/`Storage`/`DType`; the numpy port already in flight.

### 3.2 Multi-vendor GPU acceleration — incl. Apple Silicon  [PARTIAL]  → `apple-metal-backend-spec`
- **Capability:** one-source `@Kernel` lowering across *all* the hardware researchers
  actually own — AMDGPU + NVIDIA/NVPTX + Vulkan/SPIR-V + **Apple Silicon (Metal/MSL)** + CPU.
- **Status:** AMDGPU/SPIR-V/NVPTX/CPU are **[HAS]** and a genuine strength *beyond torch's
  NVIDIA-centricity* (xpu backends `amd`/`nvidia`/`vulkan`/`cpu` today). **Apple is the gap
  and is not optional:** Apple Silicon is the most common ML-research dev machine and
  PyTorch's MPS backend is heavily used for local iteration — a research platform that
  doesn't run on a MacBook loses the audience before it starts.
- **Research:** native **Metal/MSL** codegen vs. a near-term SPIR-V→Metal bridge
  (MoltenVK); Apple **simdgroup-matrix** as the WMMA/coop-matrix analog (§3.4); **unified
  memory (UMA)** semantics — host↔device copies are ~free on Apple Silicon, which changes
  the offload (§6.2) and staging (§6.3) calculus; the CPU-side AMX/matrix coprocessor;
  fp16/bf16 coverage (bf16 on M2+), fp8 limits (§5.3). Prior art to position against:
  Apple's own **MLX** (Apple-native but single-vendor) and **MPS**.
- **Benefits & applications:** every researcher on a Mac runs Cajeta locally; "AMD +
  Apple + NVIDIA, one source" is the multi-vendor story torch can't tell. Becomes a
  platform invariant once landed. (The Neural Engine / ANE is *out of scope* — closed,
  reachable only via CoreML.)
- **Build & validation gate (no Apple hardware on hand — develop blind, validate later):**
  write to the documented API now — host via **metal-cpp** (Apple's header-only C++ Metal
  wrapper; no Objective-C needed, dlopen `Metal.framework`), device via **MSL** (`kernel`,
  `threadgroup` memory = LDS analog, `simdgroup_matrix` = WMMA analog, `threadgroup_barrier`),
  UMA-aware `MTLBuffer` (`StorageModeShared`). **Two tiers:** **(1)** SPIR-V→Metal bridge —
  Cajeta already emits SPIR-V, so feed **SPIRV-Cross** to generate MSL (or run the existing
  Vulkan path under **MoltenVK**); most of this is *verifiable on Linux* (spirv-val + the
  SPIRV-Cross MSL it produces). **(2)** native MSL/AIR + simdgroup-matrix for perf — needs
  **Xcode's `metal` compiler** (macOS-only) and a real device. **On-device correctness +
  perf is gated on a Mac** (cloud: AWS EC2 mac instances / MacStadium / GitHub macOS CI
  runners; or a borrowed device), mirroring the repo's GPU-window discipline: emit-verify
  what we can on Linux, book a device window for the rest.

### 3.3 Graph capture + op fusion + JIT/AOT compile  [HAS] — baseline
- **Capability:** the `torch.compile`/XLA-equivalent — Cajeta *is* a compiler with
  kernel fission/fusion. Structurally ahead of bolt-on tracing.
- **Research (forward-looking):** a graph/op-level capture surface researchers can
  target for whole-program fusion (links to §7.4 auto-sharding and §8 sparse codegen).

### 3.4 In-language custom kernel authoring  [HAS] — baseline
- **Capability:** Triton/Pallas-class authoring via `@Kernel` (WMMA/coop-matrix — with
  Apple **simdgroup-matrix** as the per-vendor analog, §3.2 — exposed LDS / threadgroup
  memory, `Swizzled`/`CoopStage`). A first-class strength and the substrate the
  differentiators (§4.2, §8) exploit.

### 3.5 Dense + sparse linear algebra, solvers, FFT  [PARTIAL]  → `numerical-linalg-fft-spec`
- **Capability:** the cuBLAS/cuSOLVER/cuFFT/scipy surface — matmul, decompositions
  (LU/QR/Cholesky/SVD/eig), linear solves, FFT — at competitive perf on all backends.
- **Research:** which decompositions are kernel-expressible vs. need vendor-lib bridges;
  multi-vendor numerical parity; the sparse-solver subset (links §8.1).
- **Benefits & applications:** scientific computing, classical ML, optimization,
  signal/image processing, the math under every model.
- **Depends:** §3.1; existing `math/linalg`, `math/fft`, `math/stats`.

### 3.6 High-quality parallel RNG + distributions  [HAS] — baseline → minor completion
- **Capability:** counter-based (Philox-class) device RNG reproducible across
  parallelism + a full distribution library.
- **Status:** held (`math/random`, `GeneratorGpu`); completion folds into §6.4
  (determinism) so RNG is bit-reproducible by construction.

## 4. Phase 1 — The autodiff keystone

### 4.1 Autodiff: reverse + forward + higher-order  [MISSING]  → `autodiff-core-spec`
- **Capability:** vjp/jvp, `grad`, grad-of-grad, Hessian-vector products, per-sample
  gradients — not merely `.backward()`. The single highest-leverage item; every ML
  capability depends on it.
- **Research:** **where AD lives** is the load-bearing decision (see
  [[project_differentiable_safe_gpu_wedge]]): (A) source-to-source on the typed AST —
  cheapest first prototype, structure-preserving; (B) grow `XpuMir` from a structural
  envelope into a real computation IR and differentiate there — the eventual home and
  the substrate the sparse-AD moat (§8.2) needs; **not** (C) Enzyme-on-LLVM (the
  abandoned low-level position). Plus: checkpointing/remat interaction (§5.2), and a
  finite-difference oracle for validation.
- **Benefits & applications:** training, sensitivity analysis, inverse problems,
  optimization, differentiable simulation/rendering — the entire reason to build this.
- **Depends:** §3.1 tensor surface. **Unblocks:** §5, §6, §7, all of §8.

### 4.2 Deterministic, race-free scatter as the backward primitive  [MISSING]  → `deterministic-scatter-spec`
- **Capability (differentiator, slotted onto the keystone):** the gather→scatter
  transpose that *is* the AD backward of any gather is built as a **typed commutative
  accumulation** that is race-free under colliding indices **and bitwise-deterministic
  by construction** (fixed-order / reproducible accumulator, not raw atomics).
- **Research:** the typed commutative-monoid + determinism design (vs. Futhark's
  contract-only `reduce_by_index`, vs. CCCL RFA as a runtime knob); proving the AD
  transform *preserves* the type; LDS-staged accumulation. Out-position Descend / cuTile
  / Futhark — claim the conjunction none hold (imperative + borrow-checked + exposed LDS
  + typed-deterministic + AD-transparent + multi-vendor).
- **Benefits & applications:** the differentiable-rendering / sparse-attention / GNN
  backward pass becomes correct *and reproducible* — a claim no other framework can
  make. Acceptance demo: the 3D Gaussian-splatting backward (today an `atomicAdd`,
  nondeterministic, hand-reparallelized across a whole literature).
- **Depends:** §4.1 (it is the backward codegen), §3.4 exposed-LDS kernels.

## 5. Phase 2 — Single-device training stack

### 5.1 Module / parameter system  [MISSING]  → `module-system-spec`
- **Capability:** composable layers, parameter registration/iteration, buffers, train/
  eval state — the `nn.Module`-equivalent.
- **Research:** parameter ownership/lifetime under the borrow model; how state-dicts
  interact with §5.4 serialization; generic/reified layer types.
- **Benefits & applications:** the unit researchers build, share, and subclass; required
  for any model definition.
- **Depends:** §4.1 (parameters need gradients).

### 5.2 Optimizers + LR schedulers  [MISSING]  → `optim-library-spec`
- **Capability:** AdamW/SGD/Lion/Adafactor + warmup/cosine/step schedules; fused
  optimizer steps.
- **Research:** fused multi-tensor update kernels on all backends; optimizer-state
  dtypes (fp32 master, 8-bit states); foreach/capturable forms.
- **Benefits & applications:** every training run. Fused steps are a perf differentiator.
- **Depends:** §4.1, §5.1.

### 5.3 Mixed / low precision + autocast  [PARTIAL]  → `mixed-precision-autocast-spec`
- **Capability:** bf16/fp16/fp8 compute with automatic casting policy + loss scaling.
- **Research:** per-op autocast policy table; fp8 scaling/recipes; where the compiler
  can prove a cast safe vs. needs a runtime guard.
- **Benefits & applications:** 2–4× throughput/memory; the default regime for modern
  training. fp8 is increasingly mandatory.
- **Depends:** §3.1 (dtypes exist), §4.1, §5.2.

### 5.4 Checkpoint serialization + portable weights format  [PARTIAL]  → `checkpoint-serialization-spec`
- **Capability:** robust save/load of model + optimizer state; a `safetensors`-class
  zero-copy interchange format; sharded/resumable checkpoints.
- **Research:** a memory-mappable, borrow-safe tensor container; version/compat policy;
  cross-framework load (ingests safetensors).
- **Benefits & applications:** resumable training, model sharing, the on-ramp from the
  existing model ecosystem. Ties to §7.3 interop.
- **Depends:** §3.1, §5.1; existing `math/npio`.

## 6. Phase 3 — Scale & throughput

### 6.1 Distributed training + collectives  [MISSING]  → `distributed-training-spec`
- **Capability:** data-parallel (DDP/FSDP-class), tensor/pipeline/sequence parallelism,
  and collectives over NCCL/**RCCL** (AMD), with an MPI/host fallback covering Apple
  (no native multi-GPU collective lib) and CPU.
- **Research:** collective-comm binding per vendor; FSDP-style sharded params/optimizer
  state under the ownership model; overlap of comm with compute; failure/elasticity.
- **Benefits & applications:** *no frontier result is single-GPU* — this is the gate to
  serious model-scale research. AMD-capable distributed is itself beyond-torch-common.
- **Depends:** §4.1, §5.1, §5.2; existing CPU fork-join pool as the host floor.

### 6.2 Activation checkpointing / remat / grad-accum / offload  [MISSING]  → `activation-memory-spec`
- **Capability:** rematerialization, gradient accumulation, activation/optimizer offload
  — the levers that make large models fit.
- **Research (differentiator):** the borrow/live-set model knows allocation lifetimes —
  so **automatic, memory-optimal remat as a compiler/language feature** (vs. torch's
  manual `checkpoint()`) is in reach. This is beyond torch. The offload policy must be
  **device-aware**: on **Apple Silicon UMA** host↔device offload is near-free (a lever
  that doesn't exist on discrete GPUs), whereas AMD/NVIDIA pay a PCIe transfer.
- **Benefits & applications:** train models that otherwise OOM; the difference between a
  research idea fitting on available hardware or not.
- **Depends:** §4.1 (remat reshapes the backward), §6.1.

### 6.3 Data pipeline + input loading  [MISSING]  → `data-pipeline-spec`
- **Capability:** parallel, prefetching, sharded dataset/`DataLoader` abstractions;
  deterministic shuffling; streaming.
- **Research:** overlap with the training step; borrow-safe zero-copy staging to device;
  determinism/seeding across workers (ties §6.4).
- **Benefits & applications:** unglamorous but where researchers live; a slow input
  pipeline starves every GPU. Serves all training/eval.
- **Depends:** §3.1; the threading/concurrency runtime.

### 6.4 Reproducibility / determinism guarantees  [MISSING]  → `determinism-spec`
- **Capability:** seeding discipline, deterministic ops, and — uniquely — **bitwise
  reproducibility as a typed guarantee**, not a runtime flag that throws.
- **Research:** typed determinism propagated through ops + AD (consumes §4.2's
  deterministic accumulator); reproducible reductions across parallelism/vendors;
  the cost/precision tiers (run-to-run vs. GPU-to-GPU).
- **Benefits & applications:** debuggable research, regulated/clinical ML, exact
  ablations, reproducible papers. A standing differentiator the field only offers as an
  expensive opt-in.
- **Depends:** §3.6 RNG, §4.2 scatter; informs §6.1 (deterministic collectives).

### 6.5 Auto-parallelism / sharding compilation  [MISSING]  → `auto-sharding-spec`
- **Capability (differentiator):** annotate sharding, the compiler partitions the
  program (GSPMD/XLA-style) — beyond torch, JAX's edge, and a natural fit for a
  compiler-first language.
- **Research:** a sharding-annotation surface on §3.3's graph capture; partition + comm
  insertion as a compiler pass; cost model.
- **Benefits & applications:** scale without hand-written parallelism; lowers the barrier
  to multi-GPU/multi-node for non-systems researchers.
- **Depends:** §3.3, §6.1.

## 7. Phase 4 — Functional & workflow ergonomics

### 7.1 Composable function transforms + `vmap`  [MISSING]  → `function-transforms-spec`
- **Capability:** auto-batching (`vmap`) and the functorch/JAX `transform∘transform`
  model (`vmap`/`grad`/`jit` composition).
- **Research:** a batching-rule mechanism over the op set; interaction with §4.1 AD so
  `grad(vmap(f))` composes; compiler support for the batched dim.
- **Benefits & applications:** per-sample grads, ensembling, Jacobian/Hessian
  construction, clean vectorized research code. Now table stakes.
- **Depends:** §4.1, §3.1.

### 7.2 Profiling, debugging & observability  [PARTIAL]  → `observability-spec`
- **Capability:** op/kernel profiler, memory profiler, NaN/anomaly detection, a
  TensorBoard-class metrics surface.
- **Research:** a user-facing tensor/training profiler (vs. today's `gpu-profile`/rocprof
  plumbing); trace format; multi-vendor counter access.
- **Benefits & applications:** researchers abandon tools they can't see inside;
  performance work and correctness triage both depend on it.
- **Depends:** §3.2; existing `cajeta gpu-profile` / rocprof workflow.

### 7.3 Ecosystem interop  [PARTIAL]  → `ecosystem-interop-spec`
- **Capability:** numpy / **DLPack** / Arrow zero-copy, ONNX import/export, HuggingFace /
  safetensors ingestion, a clean Python bridge.
- **Research:** the borrow-safe zero-copy boundary (already prototyped in
  `TensorProtocol`); ONNX op coverage; the Python FFI surface.
- **Benefits & applications:** a research language that cannot load the world's models
  and data is stillborn; this is the adoption on-ramp.
- **Depends:** §3.1, §5.4; existing `TensorProtocol`/DLPack/Arrow seam.

## 8. Phase 5 — Differentiator frontier (optioned, built on the complete substrate)

These are the "above and beyond torch" bets from the research sweeps. Not every one is
committed; each is promoted to a child spec only when its substrate dependencies are met.

### 8.1 Sparse / structured tensors  [MISSING]  → `sparse-structured-tensors-spec`
- **Capability:** CSR/blocked/2:4 + structured formats and structured codegen; torch's
  sparse is weak and GPU-uncompetitive below ~90% unstructured sparsity, so target
  **structured/block/2:4 + fusion**, not general unstructured CSR.
- **Research:** a sparse iteration-space representation in the IR (the thing TACO/Finch
  are built around; `XpuMir` lacks it — see §4.1 path B); when sparse beats dense on the
  WMMA path. Prior art to out-position: Finch (CPU), MLIR Sparsifier.
- **Benefits & applications:** GNNs, recommender embeddings, sparse attention, scientific
  sparsity, pruned-model serving.
- **Depends:** §4.1 (path B IR), §3.4, §3.5.

### 8.2 Structure-aware sparse-codegen ∘ autodiff in one IR  [MISSING]  → `sparse-autodiff-fusion-spec`
- **Capability (the moat):** a sparse/structured forward yields a **fused sparse
  backward automatically** — "sparse forward → sparse backward for free."
- **Research:** fuse §8.1's sparse codegen with §4.1's AD pass in one structure-preserving
  IR. Prior art on the *idea*: ∇SD (CGO 2024, CPU/C++, no GPU) — Cajeta's novelty is
  *execution* (GPU, AMD, general language, one IR), not invention. Window ~1–2 yrs.
- **Benefits & applications:** every novel sparse/structured op (sparse attention, GNN
  message-passing) gets a competitive gradient as a compile step, not a manuscript.
- **Depends:** §8.1, §4.1 (path B), §4.2.

### 8.3 Differentiable hardware ray tracing  [MISSING]  → `differentiable-raytracing-spec`
- **Capability:** differentiable path tracing / inverse rendering through **hardware
  ray-query** (the fork LLVM enables SPIR-V ray-query), vendor-portable / AMD-capable.
- **Research:** AD through ray-query intrinsics; the scatter-based gradient accumulation
  (consumes §4.2); RT-core utilization on AMD.
- **Benefits & applications:** inverse rendering, neural rendering beyond splatting,
  differentiable graphics/vision. Warp/Slang prove demand and are NVIDIA-only.
- **Depends:** §4.1, §4.2; the fork ray-query path.

### 8.4 Forward-only / local learning  [MISSING]  → `local-learning-spela-spec`
- **Capability:** local/forward-forward learning rules as a first-class training target
  (per-layer local loss, no global backprop) — the SPELA lane.
- **Research:** a local-update training abstraction orthogonal to §4.1 reverse-mode AD;
  the connection to the user's `ml/spela-training` reference implementation.
- **Benefits & applications:** on-device / continual learning, memory-cheap training,
  non-backprop research. Unclaimed by any framework *and* aligned with the user's own
  research identity.
- **Depends:** §5.1 modules; orthogonal to §4.1.

### 8.5 Quantization (PTQ / QAT, int4/fp8 inference)  [MISSING]  → `quantization-spec`
- **Capability:** post-training + quantization-aware training; low-bit (int8/int4/fp8)
  inference and serving.
- **Research:** quant op kernels per backend; QAT fake-quant + straight-through gradients
  (uses §4.1); calibration.
- **Benefits & applications:** efficient inference/deployment, edge, the cost side of
  serving research. Increasingly expected.
- **Depends:** §4.1, §5.3, §3.4.

### 8.6 Probabilistic programming  [MISSING]  → `probabilistic-programming-spec`
- **Capability:** distributions + a sampler/inference layer (HMC/NUTS/VI) over the AD
  core.
- **Research:** a tractable PPL surface; reparameterization-gradient support; MCMC
  kernels.
- **Benefits & applications:** Bayesian ML, uncertainty quantification, scientific
  inference. A whole research segment torch serves only via add-ons.
- **Depends:** §4.1, §3.6.

## 9. Child-spec index & dependency spine

Promotion order follows the spine; differentiators attach where noted.

```
P0  tensor-numpy-surface ─┬─> P1 autodiff-core ─┬─> P2 module-system ─> optim-library
    numerical-linalg-fft  │       │             │        mixed-precision-autocast
    (GPU/compile/kernels/ │       │             │        checkpoint-serialization
     RNG: HELD)           │       │             └─> P3 distributed-training ─> activation-memory
                          │       │                     data-pipeline, auto-sharding
                          │       └─(differentiator) deterministic-scatter ─> P3 determinism
                          │                                                    P4 function-transforms
                          │                                                    observability, ecosystem-interop
                          └─(path B IR) ─> P5 sparse-structured-tensors ─> sparse-autodiff-fusion
                                                differentiable-raytracing, quantization,
                                                local-learning-spela, probabilistic-programming
```

**Apple/Metal** (§3.2, `apple-metal-backend-spec`) is the one backend not yet held — it
lands in P0 alongside `tensor-numpy-surface`, since every later phase assumes it.

**Keystone:** `autodiff-core` (§4.1) unblocks the entire training stack and every §8 bet.
**First differentiator on the critical path:** `deterministic-scatter` (§4.2), built as
the AD backward primitive. **The "usable for research" threshold** is crossed when
`autodiff-core + module-system + optim-library + ecosystem-interop` are done.

## 10. Open questions for the developer (resolve as child specs open)
- §4.1: commit to AD path A-first-then-B, or invest in the §4.1-path-B computation IR up
  front (gates §8.2 the moat)?
- §6.1: is FSDP-class multi-node mandatory for the target researcher, or does
  single-node-multi-GPU suffice for v1?
- §8: which differentiators are *committed* vs. *optioned* — esp. SPELA (§8.4) given its
  alignment with the user's own research?
