# Cajeta XPU — CPU backend + graceful degradation (bring-up plan/log)

The fourth backend, and the one that makes "run anywhere" literally true: when no
GPU/accelerator (XPU) is present, the same portable `@Kernel` source must execute on
the CPU. This is the AMD/Vulkan discipline applied once more — thread a CPU target
through the existing `LoweringTarget` seam, let the duplication reveal what the CPU
actually forks, design nothing abstract up front.

## 0. Decisions (locked 2026-05-30)

Four design choices, made deliberately:

1. **CPU is a real *vectorized* backend, built first — not an emulation upgrade.**
   The CPU target lowers `@Kernel` to native host code where the **wave is a SIMD
   register and the lane is a vector element**, the grid is a threaded iteration
   domain, and LLVM carries it to AVX-512 / AMX / NEON / SVE. (Contrast: the existing
   "CPU emulation" — `XpuLaunchTests.saxpyCpuEmulationEndToEnd` — is *not* an XPU
   backend; it compiles a `@Kernel` as an ordinary host method with an explicit
   `uint32 i` index param and a serial host loop. It cannot run a portable GPU-style
   kernel (`Buffer<T>` + `Thread.globalIdX()`), so it is the legacy path, separate from
   this backend.)

2. **Fall-to-CPU triggers on *availability at startup*, cached — no mid-run recovery.**
   The dispatcher probes backends once at first launch, picks the best available,
   caches it. A GPU that is present but fails mid-run (device lost, OOM) is a **hard
   error**, not a silent CPU re-run — keeps us out of the partial-device-state swamp.

3. **Bundling is *explicit only*.** A binary embeds exactly the backends listed on the
   CLI (so `--xpu-backend` becomes a *list*; registration/dispatch hold N variants per
   kernel keyed by backend). Nothing auto-injected. **Consequence, accepted on purpose:
   degradation is a build-time contract, not a runtime guarantee** — a binary built
   `--xpu-backend=vulkan` shipped to a GPU-less box *fails hard*, because there is
   nothing to fall to. A must-run-anywhere artifact lists `cpu` among its targets
   (`--xpu-backend=vulkan,cpu`). The humane requirement: when no bundled backend is
   available, the dispatcher must say so precisely — *"no available backend among
   {vulkan}; rebuild with `cpu` to enable CPU fallback"* — never crash opaquely.

4. **CPU is the lowest-priority backend on the same seam, and its `available()` is
   always true.** That turns "degrade to CPU" into "the priority chain reached its
   guaranteed terminal" — ordinary dispatch, not a special case. Probe order:
   `CUDA → HIP → Vulkan → CPU`. `@Backend(...)` constrains a kernel's candidate set;
   `CAJETA_XPU_BACKEND=cpu` forces it (and is a debugging superpower — run "GPU"
   kernels deterministically on the CPU).

## 1. The variance surface — what CPU adds to the seam

GPU backends are SIMT: the hardware runs one scalar lane per work-item and supplies
coordinates via intrinsics. The CPU has **no hardware grid and no coordinate
intrinsics** — so the fork is the *coordinate source* and the *execution model*, not
the body:

| Seam point | GPU (NVPTX/AMDGPU/SPIR-V) | CPU (`CpuTarget`) |
|------------|---------------------------|-------------------|
| `allocaAddressSpace` | 0 / 5 / 0(Function) | **0** (flat host) |
| `createKernel` | buffers `ptr addrspace(1)` + scalars | buffers `ptr addrspace(0)` + scalars **+ 9 trailing `i32` coordinate params** (`tid.{x,y,z}`, `ctaid.{x,y,z}`, `ntid.{x,y,z}`) — the grid→threads model |
| `threadId`/`workgroupId`/`workgroupDim` | hardware sreg intrinsics | **read the trailing coordinate args** |
| `globalId` | native or default | shared default `ctaid*ntid + tid` (works verbatim) |
| `materializeParam` / `bufferElementPtr` | default (arg / GEP) | **default reused** — host pointers are addrspace 0, plain GEP is correct |
| `decorateKernel` | ptx/amdgpu CC + markers | no-op (default C calling convention, external linkage for JIT/AOT lookup) |
| `workgroupBarrier` | hardware barrier | **work-item loop fission (Inc 6 ✅)** — emits a `@__cajeta_xpu_cpu_barrier()` marker; the registration pass splits the kernel at each barrier into regions, each looped over the block, with per-block shared memory + context arrays. Unsupported shapes → `XPU-N02` host-stub fallback. |
| `waveWidth`/`Shuffle`/`Ballot`/`ReduceSum` | hardware wave intrinsics | **first cut: width 1** (one work-item per invocation): `width→1`, `shuffle→value`, `ballot→zext(pred)`, `reduceSum→value` — the honest single-lane semantics, matching the existing `__cajeta_xpu_wave_*` runtime stubs. True wave=SIMD-lane is a later increment (§2, Inc 5). |

**What stays shared:** the entire kernel-body AST walk, control flow, the operator set,
the mutable scalar-slot model, casts. The coordinate reads route through `threadId`
etc. and the buffer GEP through `bufferElementPtr` — both already seam points — so the
body is untouched. This is the same ~90% core, now spanning four backends.

## 2. Increments

### Increment 1 — `CpuTarget` over the shared lowerer (Tier-0) ← **in progress**
- `src/cajeta/xpu/cpu/CpuKernelLowering.{h,cpp}` — `CpuTarget : LoweringTarget` with
  the grid→threads coordinate model above; `cpu::lowerKernel(method, module)` wrapper;
  `cpu::configureHostModule` (native triple + DataLayout) + `createCpuTargetMachine`.
- Test `test/xpu/XpuCpuEmitTests.cpp` — lower the **portable** SAXPY
  (`Buffer<float32>` + `Thread.globalIdX()`) and assert: addrspace-0 buffer params, the
  9 trailing coordinate params, coordinate reads come from args (no `nvvm`/`amdgcn`/
  `spv` intrinsics), plain GEP, `ret void`. First CPU overlap data point.
- Stretch (same increment if the JIT infra is light): `XpuCpuDeviceTests.cpp` —
  JIT-compile the lowered module, loop the host driver over the grid, verify
  `y[i] = a*x[i] + y0[i]`. The CPU *oracle* the GPU backends can be diffed against.

### Increment 2 — `CpuBackend` codegen + enum/CLI/registration (Tier-0) ✅ landed 2026-05-30
- `src/cajeta/xpu/cpu/CpuBackend.{h,cpp}` — host `TargetMachine` (moved here from
  CpuKernelLowering to match the other backends' structure) + `emitObject` (native `.o`).
- `Backend::Cpu` (xpu layer) + `XpuBackend::Cpu` (compiler) + `XpuEmit::Object`;
  `--xpu-backend` is now a **comma-separated list** (`vulkan,cpu`), stored as
  `vector<XpuBackend>` with `addXpuBackend`/`usesXpuBackend`; each selected backend uses
  its own default arch (a single `--xpu-arch` can't serve all).
- `CpuRegistration.{h,cpp}` — the CPU registration is structurally *different* from the
  GPU ones: a CPU kernel **is** host code, so there is no blob to embed. It lowers each
  kernel into a fresh module sharing the host context, decorates the symbol
  (`__cajeta_xpu_cpu.<name>`), **links it into the host module**, and emits a ctor calling
  the new runtime hook `__cajeta_xpu_register_cpu_kernel(name, fnptr)` (+ a tiny
  name→pointer registry + `__cajeta_xpu_lookup_cpu_kernel` in `cajeta_runtime.c` for the
  Inc-4 dispatcher). A mid-lowering `XPU-N01` discards the fresh module, host untouched.
- Tests `XpuCpuAotCliTests`: pure-CPU registration in the `.ll`; **`vulkan,cpu` bundles
  both** a SPIR-V blob registration *and* a CPU host-kernel registration in one module;
  `--xpu-emit=obj` drops a valid ELF object.

### Increment 3 — generic `CpuDriver` + the launch ABI (Tier-1, serial) ✅ landed 2026-05-31
- `src/cajeta/xpu/cpu/CpuDriver.{h,cpp}` — `available()` ≡ true (the guaranteed
  terminal of the priority chain); `launch(name, argv, gridX, blockX, …)` resolves the
  kernel's registered launcher thunk by name and runs the grid→threads loop, calling the
  thunk per work-item with the kernelParams `argv` (shared) + that work-item's `i32[9]`
  coordinate vector. Buffers ARE host pointers (no alloc/transfer). On-CPU SAXPY green.
- **The launch ABI is the measured CPU launch fork.** A CPU kernel can't be called from a
  bare `void*`, so registration emits a uniform **launcher thunk**
  `__cajeta_xpu_cpu_launch.<name>(void** argv, const i32* coord)` — the CPU's
  `kernelParams` ABI, mirroring `cuLaunch`/`hipModuleLaunch` (`argv[i] → &arg_i`).
  It's reconstructed from the kernel's `FunctionType` alone (the last `kNumCoordParams`
  i32s are coordinates; pointer params ⇒ buffer ptr, value params ⇒ scalar), so nothing
  new threads through the seam. The thunk pointer is what `__cajeta_xpu_register_cpu_kernel`
  registers, giving the driver/dispatcher one signature for every kernel.
- **Serial.** *All* parallelism — multi-core threading **and** true wave=SIMD-lane — is
  Increment 5 (decision 2026-05-31): through an opaque function-pointer call LLVM cannot
  auto-vectorize the work-item loop, so threading and SIMD belong together there.

### Increment 4 — the runtime dispatcher + explicit multi-target bundling
The active backend is chosen once at first device touch among the **bundled** set (a
manifest of `__cajeta_xpu_register_backend` ctors the compiler emits) and the
**available** set (runtime probes), priority `CUDA→HIP→Vulkan→CPU`, honoring a
`CAJETA_XPU_BACKEND` force-override, then cached. Every device entry point
(`__cajeta_xpu_buffer_*`, `__cajeta_xpu_launch`, `stream.sync`) routes to it. Staged,
because **the C runtime is the sole launch path** (see findings) — every backend the
dispatcher uses must be wired *in C* there:
- **4.0 dispatcher core** ✅ + **4.1 CPU rung** ✅ (landed 2026-05-31): manifest, cached
  selector, env override, precise *"no available backend among {…}; rebuild with `cpu`…"*
  diagnostic, and the CPU rung (buffer = malloc/memcpy, launch = the launcher-thunk grid
  loop in C). CUDA (already present) refactored behind the switch. GPU-free end-to-end:
  a host-source `kernel.launch()` SAXPY built `--xpu-backend=cpu` runs on the CPU.
- **4.2 HIP rung** ✅ (landed 2026-05-31) — `dlopen` libamdhip64 + the HIP buffer/launch
  block (API-identical to CUDA, shares the module table since one device backend is active
  per run). **Device-validated on the AMD Strix Halo**: a host-source SAXPY built
  `--xpu-backend=amdgpu` runs on the GPU via HIP, and `--xpu-backend=amdgpu,cpu` with
  `CAJETA_XPU_BACKEND=cpu` falls to the CPU even with the GPU present — real-hardware
  degrade-to-CPU.
- **4.3 Vulkan rung** ✅ (landed 2026-05-31) — `VulkanDriver.cpp` ported to C (instance/
  device/compute-queue/cmd-pool bring-up, host-coherent buffer table, descriptor-set
  compute launch), guarded by `__has_include(<vulkan/vulkan.h>)` and `dlopen`-resolved (no
  link dependency). The launch-ABI fork is handled by **per-kernel parameter metadata**
  (`__cajeta_xpu_register_kernel_params`, emitted by the Vulkan registration from
  `collectKernelParamInfo`) + an **argv→descriptor-set translation**: buffer args bind to
  their storage buffers, scalar args are copied into transient single-element SSBOs (freed
  after the dispatch). Local size is baked into the SPIR-V (`kVulkanLocalSizeX = 64`), so
  the launch dispatches `gridX` work-groups. **Device-validated on RADV**: host-source
  SAXPY `--xpu-backend=vulkan` runs on the GPU; `vulkan,cpu` + `CAJETA_XPU_BACKEND=cpu`
  falls to the CPU. **All four backends now route through the one dispatcher.**

### Increment 5 — parallelism: multi-core threading + true wave = SIMD lane
- **5A — Multi-core threading** ✅ (landed 2026-05-31). The runtime's CPU launch
  (`cajeta_xpu_launch_cpu`) now fans the gridX blocks across `min(gridX, cores)` **pthread**
  workers (pthreads, not the cooperative fiber carrier, and not C++ `std::thread` — the
  launch lives in the C runtime); the calling thread runs the last slice while the others
  work, then joins. Each worker owns its `coord[9]`, and a launched CPU kernel is always
  barrier-free (`XPU-N01`) so the grid is embarrassingly parallel — race-free for any kernel
  correct on a GPU. Serial below a 4096-work-item threshold (fan-out costs more than a small
  launch saves) and with one core / one block. `CAJETA_XPU_CPU_SERIAL=1` forces
  single-threaded (deterministic debug/oracle mode + the serial benchmark baseline).
  **Measured ~8× on 32 logical cores** (a compute-bound 1M×3000-iter kernel: 9.56 s serial →
  1.21 s parallel, identical result). Tests: `XpuCpuDispatchTests.saxpyLargeGridParallelOnCpu`
  (a 65536-work-item grid through the dispatcher, result matches serial exactly).
- **5B — SIMD via loop-vectorization (POCL model)** ✅ (landed 2026-05-31). Rather than a
  from-scratch whole-function vectorizer, the kernel body is wrapped in a per-**block**
  work-item loop and handed to LLVM's mature, divergence-aware `LoopVectorize` — the model
  OpenCL CPU drivers use. The per-work-item kernel (`__cajeta_xpu_cpu.<name>`, the shared
  lowerer unchanged) is marked `alwaysinline`; a per-block wrapper
  `__cajeta_xpu_cpu_block.<name>(real params…, ctaid.{x,y,z}, ntid.{x,y,z})` loops `tid.x`
  over `[0, ntid.x)` calling it; the kernel is inlined into the loop and a focused
  pass pipeline (mem2reg → loop-rotate → LoopVectorize → SLP → instcombine → simplifycfg,
  `compile/Optimizer.cpp`) vectorizes it. The launch ABI shifts to **per-block** (the thunk
  + `cajeta_xpu_launch_cpu` + `CpuDriver` call once per block; the wrapper owns the
  work-item loop), composing with the 5A pthread fan-out (threads run block ranges, each
  block is a vectorized loop).
  - **The host TargetMachine now targets the native CPU + features** (`getHostCPUName` +
    `getHostCPUFeatures`) so TTI exposes AVX2/AVX-512 — without it LoopVectorize leaves the
    masked store scalar. Tradeoff: AOT objects are tuned to this machine (a `--cpu-arch`
    baseline knob for portable binaries is a later refinement).
  - **A general optimization pipeline landed too**: cajeta ran *zero* IR optimization on
    user code; `--opt=O0|O1|O2|O3` (default O0; `--release`→O2, `--fast`→O3) runs the full
    per-module pipeline in `emitForModule`. Kernel vectorization is independent of `--opt`
    (always on). The JIT test path stays unoptimized, so existing JIT correctness tests are
    unaffected — but the dispatch/driver tests now run the *vectorized* kernel, validating
    its correctness.
  - **Measured:** a compute-bound 1M×3000-iter kernel went 9.56 s (scalar serial) → 3.65 s
    (SIMD serial) → **0.138 s (SIMD + 32-thread parallel) — ~69× total**, AVX-512
    `<16 x float>` confirmed in the IR. Tests: `XpuCpuAotCliTests.cpuBackendVectorizesBlockWrapper`
    (divergence-free kernel → SIMD ops in the wrapper, host-robust) + the dispatch/driver
    suite on the vectorized kernel.
- **5C — wave-op SIMD via the LLVM Vector Function ABI** ✅ (landed 2026-05-31). Wave ops are
  cross-lane, so `LoopVectorize` bails on them (5B left them width-1). 5C makes the wave = the
  SIMD vector: each `Wave.*` lowers to a **scalar call** to its `__cajeta_xpu_wave_*` stub, and
  the CPU registration pass gives that stub a **Vector Function ABI variant** — a small SIMD
  function LLVM 22's `VFDatabase` reads off the `vector-function-abi-variant` attribute and
  `LoopVectorize` *substitutes* when it widens the per-block work-item loop. We write no
  vectorizer — only the per-op variant bodies (synthesized in IR, so they legalize to real SIMD
  on any host):
  - `reduceSum` → `broadcast(llvm.vector.reduce.add(v))`; `ballotSync` →
    `broadcast(zext(bitcast <W x i1> → iW))`; `shuffleSync` → a per-lane gather `val[src[i]]`.
  - **Non-degrading by construction.** The op is a scalar call with a variant *attribute*;
    if vectorization fires it uses the variant (wave = W), if not the scalar width-1 stub runs
    — always correct, never silently wrong. The mangled name uses the target-independent
    `_LLVM_` ISA token (LoopVectorize matches on VF + parameter shape, not ISA).
  - **Wave width W = the host's native SIMD width** (16 on AVX-512, 8 on AVX2), read from the
    host TTI (`getRegisterBitWidth(FixedWidthVector)/32`). The work-item loop is *forced* to
    that VF (`llvm.loop.vectorize.width` + `.enable`) so the loop VF equals the variant VF.
  - **Divergence → masked variants.** The idiomatic `if (i < n) { …reduceSum… }` predicates the
    block; LoopVectorize then uses the **masked** variant (`_Mv16(<W x …>, <W x i1> mask)`),
    passing the active-lane predicate — GPU active-mask semantics (reduce/ballot over active
    lanes only). Tail handling is the kernel author's job (block a multiple of W, as on GPU);
    tail-folding the whole loop was rejected — it scalarizes the loads (slow) where the common
    full-block path stays clean wide loads.
  - **`Wave.width()` and `Wave.laneId()` — the queryable environment.** `width()` takes no
    argument, so it cannot carry a VFABI variant (a 0-param variant is rejected by the
    verifier); instead it is **rewritten to the constant W** in a vectorized wave kernel (W is
    the architectural width, constant across active and inactive lanes, like a warp size).
    `laneId()` (new across the seam + all four backends — NVPTX `%laneid`, AMDGPU `mbcnt`,
    SPIR-V `SubgroupLocalInvocationId`, CPU `tid.x % W`) gives the lane index; `isFirstLane()`
    (= `laneId() == 0`, lowered for every backend) is the width-agnostic cooperation guard.
    **The rule: never hardcode the wave width — query `Wave.width()` / `laneId()`.** No width
    constant is exposed; the same wave-cooperative source is correct on NVIDIA (32), AMD
    (32/64), Vulkan (runtime), and CPU (16/8).
  - Tests: `XpuCpuWaveSimdTests` (reduceSum/divergent-masked/ballot/shuffle/width/laneId/
    isFirstLane, each width-agnostic — probes the real W then verifies against it);
    `XpuWaveEmitTests` (per-backend `laneId` intrinsics + spirv-val); `XpuWaveDeviceTests`
    (`laneId` on real AMD + Vulkan hardware).
- Probe AMX / SME matmul lowering (CPU matrix engines, runtime-feature-gated).

### Increment 6 — workgroup barriers via work-item loop fission (POCL-style) ✅ (landed 2026-05-31)
A `@Kernel` with `Barrier.workgroup()` can't run as one work-item loop: a barrier means
every work-item of the block must reach it before any proceeds. POCL-style **fission**
(`xpu/cpu/CpuBarrierFission.cpp`, invoked from `CpuRegistration`) splits the kernel at each
barrier into **regions** and wraps each in its own loop over the block — and the serialized
region loops honor the barrier for free, because **a per-block wrapper call runs on exactly
one pthread** (5A fans *blocks* across threads, never the work-items of one block). This
finally unblocks shared-memory tree reductions on the CPU. Barriers were `XPU-N01`; the new
diagnostic for an *unsupported* barrier shape is `XPU-N02`.
- **The barrier becomes a marker.** `CpuTarget::workgroupBarrier` emits an impure,
  `noinline`/`noduplicate` `call void @__cajeta_xpu_cpu_barrier()` (instead of throwing) that
  survives lowering as a region delimiter; the pass erases every call once it has split.
- **The pass** (operating on the per-work-item kernel cloned into the wrapper):
  1. `CloneFunctionInto` the kernel body into the wrapper, mapping `tid.x` → a placeholder
     replaced per region with that region's work-item-loop index, `tid.y/z` → 0 (the launch
     ABI is 1-D), `ctaid/ntid` → wrapper args. Connect entry→body so `DominatorTree`/`LoopInfo`
     see the CFG (the load-bearing fix — without the entry terminator, the body is unreachable
     and *no loops are found*).
  2. Split each barrier into its own boundary block; build `LoopInfo`.
  3. **Region formation is a loop-aware structured walk**, not SESE/dominance: a region is a
     maximal barrier-free run within one loop level; a uniform loop that contains a barrier
     stays the **outer scalar scaffold** (its header/latch run once per iteration) and its body
     region is wrapped in a work-item loop **nested inside**. So the tree reduction's
     `for (s)` loop is preserved and the halve-and-add (region B) loops over the block within it.
  4. **Context save/restore at the alloca-slot level** (pre-mem2reg, the key simplification —
     no SSA liveness analysis): a local that is **per-work-item** (a `tid.x`-tainted value is
     stored to it) **and** lives across a barrier (accessed in >1 region) is widened from `T`
     to a `[ntid.x × T]` array indexed by the region's work-item index. Block-uniform locals
     (the loop var `s`, kernel params) stay scalar; region-local temps stay scalar and mem2reg
     promotes them. The generated work-item loops are clean counted loops, so 5B's
     `LoopVectorize` still widens them to SIMD.
  5. **Per-block shared memory.** A `Shared<T>` array lowers to one module-level `addrspace(3)`
     global — one instance, which would race across the blocks/threads that each call the
     wrapper. The pass replaces it with a wrapper-local `alloca` (fresh per block call),
     `addrspacecast` 0→3 (a no-op the CPU backend folds), and deletes the dead global.
- **Guardrails (`XPU-N02`, checked before any mutation → host-stub fallback, never a
  miscompile):** a barrier under work-item-divergent control flow (post-dominance check — the
  barrier must post-dominate its level's entry); a barrier in a loop with a `tid`-dependent
  trip count; a barrier in a nested loop (one level — *lifted in Inc 8*); dynamic-sized shared
  memory (*lifted in Inc 10*); wave ops + barriers in one kernel (the 5C forced-VF path and
  fission — *composed in Inc 9*).
- **Verified:** `XpuCpuBarrierExecTests` — two straight-line regions run the whole block;
  per-block shared memory staged + read cross-lane; the **canonical tree reduction**
  (`in[i]=i`, 256 block ⇒ `out[0]=32640`) with a barrier inside the uniform loop; 32 blocks ×
  256 across pthread workers with no shared-buffer aliasing; a divergent barrier falls back
  cleanly. Barrier-free CPU + wave (5C) + GPU suites unchanged.
- **Out of scope (later):** ~~multiple barriers / multiple regions inside one loop~~ (Inc 7 ✅);
  nested uniform loops with barriers; composing wave ops with barriers; a register accumulator
  carried per-work-item across the uniform loop (the canonical kernel keeps it in shared memory).

### Increment 7 — multiple barriers / regions inside one uniform loop ✅ (landed 2026-05-31)
Lifts the first Inc 6 scope cut: a uniform loop body may now contain **more than one barrier**,
splitting it into N work-item-loop regions (e.g. a ping-pong stencil `read; barrier; write;
barrier` per iteration). The region walk already iterated multiple barriers — the loop body's
`while (cur)` in the structured walk closes a region at each barrier and continues — so the
*structure* was right; the gap was **correctness of context arrays for a per-work-item local
that crosses an in-loop barrier**.
- **The bug.** Fission widens a per-work-item local that lives across a barrier into a
  `[ntid.x × T]` context array, deciding "per-work-item" by tainting transitively from the
  work-item-index placeholder. But the taint walked **SSA def-use only**, and Cajeta lowers
  `uint32 t = Thread.x()` as a *store to `t`'s slot* followed by a *load* at each use — so the
  SSA chain breaks at the slot. A local like `int x = a[t] + a[(t+1)&255]` is therefore stored
  from a value the taint never reached; `x`'s slot stayed a **single scalar** shared by every
  work-item, so the last lane's `x` clobbered all others (lane 0 read the wrong value). The
  Inc 6 reduction never hit this — its only barrier-crossing per-work-item value was `t` itself
  (directly tainted), everything else lived in shared memory.
- **The fix** (`CpuBarrierFission.cpp`, `computeTaint`): make taint a **fixpoint over SSA
  def-use *and* memory round-trips** — a load from any alloca slot that ever receives a tainted
  value is itself per-work-item; re-propagate until stable. Over-approximation is safe (a
  wrongly-widened uniform slot just gets redundant per-lane storage, same result). Now `x`'s
  slot is recognized as per-work-item and widened to `x.slot.ctx`.
- **Register accumulator across the loop back-edge — covered by the same fix.** A per-work-item
  scalar carried across a uniform loop's iterations (`int acc = 0; for (…) { acc += tile[…];
  barrier; }`) was a separate Inc 6 scope cut, but it has the *same* root cause — `acc` derives
  from `t` through `t`'s slot, so pre-fix it wasn't tainted and stayed a single shared scalar.
  The memory-aware taint widens `acc` to `acc.slot.ctx`, and because a context array is allocated
  once in the wrapper entry and indexed by work-item, it **persists across the outer scalar
  loop's iterations** for free — the accumulation carries correctly with no shared memory for the
  accumulator. So this cut is lifted too.
- **Verified:** `XpuCpuBarrierExecTests.multipleBarriersInOneLoop` (shared-memory ping-pong,
  two barriers per iteration, matches the host recurrence for every lane),
  `localCarriedAcrossInLoopBarrier` (the local-across-an-in-loop-barrier case — the regression
  that exposed the taint gap), and `registerAccumulatorAcrossLoopBackEdge` (a per-work-item
  register accumulator carried across iterations); `XpuCpuBarrierEmitTests.multiBarrierLoopBodySplitsAndWidensLocal`
  (the loop body splits into ≥2 work-item loops nested in the single outer scalar loop; `x` is
  widened to a context array). The Inc 6 single-barrier reduction + all prior CPU/wave/GPU
  suites unchanged.
- **Still out of scope (after Inc 7):** ~~nested uniform loops with barriers~~ (Inc 8 ✅);
  wave ops + barriers in one kernel.

### Increment 8 — nested uniform loops with barriers ✅ (landed 2026-05-31)
Lifts the one-level restriction: a barrier may sit inside a loop **nested inside another loop**
(e.g. a tiled/blocked iteration). The region walk was already recursive — it recurses into each
barrier-containing subloop (`walk(inLoopSucc(L), L, …)`), and `loopHasBarrier` is transitive, so
the outer loop is detected as a barrier-subloop at the top level and the inner one within it.
Each loop that (transitively) contains a barrier stays an **outer scalar scaffold**, nested one
inside the next, with the innermost body's regions wrapped in work-item loops nested as deep as
the loop nest. Context arrays (allocated once in the wrapper entry, indexed by work-item) and
per-block shared memory already persist across every level, so the cross-iteration / cross-level
carry needs no extra machinery.
- **The change** was small: drop the `if (L->getLoopDepth() > 1) unsupported(…)` guardrail, plus
  one defensive check in the walk — if a level starts *directly* on a nested barrier-loop header
  (no separating preheader region) enter it as a subloop rather than letting `collect` flatten it
  (Cajeta's structured loops always have that preheader, so this is belt-and-suspenders for the
  never-miscompile contract). Every other guardrail is unchanged and still per-loop.
- **Guardrails intact:** a barrier in a nested loop with a **work-item-dependent trip count** is
  still rejected (`XPU-N02` → host stub), since different work-items would execute different
  barrier counts (GPU-undefined deadlock); likewise a barrier under divergent control flow.
- **Verified:** `XpuCpuBarrierExecTests.nestedUniformLoopsWithBarrier` (a barrier in a
  doubly-nested loop; both loops stay scalar scaffolds, two `for.head`),
  `nestedLoopsWithOuterLevelRegions` (an outer loop holding a direct barrier-region *and* a nested
  barrier-loop — the `region·barrier·region·barrier·subloop` sequence at one level), and the
  guardrail `nestedTidDependentTripCountFallsBack` (a tid-dependent inner trip count falls back,
  not miscompiles). All prior suites unchanged.
- **Still out of scope (after Inc 8):** ~~composing wave ops with barriers~~ (Inc 9 ✅).

### Increment 9 — wave ops + barriers composed in one kernel ✅ (landed 2026-05-31)
Lifts the last barrier scope cut: a kernel may use `Wave.*` (5C) **and** `Barrier.workgroup()`
(Inc 6) together — the canonical **two-level reduction** (intra-wave SIMD reduce → shared-memory
staging → barrier → cross-wave combine) now runs on the CPU. The two mechanisms were kept
disjoint because 5C forces a vector width and attaches VFABI variants to the *one* work-item
loop, while fission produces *many*. The insight: each fission region is itself a clean counted
work-item loop, so the 5C machinery applies **per region** unchanged.
- **The composition.** `fissionBarrierKernel` now reports the back-edge branch of every region
  work-item loop (`workItemLatches`). The registration pass, after fission, runs the same 5C
  setup it uses for a barrier-free wave kernel — attach the SIMD variants (`setupWaveVariants`),
  rewrite `width()` to the constant W, and `forceLoopVectorWidth(W)` on **each** region latch —
  then vectorizes and folds the substituted variants (`foldWaveVariants`). A wave op inside a
  region becomes W SIMD lanes; the barriers still delimit regions. The 5C variant/fold code was
  extracted into shared helpers so both paths use one implementation. The fission `XPU-N02`
  guardrail that rejected wave+barrier is removed.
- **Semantics.** On the CPU the wave = the W SIMD lanes of one vectorized iteration; a fission
  region loop vectorized at W partitions work-items [0,W),[W,2W),… — the *same* partition in
  every region, so wave membership is consistent across barriers. Forcing VF=W on every region
  loop is safe (the block width is uniform); only wave-bearing regions carry the inherited 5C
  assumption that the block size is a multiple of W (a partial tail wave would run the scalar
  width-1 stub in the loop's scalar epilogue — same limitation as the barrier-free 5C path).
- **Verified:** `XpuCpuBarrierExecTests.waveReduceWithBarrierBlockSum` (two-level block reduction,
  `out[0]=32640` — width-independent, and distinguishes a real wave reduce from the width-1 stub,
  which would give 1920) and `waveReduceWithBarrierMembership` (`in[t]=1 ⇒ out[t]==W` for every
  lane against a probed width — per-wave membership, not just an associative total). All 5C wave,
  barrier, and GPU suites unchanged.
- **Barrier fission scope cuts are now fully lifted** — multiple barriers per loop (Inc 7),
  register accumulator across the back-edge (Inc 7), nested loops (Inc 8), wave + barrier (Inc 9).

### Increment 10 — dynamic (runtime-sized) shared memory + a barrier ✅ (landed 2026-05-31)
Lifts the static-shared-size requirement: a `shared T[runtimeN]` array (sized by a kernel
parameter, not a constant) lowers to an external unsized `[0 × T]` addrspace(3) global, and the
fission pass now allocas it per block from the launch's `sharedBytes:` count instead of throwing
`XPU-N02`.
- **The launch ABI carries the byte count to the CPU wrapper.** `__cajeta_xpu_launch` already
  took `sharedBytes` (cuLaunchKernel's dynamic-shared bytes), but `cajeta_xpu_launch_cpu` dropped
  it. It now threads it: the per-block coord vector gains a 10th slot (`coord[9]` = dynamic-shared
  bytes), the generated launcher thunk loads it and passes it as the per-block wrapper's trailing
  param, and fission's step 5 allocas `alloca i8, i64 sharedBytes` (aligned 16) for an unsized
  shared global and `addrspacecast`s it to addrspace(3). The kernel's typed GEPs index the byte
  buffer unchanged. Every wrapper gains the trailing param (uniform thunk ABI); non-dynamic
  kernels ignore it (DCE). The two thunk callers — the C runtime `run_slice` and the C++
  `CpuDriver` — both write the 10-slot coord.
- **Verified:** `XpuCpuBarrierExecTests.dynamicSharedMemoryWithBarrier` — the canonical tree
  reduction with `shared int32[n]` (runtime `n`), launched `sharedBytes: [1024]` over a 256
  block ⇒ `out[0]=32640`. Full `Xpu*` suite green (137 passed / 7 GPU-skipped / 0 failed);
  static-shared, wave, and device launch paths unchanged (the extra coord slot is additive).

### Wave-width / block-size constraint (documented — not a fixable bug)
The wave-on-CPU model (5C) maps a wave to the **W SIMD lanes** of a vectorized iteration, where
W is the host's native width (16 AVX-512 / 8 AVX2 / 4 SSE). This W differs from a GPU warp
(32/64), so a wave kernel's *partitioning* is platform-specific by construction — only
**width-agnostic** kernels (that query `Wave.width()` and produce a width-independent result, e.g.
the two-level reduction whose block total is the same for any W) are portable. Such kernels need
the **block size to be a multiple of W** to have full waves; a partial tail wave (block not a
multiple of W) runs the scalar width-1 stub in the loop's scalar epilogue. This is **inherent**,
not a miscompile to fix: tail-folding wouldn't make a non-W-multiple block cross-platform-correct
(the kernel's own `block/width` already drops the partial wave), and it is automatically satisfied
by every warp-multiple block size (32/64/128/256 — all divisible by 4/8/16), i.e. by all
GPU-idiomatic launch shapes. Inherited from the barrier-free 5C path; Inc 9 adds nothing new here.

### Docs
- This file (the log). The matrix gains a **CPU column** (today: emit + grid→threads
  measured; wave width 1; barrier deferred). `cajeta-xpu.md` reckoning extends to four
  backends.

## 3. Findings & limitations

- **Inc 1 — landed 2026-05-30.** `CpuTarget` over the shared lowerer; the portable
  SAXPY kernel (`Buffer<float32>` + `Thread.globalIdX()`) lowers to a host function and
  **runs correctly on the CPU** over a grid.
  - **The seam held with a single new fork: the coordinate *source*.** The CPU reuses
    `materializeParam`, `bufferElementPtr`, and `globalId`'s default verbatim — the only
    overrides are `createKernel` (append 9 i32 coordinate params, addrspace-0 buffers)
    and the three coordinate reads (pull from those args). The body walk is untouched —
    the same ~90% core now spans four backends.
  - **The existing "CPU emulation" is confirmed orthogonal.** It compiles a `@Kernel` as
    an ordinary host method with an explicit index param; it cannot run a `Thread.x()`-
    style portable kernel. `CpuTarget` is the actual portable path.
  - Wave ops are width-1 (honest single-lane), barriers raise `XPU-N01` — both as
    designed; Inc 5/6 lift them.
  - Tests: `XpuCpuEmitTests` (lowering shape — addrspace-0, coordinate-param model, no
    device intrinsics) + `XpuCpuExecTests` (JIT-execute SAXPY over a grid via LLJIT,
    verify `y[i] = a*x[i] + y0[i]` — the CPU oracle).
  - **Build gotcha (recorded):** the out-of-source build means `cmake .` in the source
    tree is a no-op for the real `build/` dir — new files need `cmake -S . -B build` to
    re-glob (`GLOB_RECURSE` without `CONFIGURE_DEPENDS`).
- **Inc 2 — landed 2026-05-30.** `Backend::Cpu` threads through the whole AOT path
  (enum → dispatch → Compiler → CLI), the backend selection became a multi-target list,
  and CPU registration links the kernel into the host module + registers its pointer.
  - **The registration shape is the measured CPU fork.** GPU backends embed a
    context-independent *blob* + `__cajeta_xpu_register_module(name, bytes, len)`. The CPU
    can't: its kernel is host IR in the program's own `LLVMContext`, so it must be *linked*
    (same-context fresh module → `Linker::linkModules`) and registered by *function
    pointer* via `__cajeta_xpu_register_cpu_kernel(name, fnptr)`. Cross-context linking is
    illegal, which is why the fresh module shares `hostModule.getContext()`.
  - **Multi-target works end-to-end:** `--xpu-backend=vulkan,cpu` puts a SPIR-V blob
    registration *and* a CPU host-kernel registration in one `.ll` — the bundling the
    explicit-only fall-to-CPU contract needs.
  - The `emitXpuKernels` artifact/registration loop now iterates selected backends;
    single-backend behavior (and `--xpu-arch`) is preserved, multi-backend uses
    per-backend default arches.
- **Inc 3 — landed 2026-05-31.** `CpuDriver` launches a registered kernel **by name**
  over a grid, serial. A portable SAXPY (`Buffer<float32>` + `Thread.globalIdX()`) lowers,
  registers, and runs correctly on the CPU end-to-end through the runtime registry.
  - **The launcher-thunk is the measured CPU launch fork.** GPU drivers hand a flat
    `void** kernelParams` to `cuLaunchKernel`/`hipModuleLaunchKernel`, which the hardware
    ABI unpacks. The CPU has no such ABI — calling host code needs a concrete C signature
    — so registration emits a per-kernel **uniform unpacker**
    `__cajeta_xpu_cpu_launch.<name>(void** argv, const i32* coord)` and registers *it*.
    The driver then drives one signature for every kernel. The unpacker reconstructs from
    the kernel's `FunctionType` alone — no `KernelParam` list threaded through — so the
    seam stayed put: the coordinate *source* (Inc 1) and the *registration shape* (Inc 2)
    are still the only CPU forks; the launch fork lives entirely in the registration pass
    + the trivial driver, not on `LoweringTarget`.
  - **`argv` mirrors the CUDA/HIP `kernelParams` convention exactly** (`argv[i] → &arg_i`),
    so host-side launch ergonomics match the GPU device tests minus the device.
  - Serial; threading + SIMD are Inc 5 (the honest split — an opaque function-pointer call
    isn't auto-vectorizable, so "vectorized payoff" was never reachable in Inc 3 anyway).
  - Tests: `XpuCpuDriverTests` (by-name launch + correct SAXPY over a grid; a second
    param shape — `+uint32 n` with an `if (i<n)` guard — proving the FunctionType-driven
    unpacker is general; a lookup-miss returning false) + an extended `XpuCpuAotCliTests`
    asserting the thunk is emitted and registered. The driver test exercises the **real**
    registration ctor: it JITs the registration module, maps `__cajeta_xpu_register_cpu_kernel`
    as an absolute symbol, runs `llvm.global_ctors`, and lets the ctor register the thunk
    into the same registry the driver reads back.
- **Inc 4.0 + 4.1 — landed 2026-05-31.** A compiled @Kernel program runs on the CPU with
  **no GPU**, through the real host-source launch path — the degrade-to-CPU headline.
  - **The decisive architectural finding: the C runtime is the SOLE launch path for
    compiled programs.** `runtime/native/cajeta_runtime.c` is parsed from embedded bitcode
    and linked into every program (`CajetaModule::linkRuntime`); it self-contains CUDA (its
    own `dlopen` + `cuLaunchKernel`). The C++ `CudaDriver`/`HipDriver`/`VulkanDriver`/
    `CpuDriver` classes are **compiler/test-only and never linked into a user program** (the
    runtime's own comment says as much for CUDA). So the dispatcher — and every backend's
    launch + device memory it can use — must live **in C, in the runtime**, not in the C++
    drivers. That is why Inc 4 is staged per-backend and why the C++ `CpuDriver` (Inc 3) is
    the *test-side twin* of the runtime's in-C CPU launch, exactly as `CudaDriver` mirrors
    the runtime's in-C CUDA — a deliberately duplicated pattern, not redundancy.
  - **One cached selector, then per-backend branches.** `cajeta_xpu_active_backend()` picks
    the highest-priority backend that is both bundled and available (env-forceable), caches
    it, and every device entry point switches on it. The int64 `Buffer<T>` handle is
    backend-specific (CUDA/HIP device ptr, Vulkan buffer index, CPU host block) but coherent
    within a run because the backend is fixed at first device touch (locked decision #2 — a
    GPU lost mid-run is a hard error, not a silent CPU re-run).
  - **The manifest closes the registry-ambiguity gap.** The runtime can't tell a cubin from
    a SPIR-V blob (both register via `__cajeta_xpu_register_module`), so the compiler emits a
    `__cajeta_xpu_register_backend((int) Backend)` ctor per bundled backend; the
    `xpu::Backend` enum values were already priority-aligned with the runtime ids
    (CUDA 0 / HIP 1 / VULKAN 2 / CPU 3), so the id is just the cast. *Limitation, documented:*
    at most one non-CPU device backend per bundle (mixed `nvptx,amdgpu` would collide
    one-blob-per-name) — a per-blob backend tag is the follow-up.
  - Tests: `XpuCpuDispatchTests` (GPU-free) — host-source SAXPY `--xpu-backend=cpu` runs on
    the CPU (sum 4096); forcing an unbundled backend (`CAJETA_XPU_BACKEND=cuda` with only
    `cpu` bundled) degrades gracefully (precise diagnostic, no crash, un-launched 2048);
    `CAJETA_XPU_BACKEND=cpu` force runs. `JitTestHelper` gained an `xpuBackends` option +
    the manifest emit; the CUDA host-launch path is behavior-preserved (full Xpu suite green).
  - **Remaining:** 4.2 (HIP) + 4.3 (Vulkan) wire those backends' launch + memory into the C
    runtime so the GPU rungs of the chain are live, not just CPU + CUDA.
- **Inc 4.2 — landed 2026-05-31.** The HIP rung in the C runtime (mirrors the CUDA block —
  `dlopen` libamdhip64 with the canonical-ROCm path resolution, `hipMalloc`/`hipMemcpyHtoD`/
  `hipModuleLoadData`/`hipModuleLaunchKernel`/`hipDeviceSynchronize`), sharing the module
  table (one device backend active per run). **Device-validated on the AMD box** via the
  dispatcher: `--xpu-backend=amdgpu` host-source SAXPY runs on the GPU; `amdgpu,cpu` +
  `CAJETA_XPU_BACKEND=cpu` falls to CPU with the GPU present. Fixed a latent deadlock the
  4.0 refactor introduced — `active_backend()` holds `g_xpu_cuda_lock` while selecting, so
  the availability probes must call the `*_init_locked` variants, not the locking
  `*_ready` wrappers (it never fired before because no CUDA/HIP bundle had run the selector
  under the lock; the HIP device test is the first to exercise it).
- **Inc 4.3 — landed 2026-05-31.** The Vulkan rung in the C runtime; **all four backends
  now route through the one dispatcher**, device-validated on AMD (HIP + Vulkan) and the CPU
  (GPU-free), CUDA behavior-preserved.
  - **The uniform launch ABI doesn't reach Vulkan for free** — the scope finding that shaped
    the work: `__cajeta_xpu_launch` gets a CUDA/HIP `kernelParams` argv (buffer handles + raw
    scalar values), but Vulkan's compute entry is `void main()` with descriptor-bound storage
    buffers and **no params** — scalars included must become single-element SSBOs. So beyond
    porting `VulkanDriver.cpp` to C, the rung needed **(1) per-kernel parameter metadata**
    (`__cajeta_xpu_register_kernel_params(name, count, isBuffer[], byteSize[])`, emitted by
    the Vulkan registration ctor from the shared `collectKernelParamInfo`) and **(2) an
    argv→descriptor-set translation** in the launch: buffer args bind their storage buffers,
    scalar args are copied into transient SSBOs (allocated, bound, freed per dispatch).
  - **The Vulkan buffer handle is a table index, not a pointer** — `Buffer<T>.deviceHandle`
    on Vulkan is a 1-based slot in the runtime's `VkBuffer`/`VkDeviceMemory`/mapped table;
    upload/download memcpy through the host-coherent mapping. The per-run-fixed backend keeps
    the handle's meaning consistent (decision #2).
  - **Baked local size** (`kVulkanLocalSizeX = 64`) is the one launch asymmetry — Vulkan
    fixes the workgroup size at SPIR-V compile time, so the launch dispatches `gridX`
    work-groups and the host-source block must match the bake (the device test uses
    `block:[64]`, `grid:[16]` for n=1024). CUDA/HIP take block dim per-launch; Vulkan can't.
  - The port is `dlopen`-resolved + `__has_include`-guarded, so a box without a Vulkan SDK
    header at runtime-build time compiles it out (Vulkan probes unavailable) — same graceful
    shape as the absent-GPU paths.
  - Tests: `XpuVulkanDispatchDeviceTests` (RADV) — `--xpu-backend=vulkan` SAXPY runs on the
    GPU through the dispatcher; `vulkan,cpu` + `CAJETA_XPU_BACKEND=cpu` falls to CPU. Full
    Xpu suite: 108 passed, 0 failed.
- **Two compiler bugs fixed via the AOT tour — 2026-05-31.** Building the first AOT
  (`--emit=obj`) `@Kernel` program (`samples/Tour/xpu/`, run by `run-xpu.sh`) surfaced two
  pre-existing compiler defects that the JIT-only test path had masked:
  - **`UseInitArray` — AOT global constructors never ran.** `Compiler::rebuildTargetMachine`
    left `TargetOptions::UseInitArray` at its `false` default, so the AsmPrinter emitted the
    legacy `.ctors` section instead of `.init_array`. Modern glibc startup runs `.init_array`,
    not `.ctors`, so **every** `--emit=obj/exe` global constructor silently never fired:
    per-class clinit (non-foldable static initializers), the `UnrecoverableException` vtable
    marker, the embedded runtime's `__attribute__((constructor))` init — and the XPU
    kernel/backend registration. Masked because the test suite runs through the JIT (LLJIT's
    `initialize()` runs `llvm.global_ctors` directly) and AOT programs so far used only lazy
    runtime init. Fix: `opt.UseInitArray = true` (one line) — what clang/llc set. The CPU
    tour kernel then runs as a native binary; this also un-breaks AOT static init generally.
  - **Vulkan multi-kernel SPIR-V codegen crash.** `vulkan::emitKernelRegistration` created one
    SPIR-V `TargetMachine` and reused it across all kernels' `emitSpirv`. LLVM 22's SPIR-V
    backend carries codegen state (`SPIRVGlobalRegistry`) on the TargetMachine, so the second
    kernel crashed in `SPIRVEmitIntrinsics::buildAssignPtr` (a segfault, sometimes an abort —
    classic state corruption). Never hit before because every prior Vulkan test compiled a
    single kernel; the two-kernel tour is the first multi-kernel Vulkan module. Fix: a fresh
    `TargetMachine` per kernel.

## 4. Runnable demo

`samples/Tour/xpu/` is the XPU tour — a portable `@Kernel` SAXPY + vecAdd program with a
`run-xpu.sh` that compiles for any backend and runs it (default `--xpu-backend=cpu`, so it
runs anywhere with no GPU). `./run-xpu.sh amdgpu,cpu` / `vulkan,cpu` target a device with
CPU fallback; `CAJETA_XPU_BACKEND=cpu ./run-xpu.sh amdgpu,cpu` forces the fall-to-CPU path
on a box that has the GPU. It sits beside the stdlib/language tour in `samples/Tour/`.

## 5. `Buffer<T>` is RAII (2026-05-31)

The XPU tour exposed that `Buffer<T>` used a manual `allocate()`/`free()` pattern with **no
destructor** — the drop chain freed the 16-byte handle struct but the device memory leaked
unless you remembered `free()`, working *against* the language. It also diverged from the
stdlib's own resource-handle convention (`FileReader`/`File`/`Path`: acquire in the ctor,
release in `~Dtor()`, idempotent `close()` as an escape hatch). Reworked to match:

- **`heap Buffer<T>(n)`** allocates device memory in the constructor; **`~Buffer()`** frees
  it via the drop chain at scope exit. `free()`/`allocate()` are now idempotent + null-
  guarded escape hatches. Move-out (`#buf`) transfers ownership (the source's drop entry is
  marked inactive, so its destructor no-ops). The tour uses the RAII form — no
  `allocate()`/`free()` — and runs identically on CPU, AMD (HIP), and Vulkan (RADV).
- Resize isn't a Buffer concern: GPU buffers are fixed-size; a different size is just a new
  `heap Buffer<T>(m)` (the old one drops). So in-place re-allocate is a rarely-needed escape
  hatch, not the model.

**Launch-borrow gate extended (XPU-K02).** A `kernel.launch(...)` borrows each Buffer until
`Stream.sync()`. The checker already errored on an explicit `free()` before sync; with a
destructor, an *implicit drop* before sync is the same use-after-free. `Method::destroyScope`
now also gates it: an owned, launch-borrowed Buffer that leaves scope before a sync is
XPU-K02. Only owned locals with a live drop entry trip it — borrowed params and `#`-moved
buffers are skipped, and a sync clears the borrow, so there are no false positives (verified:
`XpuLaunchBorrowTests.dropBeforeSyncRejected` / `dropAfterSyncAccepted`).

**Two compiler bugs fixed in passing:**
- **Generic-class destructor names.** `~Buffer()` was rejected because the validator compared
  the declared name against the *monomorphized* type name (`Buffer<float32>`) rather than the
  base name. `Buffer<T>` is the first generic stdlib class with a user destructor, so nothing
  had exercised it. Fix: strip the `<…>` type arguments before comparing
  (`CajetaLlvmVisitor.h`).
- **Stale-stdlib gotcha (recorded).** The stdlib `.cajeta` sources are *embedded into the
  compiler binary at build time* (`cajeta::stdlib::g_files`), so editing
  `runtime/src/cajeta/**` requires rebuilding the compiler (`cmake --build build`) before the
  change takes effect — testing against a stale binary mis-resolves heap overloads (a 1-arg
  ctor fell back to the 2-arg one, passing `n` as the device handle → a wild-pointer crash).

## 6. Backlog / known issues

Work items not part of the active increment, in priority order. **Resolution policy:** when
one is fixed, if the fix is a clean, self-contained change, **cherry-pick it onto `main`**;
otherwise commit it to `cajeta-xpu` and let it ride to `main` with the next branch merge.

_No open items._

### ✅ RESOLVED 2026-05-31 — CPU kernel registry held caller-owned name keys (order-dependent SIGSEGV)

`XpuCpuDriverTests.lookupMissOnUnknownKernel` passed in isolation but **segfaulted** when run
after the suite's two registration tests (and so in any broad `Xpu*`/full-suite run). Root
cause: the CPU kernel registry (`__cajeta_xpu_register_cpu_kernel`, `cajeta_runtime.c`) stored
the kernel name as the **raw `const char*` the caller passed** — for a JIT'd registration ctor
that key is the `xpu.cpu.kname.<name>` global living in JIT memory, which is freed when the test
tears down its `LLJIT`. A later lookup's `strcmp()` then dereferenced the dangling key. (In a
real compiled program the kname globals live for the process, so this only bit the JIT test
harness — but the registry was wrong to assume caller lifetime.) **Fix:** `strdup` the name on
registration so the registry owns its keys (matching the env-registry in the same file).
Regression test `XpuCpuDriverTests.registryKeySurvivesCallerTeardown` registers a uniquely-named
kernel, tears down its JIT, then looks up — clean miss, no crash (deterministically SIGSEGVs
against the pre-fix runtime). Full `Xpu*` suite now green with nothing excluded.
