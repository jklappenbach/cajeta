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
- **Multi-core threading** (moved here from Inc 3, 2026-05-31): a `parallel_for` chunking
  the grid across cores (`std::thread`, not the runtime's cooperative fiber carrier),
  serial fallback. The wall-clock payoff for data-parallel kernels.
- **True wave = SIMD lane** (SPMD vectorization, ISPC-style): for wave-cooperative
  kernels, lower the body over `<W x T>` vectors with mask propagation through divergent
  control flow, `Wave.*` → SIMD permute/horizontal ops, `Wave.width()` → W. Lifts wave
  width from 1 to native.
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
