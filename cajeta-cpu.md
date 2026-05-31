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
| `workgroupBarrier` | hardware barrier | **deferred → `XPU-N01`** (a CPU barrier needs work-item loop fission / fibers; barrier-free kernels first, exactly as AMD/Vulkan deferred features) |
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

### Increment 3 — threaded host driver + on-CPU SAXPY (Tier-1, the "vectorized" payoff)
- `src/cajeta/xpu/cpu/CpuDriver.{h,cpp}` — `available()` ≡ true; a `parallel_for` over
  the grid that calls the kernel per work-item, the work-item loop **loop-vectorized**
  by LLVM (real SIMD for data-parallel kernels). On-CPU SAXPY + shared-free reductions.

### Increment 4 — the runtime dispatcher + explicit multi-target bundling
- Priority probe (`CUDA→HIP→Vulkan→CPU`), startup-availability, cached; the precise
  "no available backend among {…}" diagnostic; `--xpu-backend=vulkan,cpu` end-to-end.

### Increment 5 — true wave = SIMD lane (SPMD vectorization, ISPC-style)
- For wave-cooperative kernels: lower the body over `<W x T>` vectors with mask
  propagation through divergent control flow, `Wave.*` → SIMD permute/horizontal ops,
  `Wave.width()` → W. The harder vectorization; lifts wave width from 1 to native.
- Probe AMX / SME matmul lowering here (CPU matrix engines, runtime-feature-gated).

### Increment 6 — workgroup barriers via work-item loop fission (POCL-style)
- Lifts the barrier restriction: split the kernel at each barrier, loop over work-items
  per region. Unblocks shared-memory reductions on CPU.

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
