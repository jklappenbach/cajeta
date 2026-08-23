# The packed mat-vecs run on one core, and off the XPU path entirely

**Filed 2026-08-22**, from the performance ranking that followed
`quant-scalar-decode-cost`. Every other item on that ranking was a
percentage of one kernel. This one is a multiple.

**Revised the same day.** The first draft proposed threading the host path
with `spawn`. That was wrong twice over: it would have reimplemented, worse
and capped, machinery the XPU CPU backend already has, and it declared the
XPU path a non-goal on a reason that turned out to be no reason at all.

## 1. Definition

`cajeta-llama`'s decode path is serial host code. `LlamaForCausalLM` calls
`rmsnormRowHost`, `attendRowHost` and `matvecInto` — never a kernel launch.
The `@Kernel` bodies in `Prim.cajeta` exist and, as far as decode is
concerned, are dead code.

This spec defines the work to route the packed mat-vecs through `@Kernel`,
so that they run on the XPU CPU backend's worker pool on the host and on
real hardware on a GPU device — one change, both.

**Scope.** The four packed mat-vec kernels
(`Quant.q4k/q5k/q6k/q8MatVecInto*`) and their single caller seam,
`Linear.matvecInto`.

**Deliverables.** Host parallelism for the decode path, and a forward path
that reaches the GPU backend at all — the second is a consequence of the
first, not extra work.

**Non-goals.** Changing numerics: this must not move a single output bit.
Prefill/decode batching to GEMM — a successor, for the reason in §8.2, not
an exclusion.

## 2. Why — the measurement, not the intuition

- **2.1** A Q4_K projection at 4096x4096 reads **9.44 MB** of weights. At
  today's 11.28 ms (f32 kernel) that is **837 MB/s** against a machine that
  sustains **~39 GB/s** — 2% of available bandwidth.
- **2.2** The memory floor for that read is **0.242 ms**. The kernel is
  **47x above its own floor** (36x for the q8_K kernel at 8.77 ms). It is
  compute-bound with a very large margin, which is exactly the condition
  under which adding cores pays close to linearly.
- **2.3** This is also why `llama.cpp` gains only ~1.8x from `-t 32` while
  we should gain far more: it is already near the memory wall and we are
  nowhere near it. That comparison is not evidence that threading pays
  poorly — it is evidence that it pays poorly *once you are fast*.
- **2.4** `y[i] = dot(W[i], x)` — rows share only the read-only weight
  buffer and the read-only activation vector, and each writes one distinct
  `y[i]`. No reduction, no accumulator contention, no false sharing beyond
  the last cache line of `y`. This is a data-parallel grid in the shape the
  `@Kernel` model already expects: one work item per output row.

## 3. Why `@Kernel` and not `spawn`

Both mechanisms exist. They were compared by reading them, not by
preference.

- **3.1** The XPU CPU backend (`runtime/native/cajeta_xpu_dispatch.c`) runs
  a **persistent worker pool**: a dispatch is a broadcast plus a barrier,
  with no per-launch `pthread_create`. Grid blocks are chunked across
  **`min(gridX, cores)`** workers, the calling thread running the last
  slice. It is sized to **cores**, with no fixed ceiling.
- **3.2** It already implements the serial-cutover rule this spec would
  otherwise have to invent: below a work-item threshold, or with one core
  or one block, a launch runs serially because fan-out costs more than it
  saves. The threshold is tunable at runtime via
  `CAJETA_XPU_CPU_PARALLEL_THRESHOLD` and was re-tuned once the persistent
  pool removed the per-launch spawn cost.
- **3.3** The `spawn`/carrier route is worse on every axis that matters
  here. Carrier count is `$CAJETA_CARRIERS`, else `min(nproc, 4)`, clamped
  to `CAJETA_MAX_CARRIERS = 16` — a bound that `git log -S` shows entered
  with the R8.3 scaffold (`902bf080`) as the size of a **static array**,
  with no rationale in any commit, comment or doc. Fiber stacks are 1 MB,
  forcing coarse per-carrier granularity. None of that applies to a kernel
  launch.
- **3.4** Only the `@Kernel` route also makes the GPU backend reachable.
  The `spawn` route would have left the forward path host-only forever.

### 3.5 What is already verified about `@Kernel`

Read from the compiler's own tests, not assumed.

- **3.5.1** Kernel bodies may use `Vector<T,N>`, `vload`/`vstore`,
  `Barrier`, `Shared` and `CooperativeMatrix`
  (`test/xpu/XpuVectorStagingProbeTests.cpp`, `XpuVulkanEmitTests.cpp`).
  The SIMD the packed kernels depend on is available inside a kernel.
- **3.5.2** `KernelBuffer<int8>` is supported and already carries packed
  int8 operands in shipped tests (`AmdgpuCoopI8Tests`, `CoopI8GemmTests`).
  The packed weight array crosses as a buffer.
- **3.5.3** `dotAccum` was built with a device lowering
  (`AmdgpuKernelLowering.cpp`, `amdgcn_sdot4`/`udot4`), so it was designed
  for this path rather than retrofitted to it.
- **3.5.4** MEASURED 2026-08-22 — the gate FAILS, and worse than the
  question anticipated. It asked whether `dotAccum` keeps its VNNI tier in a
  kernel body, expecting a performance answer. In a kernel body `dotAccum`
  returns a **WRONG ANSWER**, silently.

  Identical expression, identical inputs, identical reference; only the
  mechanism differs (the control this project's method requires):

  | path | instructions | lane 0 | correct |
  |---|---|---|---|
  | host | `vpdpbusd` | **-1240** | yes |
  | CPU-backend kernel body | `vpmaddwd` + `vpaddd` | **6440** | no |

  6440 is exactly the sum with the ACTIVATIONS read as unsigned
  (`5*166 + 10*203 + 15*240 - 20`), against `-1240` read as signed. It is
  neither a tier drop nor a reduction-order difference — the strided
  interpretation gives -20, also not 6440. Probes rule out the buffers: a
  `uint8` vload in a kernel reads back `0 5 10 15` and an `int8` vload reads
  back `-127 -90 -53 -16`, both correct.
- **3.5.5** ROOT CAUSE — the device lowering seam is
  **symmetric-signedness** and cannot express what `dotAccum` means.
  `KernelLowering.cpp` routes it through
  `LoweringTarget::integerDot4x8(..., bool isSigned)` whose base is
  `vecops::idotWiden`, which widens BOTH operands with the same flag. But
  `dotAccum`'s contract is unsigned weights x SIGNED activations — the
  asymmetry that is the entire reason the instruction family exists
  (`vpdpbusd`, `usdot`, `vqdotsu`), and which `simd-fused-integer-madd`
  records two ISAs choosing independently. With an unsigned receiver the
  seam passes `isSigned=false` and zero-extends the activations too. The
  host path is correct only because it bypasses this seam entirely.
- **3.5.6** BLAST RADIUS — every device backend, not just the CPU one. The
  Vulkan override selects `OpSDot`/`OpUDot` and the AMDGPU override selects
  `amdgcn_sdot4`/`udot4` from the same single flag; both pairs are
  symmetric, so neither can express unsigned x signed. `dotAccum` is
  therefore wrong on GPU too — found on the CPU backend only because that is
  the backend being routed first. The fix needs two signedness flags at the
  seam plus a mixed path per backend (`OpSUDot` on Vulkan where available; a
  widen-and-multiply on AMDGPU, since sdot4/udot4 cannot do it).
- **3.5.7** SECOND, INDEPENDENT DEFECT found by the same probe: the
  **portable tier is wrong under AOT** while correct under JIT. Same source
  at `--cpu=x86-64`:

  | build | lane 0 | correct |
  |---|---|---|
  | JIT (`VectorDotAccumTests`, 12/12 green) | -1240 | yes |
  | AOT `--emit=exe` | **-20** | no |
  | AOT, `CAJETA_SIMD_SCALAR_FALLBACK=1` | -1240 | yes |

  -20 is the STRIDED reduction — `llvm.vector.partial.reduce.add`'s native
  lane mapping with the deinterleave shuffle absent. The shuffle IS in the
  emitted code path (`VectorOps.h` tier 2, and its mask is correct), and the
  scalar floor under the same AOT build is right, which isolates the loss to
  tier 2 under AOT. This echoes the `aot-bitcast-f32-isel-abort` finding that
  the JIT runs the LLVM verifier and AOT does not, and it means the whole
  host test suite for this tier has been passing on a pipeline that is not
  the one that ships.
- **3.5.8** IMPACT — contained, and Unit 3 proceeds. Of cajeta-llama's
  packed mat-vecs only `q4kMatVecIntoQ8` uses `dotAccum`; the four f32
  kernels that `matvecInto` actually routes today use `widenLo`/`widenHi`/
  `toF32` and touch neither defect. So the routing re-scopes to the f32
  kernels (plan 2.3.2) and the q8_K path waits on the seam fix.

## 4. Routing the mat-vec

- **4.1** When a packed mat-vec runs, each output row is one work item, and
  the grid is the row count.
- **4.2** When a work item runs, it computes exactly its own row and writes
  exactly `y[row]` — the existing per-row body is already this shape, since
  every kernel's outer loop is `while (i < rows)` over an independent row.
- **4.3** When the host has no device, the launch runs on the CPU backend's
  worker pool and the result is identical to the serial path.
- **4.4** When the grid is too small to be worth fanning out, the existing
  threshold in §3.2 runs it serially — this spec adds no threshold of its
  own.
- **4.5** When a caller needs the serial host path (debugging, a
  bit-for-bit reference), it remains reachable and unchanged.

## 5. The live-set trap — it applies to BOTH routes

- **5.1** The first kernel dispatch calls
  `__cajeta_live_set_go_multithreaded()` (`cajeta_xpu_dispatch.c:674`),
  exactly as the first `spawn` does. It is a **one-way, process-global**
  flip: afterwards every `__cajeta_live_set_add` and
  `__cajeta_live_set_claim` — so every heap allocation and every drop
  anywhere in the process, for the rest of its life — takes one global
  `pthread_mutex`. Nothing turns it back off.
- **5.2** The mat-vec kernels are themselves safe from this: as of
  `headK4` every scratch array is hoisted out of the row loops, so the
  inner loops allocate nothing. The exposure is the REST of the engine —
  tokenizer, tensor construction, sampler — which allocate freely and would
  begin paying a lock they do not pay today.
- **5.3** When the cost of that flip is measured, it is reported on its
  own, separately from any parallel gain, so a reader can see both. This is
  the one risk that can invert the whole result, and choosing `@Kernel` over
  `spawn` does not avoid it.

## 6. Not moving the numbers

- **6.1** When a routed mat-vec finishes, its output is **bit-identical** to
  the serial one. Row independence makes this exact — no reassociation, no
  reduction order to preserve — so it is a hard equality, not a tolerance.
- **6.2** When the engine decodes a prompt through the kernel path, it
  produces the same tokens as the serial path.
- **6.3** Every existing K-quant fixture passes unchanged.

## 7. Acceptance

- **7.1** The Q4_K mat-vec at 4096x4096 scales with worker count, reported
  as a table across 1/2/4/8/16/nproc rather than a single number, so the
  curve and its knee are visible.
- **7.2** End-to-end decode improves against a serial baseline re-measured
  in the same session — the 7.83 s/token figure predates today's kernel
  work and must not be carried over.
- **7.3** Bit-identical output per §6, gated by test.
- **7.4** §5's cost is measured and reported as its own number. A net win
  that hides a large regression inside a larger gain is not acceptable.
- **7.5** DISCHARGED 2026-08-22 by §3.5.4-§3.5.7, with a stronger answer
  than asked for: not a tier report but two correctness defects. Re-opens
  when the seam is fixed and the q8_K path is routed.
- **7.6** Every timing is taken on a verified-idle machine per
  `quant-scalar-decode-cost` §2, with an untouched control in the same
  interleaved run. Today's Q6_K result — where the changed kernel and the
  untouched one moved the same 2% — is why the control is mandatory.

## 8. Open questions

- **8.1** DECIDED 2026-08-22 — spike §5 FIRST, before any routing is built.
  Force the multithreaded flag on in an otherwise-serial engine run and
  measure end-to-end decode against the unflipped baseline. If the global
  allocation mutex costs more than parallelism can win, this plan pivots to
  sharding the live-set and the routing waits behind it.
- **8.2** Batching to GEMM is a **successor, not an exclusion**. It is a
  memory-traffic optimization, and at 47x above the memory floor we are not
  memory-limited, so it buys close to nothing today. It becomes the lever
  precisely when this work saturates: 11.28 ms across 32 workers is
  ~0.35 ms against a 0.242 ms floor, so this spec walks us INTO the memory
  wall and batching is what moves it. Note also that it is not
  prefill-only — the continuous-batching `Scheduler` means batched decode
  across concurrent requests is real — and that if batching lands later the
  natural decomposition is over tiles rather than rows, so §4.1's
  one-row-per-work-item choice should be re-examined then rather than
  assumed permanent.
- **8.3** Whether the other host primitives on the forward path
  (`rmsnormRowHost`, `attendRowHost`, `gluRowHost`) should follow. They
  already have `@Kernel` twins in `Prim.cajeta` that decode never calls, so
  the marginal cost is routing rather than authoring. Out of scope here
  only to keep the first measurement attributable to one change.
- **8.4** `CAJETA_DEFAULT_CARRIERS_CAP = 4` and `CAJETA_MAX_CARRIERS = 16`
  are now off this spec's critical path, since the carrier pool is not the
  mechanism. The finding in §3.3 — that the 16 is an unjustified static
  array bound, and that each carrier embeds a ~16 KB deque so 256 KB of BSS
  is reserved even under `CAJETA_CARRIERS=1` — is recorded here so it is not
  lost, and should be filed separately against the runtime.
- **8.5** `docs/specification/concurrent/Concurrency.md` documents fiber
  stacks as "~64 KB initial"; they have been **1 MB**
  (`CAJETA_FIBER_STACK_SIZE`) since the native-call-depth fix. Found
  alongside; also belongs to the runtime, not here.
