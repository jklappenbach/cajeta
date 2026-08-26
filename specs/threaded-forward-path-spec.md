# The packed mat-vecs run on one core, and off the XPU path entirely

**Filed 2026-08-22**, from the performance ranking that followed
`quant-scalar-decode-cost`. Every other item on that ranking was a
percentage of one kernel. This one is a multiple.

**Revised the same day.** The first draft proposed threading the host path
with `spawn`. That was wrong twice over: it would have reimplemented, worse
and capped, machinery the XPU CPU backend already has, and it declared the
XPU path a non-goal on a reason that turned out to be no reason at all.

## 1. Definition

`cajeta-llm`'s decode path is serial host code. `LlamaForCausalLM` calls
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
- **3.5.6** BLAST RADIUS — every device backend. First asserted from
  reading the overrides, then MEASURED ON SILICON, because this project does
  not accept the former: built with `--xpu-backend=amdgpu` and run on a real
  **gfx1151**, the same probe returns **6440** — bit-for-bit the CPU-backend
  answer, and the same unsigned-activation arithmetic.

  Control, on the same GPU in the same build: `tagK` (scalar stores) returns
  `7 8 9 10` and `copyK` (`vload`/`vstore` of int32) returns
  `-20 -17 -14 -11`, both correct, and a `amdgcn-amd-amdhsa--gfx1151` code
  object is present in the binary. The GPU path is live and healthy;
  `dotAccum` specifically is wrong on it.

  WHY, precisely: hardware DP4a comes in exactly two flavours — signed x
  signed and unsigned x unsigned — and the compiler picks between them with
  ONE bit taken from the receiver's type. Q4_K weights are unsigned nibbles,
  so the receiver is `uint8`, the bit says "unsigned", and AMD's
  `v_dot4_u32_u8` reads the genuinely-negative `int8` activations as
  unsigned bytes: -127 becomes 129, -90 becomes 166. Not a rounding
  difference — a different computation.

  The irony is structural. `dotAccum` exists BECAUSE of the asymmetric
  unsigned x signed shape — it is why x86 has `vpdpbusd`, ARM `usdot`,
  RISC-V `vqdotsu`, and `simd-fused-integer-madd` opens by noting two ISAs
  chose that asymmetry independently. The device seam models signedness as
  one symmetric bit, so the single shape the operation was built to express
  is the one shape it cannot represent.

  ACCOUNTABILITY: the AMDGPU override was added earlier in this same arc
  (`simd-fused-integer-madd` 1.6). It emitted `sdot4`/`udot4` off the
  seam's existing single flag and inherited the defect rather than noticing
  it — and it shipped with no device-side correctness test, only the host
  ones, which is why it read as done.

- **3.5.6.1** FIXED 2026-08-22, and the fix shape is the REVERSE of what
  this spec first guessed. Reading the intrinsic tables rather than assuming:

  - **AMDGPU has a native mixed dot4** — `llvm.amdgcn.sudot4(i1 a_sign,
    v4i8 a, i1 b_sign, v4i8 b, i32 c, i1 clamp)` carries a sign bit PER
    OPERAND. So unsigned x signed is ONE instruction on AMD, not the
    widen-and-multiply this spec predicted, and the GPU tier for quantized
    inference is NOT slower than assumed. `archHasDot4` admits gfx1151, so
    the dev GPU takes it.
  - **Vulkan is the one without a mixed form** — stock LLVM's
    `IntrinsicsSPIRV.td` defines only `dot4add_i8packed` and
    `dot4add_u8packed`, both symmetric. SPIR-V ITSELF has `OpSUDot` via
    `SPV_KHR_integer_dot_product`, so this is an LLVM coverage gap, not a
    hardware one; the override falls back to the portable widen until an
    intrinsic exists.

  `integerDot4x8` now takes two independent flags. `dot` passes the same
  one twice (it is symmetric, and that is what makes it differ from
  `dotAccum` on an unsigned receiver); `dotAccum` passes
  `(receiver, /*cSigned=*/true)`, matching the host contract, which reads
  only the receiver's signedness and always sign-extends the activations.

  VERIFIED: **-1240** on the CPU backend and on real gfx1151, against 6440
  before. Control, same builds: `dot` on an unsigned receiver still returns
  **6460**, so the fix did not leak into the symmetric spelling.
- **3.5.6.2** How llama.cpp avoids this entirely, which is the design
  lesson: it never models signedness as a symmetric flag. `u x s` is its
  canonical form, because that is the only form the hardware offers, and
  symmetric cases are CONVERTED into it by sign transfer —
  `ax = _mm256_sign_epi8(x, x)` (magnitude) and
  `sy = _mm256_sign_epi8(y, x)` (sign moved onto y), so
  `|x| * (y*sign(x)) = x*y` exactly. Both its AVX2 (`maddubs`) and VNNI
  (`dpbusd`) paths do this. A seam whose canonical form is the asymmetric
  one cannot express the bug we shipped.
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
- **3.5.8** IMPACT — contained, and Unit 3 proceeds. Of cajeta-llm's
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

- **7.1** DISCHARGED 2026-08-23. The Q4_K mat-vec at 4096x4096, [out=4096,
  in=4096] = 9 MB of weight per call, 32 launches per sample, median of 3,
  arms ALTERNATED, on a 16-physical-core / 32-thread Ryzen AI MAX+ 395.
  The untouched control (the host serial path, which the worker cap cannot
  reach) moved **1.06%** across repetitions, so the window was idle.

  RE-MEASURED 2026-08-23 after §7.1.7 (the 4-lane kernel). Control spread
  1.19%.

  | workers | ms | GB/s | vs 1 worker | vs host serial |
  |---|---|---|---|---|
  | 1 | 2.142 | 4.10 | 1.00x | 5.87x |
  | 2 | 1.117 | 7.87 | 1.92x | 11.25x |
  | 4 | 0.673 | 13.06 | 3.18x | 18.67x |
  | 8 | 0.374 | 23.47 | 5.72x | 33.56x |
  | 16 | 0.319 | 27.58 | 6.72x | 39.44x |
  | 32 | **0.225** | **39.10** | **9.53x** | **55.90x** |
  | host serial | 12.57 | 0.70 | 0.17x | 1.00x |

  The threading factor is essentially unchanged (9.53x against the earlier
  9.92x — the same shape, the same knee between 4 and 8). What moved is the
  per-thread constant, and it moved 3.7x.

  SUPERSEDED FIGURES, kept so the two are not confused: before §7.1.7 this
  table read 7.98 ms at 1 worker and 0.804 at 32, i.e. 10.93 GB/s and
  15.82x against serial.

- **7.1.1** TWO SEPARATE WINS, and conflating them would overstate
  threading. The 15.8x against the host serial path is 9.92x of threading
  times **1.59x that one worker already beats the serial path by**. That
  1.59x is not threading at all: it is Unit 3's header decode into named
  locals instead of `headK4`'s `int32[8]`/`float32[2]` heap scratch — 16
  blocks per row x 4096 rows = 65,536 decodes that no longer round-trip 18
  values through memory. The serial path can have that win too, and should.

- **7.1.2** CACHE RESIDENCY IS NOT WHAT THIS MEASURES, checked rather than
  assumed. One 9 MB weight fits this box's 64 MiB L3, so a bench that
  re-reads a single matrix measures a cache-resident mat-vec while real
  decode streams ~4.5 GB per token. The probe therefore runs the sweep
  TWICE — one weight re-read (hot) and 16 distinct weights in rotation,
  150 MB, past L3 (cold). They agree within 5% at every arm (32 workers:
  0.784 ms hot, 0.804 ms cold). So the shared resource that saturates is
  not DRAM; both sweeps exceed the 1 MB/core L2 equally and both hit the
  L3/fabric path.

- **7.1.3** WHY IT IS 9.92x AND NOT 32x (§7.1's gap, attributed):

  | cause | evidence | share |
  |---|---|---|
  | not the backend's dispatch | a compute-only kernel on the SAME grid reaches 12.18x at 16 workers and 15.46x at 32 | — |
  | not launch overhead | measured directly with a nop kernel: 1.9 us serial, 26-41 us parallel = **5%** at 32 workers | 5% |
  | not SMT placement | pinned to the 16 physical cores (`taskset -c 0-15`), 16 workers gives 7.07x against 7.11x unpinned | 0% |
  | SMT itself | 32 logical threads on 16 cores; the compute control gains only 1.27x from 16 to 32 | caps the last step |
  | clock throttling | peak core clock 5026 MHz at 1 core, 4773 at 16, 4596 at 32 | 9% |
  | **memory traffic beyond L2** | the residual: Q4_K reaches 7.11x where the compute control reaches 12.18x, on identical grids and identical dispatch | the rest |

  The honest reading: **the backend delivers 15.5x and Q4_K takes 9.9x of
  it.** The shortfall is Q4_K's own 9 MB of streaming per call, not the
  threading.

- **7.1.5** PER FORMAT, cold weights, at 1 worker and at 32 (plan 6.3.2):

  | format | serial ms | kernel@1 ms | kernel@32 ms | GB/s@32 | k@1 vs serial | scaling |
  |---|---|---|---|---|---|---|
  | Q4_K | 11.67 | 2.12 | 0.230 | 38.2 | 5.49x | 9.23x |
  | Q5_K | 26.81 | 2.81 | 0.302 | 35.6 | 9.53x | 9.32x |
  | Q6_K | 25.51 | 3.06 | 0.357 | 35.9 | 8.32x | 8.57x |
  | Q8_0 | 25.42 | 1.70 | 0.238 | 69.6 | 14.9x | 7.14x |

  Q4_K is no longer the outlier — all four now sit in a 36-70 GB/s band.
  Its row read `11.85 | 7.99 | 0.873 | 10.1 | 1.48x` before §7.1.7.

  Q6_K's flat result under `headK6` (`quant-scalar-decode-cost` §5.1.3) did
  NOT repeat: 8.23x per thread. Different mechanism, as suspected.

  **The Q4_K kernel is the outlier, and it is the format that matters.**
  All four SCALE alike (8.0x-9.2x), so the spread is single-thread codegen,
  not threading. Q4_K is 74.4% of a Q4_K_M by bytes, so at these rates a
  token spends ~340 ms in Q4_K against ~32 ms in Q6_K; Q4_K reaching Q6_K's
  rate would take the mat-vec total from ~372 ms to ~132 ms. The suspect is
  shape — Q4_K's is the only kernel built on 16-lane vectors and an
  `@Device` helper, the three fast ones are 4-lane straight-line code.
  Tracked as plan 6.3.3, and FIXED — see §7.1.7.

- **7.1.7** THE Q4_K KERNEL IS 4-LANE, NOT 16 (plan 6.3.3, fixed
  2026-08-23). Two things differed between Q4_K's kernel and the three fast
  ones — 16-lane vectors and an `@Device` helper — so neither could be
  blamed from outside. Four shapes, identical grids, verified before timed,
  arms alternated:

  | variant | @32 workers | GB/s | vs A |
  |---|---|---|---|
  | A 16-lane + `@Device` | 0.790 ms | 11.1 | 1.00x |
  | B 16-lane, hand-inlined | 0.782 ms | 11.2 | 1.00x |
  | C 4-lane, reduce per sub-block | 0.228 ms | 38.5 | **3.46x** |
  | D 4-lane, reduce per block | 0.240 ms | 36.6 | 3.29x |

  B ties A, so the `@Device` call was never the cost — it inlines as
  advertised. The WIDTH is: inside a kernel the CPU backend vectorizes
  ACROSS work items, and a body already occupying 16 float lanes leaves it
  no room.

  The two settings therefore want OPPOSITE shapes, which was measured, not
  assumed: on the HOST, 4 lanes is 2.19x SLOWER (25.76 ms against 11.75),
  because there is no outer loop to widen there. So `q4kMatVecInto` keeps
  16 lanes and stays the host path.

  `q4kAcc`'s standing warning — that reducing per sub-block cost more than
  the width saved — turns out to be specific to 16-lane horizontal sums. At
  width 4, C beats D.

  THE BIT-IDENTITY BAR DID NOT SOFTEN, IT MOVED. `q4kMatVecIntoLanes4` is
  the kernel's body on the host in the kernel's order, and exists only to
  be that oracle; a further test ties the oracle itself to the scalar floor
  so the two cannot be wrong together. What relaxed is the `Linear`-level
  routed-vs-serial comparison, where the two now legitimately differ in the
  last bits — and every plumbing bug those tests exist to catch is wrong by
  orders of magnitude, not by a last bit. Q6_K and Q8_0 keep hard equality
  there, since their kernel and host paths do share an order.

- **7.1.6** A MEASUREMENT THE BENCH CAUGHT ON ITSELF, recorded because the
  first version of §7.1 was published without it. Synthetic weights filled
  with a byte pattern put ARBITRARY BITS in every block's f16 scale, which
  decode to NaN — so the bench was timing NaN arithmetic on both sides.
  `MatvecProbe`'s standing comment ("timing is data-independent here, so
  the pattern only has to be well-defined") is wrong for any format with an
  f16 field: NaN and denormal handling take different paths in scalar and
  vector code. The fix is one line per block — write a real f16 scale and
  leave the payload arbitrary — and it was found only because the bench now
  VERIFIES its own output against the serial path before reporting a
  speedup. It printed `nan vs nan`. Re-measured, the Q4_K curve moved from
  9.92x to 10.37x, so the earlier table was not materially wrong; the point
  is that nothing said so at the time.

- **7.1.4** §8.2's PREDICTION IS REFUTED. It expected ~32 workers to put
  Q4_K near a 0.242 ms memory floor. Measured 0.804 ms — 3.3x off — and
  the reason is that the floor was never approached: it assumed ~39 GB/s
  and the kernel achieves 10.9 GB/s. At one worker Q4_K moves 1.10 GB/s,
  20-35x below anything memory could explain, so the serial path was
  COMPUTE-bound, not bandwidth-bound. Packing wins on footprint; it does
  not arrive at the bandwidth wall until far more cores are pulling.
- **7.2.0** FINAL, re-measured 2026-08-23 after the accessor work, load
  1.44 before / 1.75 after, all arms back to back in one session:

  | engine | config | ms/token | t/s | samples |
  |---|---|---|---|---|
  | cajeta-llm | serial host floor | 2781 | 0.36 | 1 |
  | cajeta-llm | **routed, CPU 32 workers** | **111.6** | 8.96 | 4, spread 1.4% |
  | cajeta-llm | **routed, GPU gfx1151** | **100.0** | 10.00 | 5, spread 1.2% |
  | llama.cpp | CPU `-ngl 0 -t 1` | 177.6 | 5.63 ± 0.03 | 3 |
  | llama.cpp | CPU `-ngl 0 -t 32` | 71.4 | 14.01 ± 2.30 | 3 |
  | llama.cpp | GPU `-ngl 99` | 25.4 | 39.36 ± 0.57 | 3 |

  **We are 1.56x behind llama.cpp on CPU and 3.94x on GPU** — and 1.59x
  AHEAD of it single-threaded (111.6 against 177.6).

  CAVEAT ON THE CPU COMPARISON, stated because it favours us if omitted:
  llama.cpp's `-t 32` is the noisiest arm anywhere in this spec, ±2.30 on
  14.01 (16%), and an earlier run the same day gave 10.77 t/s (92.8 ms).
  Its true figure is somewhere in 71-93 ms, so our CPU gap is 1.2x-1.56x,
  not a single number. Ours is stable to 1.4%.

  A COLD-START ARTIFACT, recorded so the number is not re-derived: the
  GPU's FIRST run in a session measured 616.5 ms/token against a steady
  state of 100. Driver init, VRAM setup and clock ramp land inside a
  4-token average. Five subsequent samples span 98.8-101.2.

  RESIDENT MEMORY: CPU 9.40 GB, GPU 5.10 GB. The CPU path keeps the host
  array AND a device copy; the GPU path keeps only the packed weights in
  VRAM at 4.5 bits each, which is §8.15 answered as a side effect.

- **7.2** SUPERSEDED BY 7.2.0. DISCHARGED 2026-08-23. Meta-Llama-3.1-8B-Instruct-Q4_K_M,
  prefill(8) then 4 greedy decode steps, arms alternated, both engines run
  back to back in one session. This desktop never reaches idle (~2.0-2.5
  from the user's own applications), so the load is REPORTED rather than
  gated on — at these effect sizes two busy cores out of 32 cannot flip a
  conclusion, and gating would simply have produced no number at all.

  | engine | config | ms/token | t/s |
  |---|---|---|---|
  | cajeta-llm | serial host | ~3155 | 0.32 |
  | cajeta-llm | routed, CPU, 32 workers | **261.4** | 3.83 |
  | cajeta-llm | routed, GPU (gfx1151) | ~271 | 3.69 |
  | llama.cpp | CPU `-t 1` | 163.7 | 6.11 |
  | llama.cpp | CPU `-t 32` | 92.8 | 10.77 |
  | llama.cpp | GPU `-ngl 99` | **25.2** | 39.70 |

  **12.1x end to end** on decode against the serial path, and 12.7x on
  prefill(8) (22.97 s -> 1.80 s). Token sequences are IDENTICAL across every
  arm (`15 198 334 62 334`), which is §7.1.1's bar.

  THE GAP: we are **2.8x behind llama.cpp on CPU** at equal thread count and
  **10.8x behind it on GPU**. The arc closed a gap that was ~34x on CPU; it
  did not close the whole thing.

- **7.2.2** A MEASUREMENT INVALIDATED BY A MERGE, recorded because it
  nearly shipped. The first end-to-end run gave serial 7336 ms/token and
  routed 567. Merging 47 commits from origin/main then moved BOTH by ~2.2x
  (serial 7336 -> 3155, routed 567 -> 261) with no change of ours in
  between — main had been improving host codegen. The pre-merge figures are
  superseded, and the §7.2 baseline of 6.72 s/token from the Unit 1 gate is
  superseded with them. Any comparison that straddles that merge is void:
  the first GPU-vs-CPU ratio computed here did straddle it, and was wrong
  by 2.2x in our favour.

- **7.2.3** THE GPU IS NOT FASTER THAN THE CPU FOR US (271 ms against
  261), and that is the finding rather than a disappointment. The routed
  path is backend-agnostic, so the packed weights go straight into VRAM at
  4.5 bits each — RSS drops from 9.40 GB (CPU, host array PLUS device copy)
  to 5.10 GB (GPU, weights resident on the device), which incidentally
  answers §8.15 without the dequantize-to-f32 detour. But the mat-vec is no
  longer what a token costs, so making it faster changes nothing.

- **7.2.4** WHERE A ROUTED TOKEN ACTUALLY GOES (the attribution §7.4 asks
  for). Capping CPU workers multiplies ONLY the mat-vec share, and the
  kernels scale 9.06x weighted (9.23x Q4_K at 74.4% of bytes, 8.57x Q6_K at
  25.6%). Measured, same binary, one variable:

  | workers | ms/token |
  |---|---|
  | 32 | 261.4 |
  | 1 | 1216.6 |

  Solving `261.4 = M + R`, `1216.6 = 9.06M + R` gives **M ~ 119 ms of
  mat-vec (45%) and R ~ 143 ms of everything else (55%)**. That remainder
  is the host primitives this arc never touched — `rmsnormRowHost`,
  `attendRowHost`, `gluRowHost` — which still run single-threaded and
  already have unused `@Kernel` twins. Routing them is plan 8.1, and it is
  now the top item: no further mat-vec work can win back more than the 45%
  it owns, and llama.cpp's CPU figure (92.8 ms) sits BELOW our 143 ms of
  un-threaded remainder alone.

- **7.2.5** SUPERSEDED. End-to-end decode improves against the SERIAL BASELINE
  re-measured 2026-08-23: **6.72 s/token** (median 6722.92 ms/token). The
  previously-quoted 7.83 predated `headK4` and must not be used — carrying
  it forward would credit threading with a gain `headK4` already delivered.
- **7.2.1** Recorded alongside: `prefill(8)` costs **~47.4 s**, i.e. ~5.9
  s/token, because `matvecInto` is a mat-VEC called once per token inside
  `while (r < rows)` — every weight matrix is re-read 8 times for an
  8-token prompt. That is §8.2's batching item, and it is larger than this
  spec assumed.
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

- **8.1** CLOSED 2026-08-23 — **GATE PASSES**, the flip costs nothing
  measurable. Same binary, same input, one flag apart
  (`CAJETA_LIVE_SET_MT` forces the locked path on WITHOUT starting a
  thread), on a gated-idle box with the arm order ALTERNATED per round:

  | arm | median ms/token | spread |
  |---|---|---|
  | OFF (single-threaded live-set) | 6722.92 | 2.5% |
  | ON (locked path forced) | 6720.54 | 8.5% |

  Arm effect **-0.04%**, far under the noise floor. All six runs emitted
  the same token (`next: 15`), so this measured cost and not a behaviour
  change.

  SCOPE LIMIT, stated rather than implied: this decode is dominated by the
  mat-vec kernels, which allocate nothing in their inner loops (§5.2). The
  tokenizer and sampler do allocate, but they are a rounding error inside a
  6.7 s/token decode. The finding is "costs nothing FOR THIS WORKLOAD", not
  "the global allocation mutex is free".

  METHOD NOTE — take 1 was INVALID and is worth recording. With the arm
  order FIXED (OFF always first) on a loaded box, forcing a mutex ON
  measured FASTER than leaving it off: OFF fell 7714 -> 7196 -> 6706 while
  ON sat flat, because a decaying load lands entirely on whichever arm runs
  first. The valid run shows a **+3.7% position effect** — first-run-of-
  round is slower regardless of arm — which is exactly that artefact, now
  cancelled by alternating. A directionally impossible result (a pure cost
  measuring as a speedup) is a contamination signal, not a finding.

  Take 2 then ABORTED after 20 minutes at load1=0.39 because the idle gate
  also required zero `cajeta` processes — three were long-lived IDLE
  servers (two `compiler-mcp`, one IDE `--lint-server`) and one belonged to
  the measuring session itself. Gate on CPU consumption, never on process
  existence.
- **8.1.1** SUPERSEDED — original: DECIDED 2026-08-22 — spike §5 FIRST,
  before any routing is built.
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

---

# 9. GPU forward path — measured state, 2026-08-23

Everything in this section was MEASURED on the development box (AMD Ryzen
AI MAX+ 395 "Strix Halo", gfx1151, RDNA3.5, 40 CU, wave32, 32 MB MALL,
96 GB unified LPDDR5X) against
`Meta-Llama-3.1-8B-Instruct-Q4_K_M.gguf`. Where a number was inferred and
later measured, BOTH are recorded — the corrections are the most useful
part of this section.

## 9.1 Where we stand against llama.cpp

Baselines from `llama-bench` on the same box and model, `-r 6` (the
first `-r 2` run reported `pp512 548 +/- 220 t/s` and was NOISE; it was
contaminated by sharing an invocation with `tg128`):

| | llama.cpp | us | |
|---|---|---|---|
| decode, comparable amortization | 25.15 ms/tok (39.76 t/s) | **26.0 ms/tok** | **1.03x — PARITY** |
| decode @ 512 ctx | — | 32.6 ms/tok | — |
| prefill 512 tokens | **0.44 s** (1157.67 +/- 7.34 t/s) | **6.77 s** | ~15x |

Decode reached parity and has not moved since; every subsequent unit was
prefill.

**Re-measured 2026-08-24, post-10.12.16** (same box/model/method; ours
at ctx512 with dec64 steady state, 2 rounds alternated, idle-gated;
llama-bench one test per invocation, `-r 6`):

| | llama.cpp | us | |
|---|---|---|---|
| prefill 512 | **439 ms** (1166.30 ± 8.31 t/s) | packed **6407 ms** / int8 **6263 ms** | 14.6x / 14.3x |
| decode @ 512 depth | **27.5 ms/tok** (tg64@d512: 36.42 ± 0.17 t/s) | **34.5 ms/tok** both modes | **1.26x** |
| decode, shallow | 24.94 ms/tok (tg128: 40.09 ± 0.12 t/s) | 26.0 (prior record) | 1.04x |

The shallow-context parity row STILL holds, but `llama-bench -d` (depth)
shows the honest 512-depth decode gap: 27.5 vs 34.5 ms/tok — their
attention tail degrades 25 -> 27.5 going 0 -> 512 deep; ours degrades
26 -> 34.5. Decode-at-depth is attention-tail bound, which feeds the
decode-attribution explore, not the GEMM work.

**Re-measured 2026-08-24 evening, post-epilogue (xpu-coopmatrix-
epilogue closed):** mixed Q4_K_M prefill **2791 ms** (Q4_K spill 1570 +
Q6_K register-epilogue 341; 6.4x vs llama.cpp's 439). Pure Q6_K
head-to-head (fresh requant of the local Q8_0): llama.cpp **580 ms**
(882.16 ± 37.62 t/s — their MMQ DECLINES Q6_K at ne11=512 on RDNA3.5
and runs dequant+rocBLAS, 32% slower than their own Q4_K_M) vs ours
**3206 ms**, all 224 calls on `wmma6-epi` — **5.5x, the closest gap on
record**. The register-epilogue verdict was split by measurement: Q6_K
-35% (shipped as default); Q4_K epi SLOWER than its spill (the dmin
term does not factor into the verb shape cheaply) — parked behind the
`q4epi` arm for the 10.12.21 re-tiling.

## 9.2 The three ceilings, each validated

A ceiling probe is only worth its number if the loop was not hoisted, so
each doubles its iteration count and checks that the TIME doubles.

| path | TMAC/s |
|---|---|
| our prefill GEMM today | 1.08 |
| llama.cpp prefill achieves | 9.3 |
| `dotAccum` ceiling | **9.9** |
| WMMA float16 | 26.8 |
| WMMA int8 | **27.0** |

Two consequences that set strategy:

- **`dotAccum` cannot beat llama.cpp.** Its ceiling is 9.9 and they
  already achieve 9.3. Instruction-efficiency work on that kernel is a
  parity play.
- **The matrix cores can.** `int8` and `float16` are NATIVE on amdgpu —
  the `[mma-tiering]` notes name only bfloat16, float32 and float64 as
  software-tier. This is answered at COMPILE time and costs nothing to
  ask; ask it before writing a kernel, not after.

An independent implementation (llama.cpp) landing at 9.3 against our
probe's 9.9 is mutual confirmation that ~10 TMAC/s is this chip's real
dp4a rate. An earlier "2-4% of peak" claim extrapolated from a vendor
spec sheet and was wrong by 3x.

## 9.3 What is on the device

- **Decode**: the whole layer. RMSNorm, both residual adds, SwiGLU, all
  seven projections, RoPE, the KV append and the attend. Nothing in a
  decode layer crosses the bus.
- **Prefill**: the projections (batched, WMMA for Q4_K) and the attend.
  RoPE and the paged-cache append stay on the host.
- **`Prim.attendF32` is NOT routed and must not be.** It gives one work
  item per OUTPUT ELEMENT, so every `headDim` lane of a head recomputes
  the same `count x headDim` score dot and each K read is a broadcast of
  one float — 11.0's one-row-per-work-item shape in its worst form.

## 9.4 Hypotheses that were REFUTED by measurement

Recorded because each is plausible enough to be tried again.

- **Attention is a rounding error.** It measured 3 ms of a 32 ms token
  and was ranked minor. That was an artifact of an 8-token bench prompt:
  at 128 positions it is 20 ms, at 512 it is **97 ms of a 125 ms token —
  77%**. The projections are FLAT at ~29 ms throughout, as they must be.
  Attention is the only part of a decode token that grows.
- **The prefill GEMM is bandwidth-bound.** Four tokens per lane cuts
  passes over the weights from 16 to 4 — a 4x traffic cut — and measured
  13% SLOWER. It is issue- and occupancy-bound.
- **Redundant decode is the GEMM's cost.** Every lane unpacks the same
  16-byte header identically. Removing the scale unpack entirely: ~1%.
  Removing the nibble split: 4%. Total decode ~5%.
- **The combine pass dominates the attend.** It runs one workgroup per
  head, 32 on a part with 160 SIMDs, so occupancy said it would. It is
  the CHEAPER half by 2.5x: combine 1.53 ms, score 3.90 ms.
- **Widening amortization pays.** The WMMA B tile depends on
  `(output rows, block)` and never on tokens, so at 512 tokens 32
  workgroups widen byte-identical tiles. Sweeping several token tiles per
  workgroup is monotonically WORSE — 3021 / 3191 / 3644 ms at 1 / 2 / 8
  tiles — because LDS goes 7.6 -> 19 KB and the grid shrinks 8192 -> 1024.
  **The redundant widening is cheaper than the occupancy it costs to
  avoid.**
- **OS page cache or L2/L3 explains llama.cpp's prefill.** It cannot: a
  batched 512-token prefill reads the weights ONCE, 23 ms at ~200 GB/s,
  about 5% of their 440 ms. Both timings exclude model load. 4.58 GB
  caches for nobody against a 32 MB MALL. Both are compute-bound.

## 9.5 The prefill GEMM, attributed

prefill(512) = 7.89 s at the time of attribution:

| phase | ms | share |
|---|---|---|
| GEMM launch | 3514 | 45% |
| pack (host q8_K) | 859 | 11% |
| download | 520 | 7% |
| copyOut (host copy) | 327 | 4% |
| upload | 104 | 1% |
| unaccounted (norms, glu, rope, attend, adds) | 2566 | 33% |

Within the GEMM: bandwidth 0% (negative), decode ~5%, activation loads
~15%, **~70% loop control, addressing, issue and latency**.

A supporting datum: making ONE 8-iteration loop's trip count
runtime-dependent cost **55%** of the kernel (3588 -> 5583 ms) by
defeating its unroll. That is the strongest evidence for issue-bound.

## 9.6 LONG CONTEXT — the blockers, computed

Two independent axes. Weight reads depend on the MODEL only (4.6 / 18.7 /
40.3 GB for 8B / 30B / 70B at Q4_K). Arithmetic depends on the PRODUCT,
params x tokens. Memory depends on the PROMPT, and that is where it
fails.

| model | ctx | MACs | WMMA floor | KV f16 | arena | score matrix |
|---|---|---|---|---|---|---|
| 8B | 8k | 66 T | 2.4 s | 1.0 GB | 1.7 GB | **8 GB** |
| 8B | 250k | 2,007 T | 74 s | 30.5 GB | 51.5 GB | **7,451 GB** |
| 70B | 250k | 17,650 T | 654 s | 76.3 GB | 101 GB | **14,901 GB** |

**9.6.1 The prefill attend is O(n^2) in MEMORY.** It materializes
`rows x heads x ctx` scores. 33 MB at 512 tokens, 8 GB at 8k, 7.4 TB at
250k. The current implementation caps out around **4-8k tokens**.
Chunking does not rescue it: at chunk 512 / ctx 250k / 64 heads it is
still 33 GB. **Flash attention — online softmax that never materializes
scores — is REQUIRED, not an optimization.**

**9.6.2 The arena is O(n).** 51.5 GB at 250k for 8B, 101 GB for 70B,
past the 96 GB GTT. Prefill must be chunked. `stepSeq` already takes a
chunk and the prefill attend already honours a nonzero `startPos`
(`thePrefillAttendReadsHistoryBeforeItsChunk`).

**9.6.3 KV must become f16 then quantized.** `DeviceKv` allocates FLAT
f32 planes sized to `maxSeq` — 2x the f16 figures. At 70B/250k even f16
KV (76 GB) plus weights (40 GB) exceeds the box.

**9.6.4 At 1M tokens attention DWARFS the weights by 9-16x**, and no
kernel work changes it:

| model | weight MACs | attention MACs | ratio | attention at the 27 TMAC/s ceiling |
|---|---|---|---|---|
| 8B | 8,030 T | 131,072 T | 16x | **1.3 h** |
| 70B | 70,600 T | 655,360 T | 9x | **6.7 h** |

Memory at 1M with GQA already assumed: 8B needs 122 GB f16 / 31 GB q4
KV (fits with weights at 35 GB); 70B needs 305 GB f16 / 76 GB q4 (117 GB
with weights — **does not fit**).

**Dense causal attention does not reach 1M.** Models that advertise it
change the mechanism: MLA (DeepSeek, ~90% KV reduction), interleaved
local/global (Llama 4 iRoPE, Gemma 5:1, Mistral SWA), attention sinks
(StreamingLLM), sparse patterns. Realistic ceiling for the dense
Llama-class models we support on this box is **128k for 8B**.

**9.6.5 We refuse the mechanism that would help.** `PagedKvCache`
already has sliding-window support — `layerWindow`, `ringK`/`ringV`, the
`from` clamp — for Gemma and Mistral. `DeviceKv.supports()` explicitly
REFUSES windowed layers, because flat planes have no ring indexing. So
the device attention path rejects exactly the mechanism long-context
models rely on.

## 9.7 MoE — scoped, not built

We have **zero** MoE support: no `expert` config key, nothing in the
model code. llama.cpp has 14 MoE architectures (Mixtral, Qwen2/3/3.5,
DeepSeek/V2, Llama4, OLMoE, PhiMoE, GraniteMoE, HunyuanMoE, OpenAI-MoE,
BailingMoE, SmolLM3), the `GGML_OP_MUL_MAT_ID` indexed-matmul primitive,
and `build_moe_ffn`.

Their `mmq.cu` carries `ids_dst`/`ids_src`/`n_expert`, so **MoE prefill
runs on the matrix cores with the expert indices threaded into the tile
ADDRESSING** rather than physically permuting activations — and
`ggml-cuda.cu` fuses `ffn_up`/`ffn_gate`/GLU when all are `MUL_MAT_ID`.
That is a generation ahead of the gather-then-GEMM shape we would reach
for first.

For us the constraint is concrete: `ma.load(buf, offset, layout, stride)`
needs a CONSTANT row stride, and MoE rows are scattered. Either we
gather each expert's tokens into a contiguous staging buffer, or
`CooperativeMatrix` gains an indexed load — a compiler-side question to
settle before writing the kernel.

Note also that the pre-dequantize plan (9.8) is a DENSE optimization and
fights MoE directly: most experts are cold most of the time, so it would
need to be lazy and hotness-driven, which couples it to an expert cache.

## 9.8 Next, in order

1. **Pre-dequantize weights to int8 once** (dense). The LDS amortization
   failed on occupancy; a global int8 copy is not blocked that way —
   `mb.load` takes `layout = 1` (column-major), so a `[outDim x cols]`
   int8 buffer feeds fragments straight from global with NO staging and
   NO barriers. Feed cost goes to zero rather than being divided. Price:
   4.58 -> 8.03 GB, and the packed copy must STAY because decode is
   bandwidth-bound and would get ~1.75x slower on int8. 12.6 GB resident.

   **Amended 2026-08-24: prefill weight residency is a USER POLICY with
   two modes, not an unconditional upgrade.** The int8 copy trades ~2x
   weight memory for GEMM speed, and not every machine has this box's
   96 GB GTT. Requirements:

   - `packed` (DEFAULT) — the shipped 10.12.14 kernel: per-block widen
     into 4 KB of LDS, zero extra bytes. This is exactly llama.cpp's
     shape — their `mmq.cuh` `load_tiles_q4_K` stages still-quantized
     blocks into SRAM per tile and never materializes a dequantized
     weight copy (verified in source, 5306f4b). Default because no user
     should get a surprise 2x weight footprint.
   - `int8` — the resident widened copy, chosen explicitly by the user
     (`EngineOptions.prefillWeights`). Decode reads the packed copy in
     BOTH modes; the policy touches only the batched prefill route, and
     the route diagnostic must say which kernel ran.
   - Auto-selection by VRAM size is BLOCKED on the device-profile work
     (`Device` cannot report memory geometry today) — explicit option
     first, auto later.
   - llama.cpp's third trick — transient per-op weight copies from a
     reuse pool (`ggml_backend_cuda_device_offload_op`, batch >= 32) —
     is a known middle mode, NOT built: our 32-row tiling touches each
     tensor ~16x per 512-token prefill, so it needs a once-per-pass
     scheduling seam nobody currently needs.
   - MoE (9.7) fights the resident copy (cold experts); the policy
     surface must not preclude going per-tensor/hotness-driven later.

   **OUTCOME (2026-08-24, plan 10.12.16/.19/.20):** shipped as the
   policy above, with the design's own feed hypothesis part-refuted:

   - The flat `[outDim x cols]` + `layout=1` form was 52% SLOWER
     (strided global fragment gathers); the shipped copy is TILE-MAJOR
     (each 16x16 fragment one contiguous 256-byte run).
   - `int8` measures launch 3015 -> 2855 ms (-5.3%), prefill 6385 ->
     6233 ms (-2.4%); decode 35.9 ms/tok in both modes. Killing the
     whole B feed bought only 160 ms — **the LDS staging was never the
     bottleneck**, so the remaining gap to llama.cpp lives in the scale
     SEAM (8 accumulator spills + 16 barriers per block) and the
     single-wave tiling.
   - CORRECTION to 9.2's framing: llama.cpp's 9.3 TMAC/s prefill on
     this box is **WMMA MMQ, not dp4a** — `amd_wmma_available` covers
     RDNA3/3.5 in the vintage benchmarked, and their kernel applies
     Q4_K scales per accumulator fragment element in REGISTERS. Ours
     cannot: `CooperativeMatrix`'s accumulator is opaque. That compiler
     feature (fragment element access) is the gating next lever.
   - The amdgpu test suite had NEVER compiled device code before this
     work (`run-tests.sh` reads `XPU_BACKEND`; sweeps set
     `CAJETA_XPU_BACKEND`) — 3 device tests had latent shape bugs that
     fired the first time they truly ran. Fixed; 194/0/1 on a real
     amdgpu compile now.
   **OUTCOME 2 (2026-08-24 evening, plan 10.12.21):** the fragment
   epilogue landed as `scaledAccumInto`/`rank1Accum`/`scaledAccumInto2`
   (compiler 8f43a68a) and the full llama.cpp shape was rebuilt in two
   measured stages — with the DIAGNOSIS INVERTED by the second one:

   - `q4kWmmaDeqMwKernel` (8 waves/WG + register epilogue + barrier-free
     K-loop) cut issues/mma-pair ~150 -> ~55 and bought only 5% (kernel
     1153 -> 1097 ms): the kernel was at the MEMORY WALL (~155 GB/s),
     because each 16-token tile re-reads the whole weight matrix — 32x
     per 512-token prefill, ~163 GB. Issue economy was never the
     constraint; ARITHMETIC INTENSITY was. This closes the "2.65 vs 27
     TMAC/s" question: the ceiling was compute-validated, but the
     single-token-tile shape never had the intensity to reach it.
   - `q4kWmmaDeqMw4Kernel` (TOKEN BLOCKING, llama.cpp's `mmq_x`): four
     token-tiles per WG share every weight fragment — weight traffic /4.
     Kernel 1097 -> 784 ms, prefill 1857 -> 1550 ms (-16.5%), and the
     kernel now runs UNDER the wall (~84 GB/s implied): the constraint
     moved back to latency/occupancy. Remaining kernel order: Q6_K twin
     of the same transform (323 ms at 152 GB/s today), then T=8.
   - Prefill(512) 2026-08-24 arc: 6770 -> 1550 ms (4.4x). llama.cpp:
     439 ms — gap 3.5x, attend (208 ms) and widen (110 ms) now rank
     beside the GEMM (784 ms) and Q6 (323 ms).
2. **Flash-attention prefill** — removes the O(n^2) score buffer (9.6.1).
3. **Chunked prefill** — the arena (9.6.2).
4. **f16 / quantized KV** — (9.6.3).
5. **Windowed layers on the device path** — (9.6.5).
6. **MoE** as its own spec (9.7).
