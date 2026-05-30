# CajetaXPU — Implementation Handoff

Cross-machine handoff for the `cajeta-xpu` feature branch. Pick up here
after cloning to another box (e.g. Windows). The design spec is
[`cajeta-docs/CajetaXPU.md`](cajeta-docs/CajetaXPU.md); the variance
discipline is [`cajeta-docs/CajetaXPU-Variance.md`](cajeta-docs/CajetaXPU-Variance.md).
This file tracks *implementation status* — what's built, what's stubbed,
and what's next.

> **AMD second backend: DONE (gfx1151, on-device).** SAXPY, a strided loop,
> and static + dynamic shared-memory (LDS) reductions all compile through
> `--xpu-backend=amdgpu` and run on a real Strix Halo APU (gfx1151) from the
> SAME Cajeta source that runs on NVIDIA. The seam was extracted empirically
> by threading AMDGPU through the originally NVIDIA-only path — see
> **[The NVIDIA∩AMD overlap reckoning](#the-nvidiaamd-overlap-reckoning)**
> below for the measured variance surface (the deliverable the whole strategy
> exists to produce). `cajeta-amd.md` was the pickup plan for this work.

> **Vulkan third backend: DONE (SPIR-V, on-device via RADV).** SAXPY and a
> static workgroup-shared tree reduction compile through `--xpu-backend=vulkan`
> (LLVM 22 → Khronos SPIR-V, no external assembler) and run on the Strix Halo
> APU via the radeon Vulkan ICD, from the SAME Cajeta source. Vulkan is the
> *bigger* fork the strategy was built to measure: with no raw-pointer kernel
> ABI and no Buffer Device Address path in LLVM 22's SPIR-V backend, buffers
> become descriptor-set storage buffers — so the kernel **signature** and
> **buffer access** fork, not just the coordinate leaf reads. Full log:
> [`cajeta-vulkan.md`](cajeta-vulkan.md); the four-column capability matrix
> (NVIDIA · AMD · Vulkan · core) is [`cajeta-xpu-matrix.md`](cajeta-xpu-matrix.md).
> The build also surfaced that LLVM 22 emits a Vulkan-invalid workgroup barrier
> (SequentiallyConsistent semantics) — fixed by a one-instruction SPIR-V
> post-emit pass, so the emitted modules pass strict `spirv-val`; the one
> remaining caveat is the compile-time-fixed block dim.

> **Status: SAXPY runs on a real NVIDIA GPU (design phase 2 milestone).**
> The NVPTX vertical slice is proven end-to-end on an RTX 4090 (sm_89,
> CUDA 12.9, LLVM 22.1.4): the spec SAXPY `@Kernel` source compiles
> `cajeta → device LLVM IR → PTX → .cubin → cuLaunchKernel` and the
> on-device result matches the CPU reference over 2^20 elements
> (`test/xpu/XpuSaxpyDeviceTests.cpp`). Steps 9–11 of the original plan
> are demonstrated; what remains is generalization + the carried-over
> frontend deferrals (see "Vertical-slice status" below).
>
> Earlier baseline (still true on the CPU-emulation path): step 7 of 11,
> the design phase-1+2 CPU-emulation milestone ([`CajetaXPU.md`
> §12](cajeta-docs/CajetaXPU.md)).

---

## Vertical-slice status (NVPTX, steps 8–11)

The NVIDIA backend was built as a **vertical slice** to de-risk the
external toolchain early rather than finishing all of step 8's frontend
deferrals first. Six commits on `cajeta-xpu` (after the restore commits):

| Increment | What landed | Tests |
|-----------|-------------|-------|
| A | launch grammar (`kernel.launch(cfg)(args)`, `[...]` literals) + `CallExpression` / `ArrayLiteralExpression` AST | `XpuLaunchGrammarTests` |
| B | launch-site recognition + MIR `bodyOps`/launch-site walker (`XpuMirBuilder`) | `XpuMirLaunchSiteTests` |
| C | `NvptxBackend` (TargetMachine + PTX emit) and `NvptxKernelLowering` (kernel AST → device IR) | `XpuNvptxEmitTests` |
| D | `ptxas` → cubin assembly (`assembleCubin`) | `XpuNvptxEmitTests` |
| E+F | `CudaDriver` (dlopen nvcuda) + SAXPY launched & verified on-device | `XpuSaxpyDeviceTests` |
| G | host-source launch (`kernel.launch(...)` → `__cajeta_xpu_launch`) + cubin registration ctors + launch-borrow-scope checking | `XpuLaunchCodegenTests`, `XpuLaunchBorrowTests`, `XpuHostLaunchDeviceTests` |
| H | general single-kernel compute bodies: mutable locals (entry allocas + mem2reg), `for`/`while`/`do-while`, unlabeled `break`/`continue`, compound assignment, full int/float operator set, unary/prefix/postfix `++`/`--`, numeric casts, `Barrier.workgroup()` | `XpuNvptxLoopEmitTests`, `XpuLoopDeviceTests` |
| I | workgroup **shared memory** via a `shared` placement keyword (sibling of `heap`/`stack`): `Shared<T> tile = shared T[N];` → one per-block `addrspace(3)` global of constant size N, indexed/assigned like a buffer. Device-only (`XPU-K03` on the host path). | `XpuNvptxSharedEmitTests`, `XpuSharedDeviceTests` |
| J | **dynamic shared memory**: a runtime-sized `shared T[expr]` lowers to an external unsized `addrspace(3)` global (`.extern .shared`), sized at launch via a new `sharedBytes:` launch-config key threaded through `__cajeta_xpu_launch` → `cuLaunchKernel`'s `sharedMemBytes`. One dynamic region per kernel. | `XpuNvptxSharedEmitTests`, `XpuSharedDeviceTests`, `XpuLaunchCodegenTests` |

**Deviations from [`CajetaXPU.md`](cajeta-docs/CajetaXPU.md) (intentional, slice-scoped):**

1. **Kernel bodies lower via a dedicated `NvptxKernelLowering`** that walks
   the kernel AST and emits device IR directly — NOT by reusing the host
   `CajetaLlvmVisitor` (entangled with scope/drop/bounds runtime emits),
   and NOT "from MIR" as §5.2 diagrams. MIR stays thin metadata. The
   lowerer builds device LLVM types fresh from `CajetaType` flags in the
   device `LLVMContext` (never `getLlvmType()`, whose cache is host-context-
   bound). Supported subset = SAXPY-class kernels; unsupported constructs
   raise `XPU-N01`. Generalizing the subset is follow-on work.
2. **Single `sm_89` cubin, no fatbin** (multi-arch §5.2/§10.2 deferred).
3. **dlopen `nvcuda.dll`** rather than link `cuda.lib` — matches §1.1/§9
   and keeps the build CUDA-free; absent driver ⇒ `CudaDriver::available()`
   is false and device tests skip.
4. Spec `@kernel`/`global_id_x()` vs code `@Kernel`/`Thread.globalIdX()` —
   unchanged; carried-over deferral #6.

**Still open (generalization + carried-over deferrals):**

- **Host launch via Cajeta source. DONE** — `XpuHostLaunchDeviceTests.
  saxpyHostSourceOnDevice` runs SAXPY on the real GPU with BOTH the `@Kernel`
  AND the host driver written in Cajeta, compiled through the LLJIT
  (allocate / upload / launch / sync / download / free, sum == 4*n).
  - `CallExpression::generateCode` lowers `kernel.launch(stream, grid:,
    block:)(args)` to `__cajeta_xpu_launch(name, gridX, blockX, argv)`,
    marshalling Buffer args to their device handle and scalars by value
    (`XpuLaunchCodegenTests`).
  - The `__cajeta_xpu_*` runtime symbols are now **real, CUDA-backed** in the
    C runtime bitcode (`runtime/native/cajeta_runtime.c`): a lazily-dlopen'd
    `nvcuda` driver (mirroring `CudaDriver`) backs
    `_buffer_alloc`/`_upload`/`_download`/`_free` (cuMemAlloc / cuMemcpy* /
    cuMemFree), `_stream_sync` (cuCtxSynchronize), `_register_module`
    (cuModuleLoadData, keyed by entry name) and `_launch`
    (cuModuleGetFunction + cuLaunchKernel). Absent GPU ⇒ graceful no-op.
  - `Buffer<T>` device methods are real Cajeta now (`alloc` factory +
    `allocate`/`upload`/`download`/`free`), with element size supplied by a
    compiler-recognized `Buffer<T>.elementBytes()` intrinsic (DataLayout size
    of `T`; `MethodCallExpression`). A `@Kernel` taking `Buffer<T>` args gets
    an empty **host stub** body (`Method::generateCode`) since buffer indexing
    is device-only.
  - `NvptxRegistration::emitKernelRegistration` builds each `@Kernel`'s cubin
    and emits an `llvm.global_ctors` entry calling `__cajeta_xpu_register_module`
    with the embedded cubin bytes; wired into `JitTestHelper` (ctors run at
    `jit->initialize()`).
  - **Dispatch fix** (`CajetaClass::invokeMethod`): `@Native` instance methods
    are now **direct-dispatched**, not virtual. They are leaf forwarders to a
    fixed C symbol — inherently non-polymorphic — and may be invoked on a
    handle/sentinel receiver: `Stream.current()` returns a NULL handle (the
    CUDA default stream), and `s.sync()` (a `@Native` void forwarder that
    ignores `self`) must not load a vtable from the null receiver. The virtual
    path segfaulted there; the direct call passes the null through to the
    forwarder, which discards it. (This was the sole blocker for the on-device
    host-source run.)
  - Known generic gaps, not on the host-launch path: a static call qualified by
    the generic class name (`Buffer.deviceAlloc(...)`) resolves to the
    uninstantiated template and lowers to null, and explicit
    `Buffer<float32>.alloc(n)` crashes the parser (bad any_cast). The working
    path uses instance methods (`this.deviceAlloc(...)`) and `heap Buffer<T>(...)`.
- **Launch-borrow-scope checking** (deferral #4): **done (core case)** —
  `launch` borrows each Buffer arg, released at `Stream.sync()` /
  `Event.waitHost()`; freeing a still-borrowed buffer is `XPU-K02`
  (`XpuLaunchBorrowTests`). Deferred: per-stream tracking, the move/reassign
  and auto-drop-at-scope-exit-without-sync cases, and cross-stream WAR/RAW
  (§11 cases 2–3).
- **`--xpu-backend` / `--xpu-arch` / `--xpu-emit` CLI flags. DONE** — the AOT
  `cajeta` binary now drives the NVPTX path (no longer JIT-test-only).
  `--xpu-backend=nvptx` embeds each `@Kernel`'s cubin + registration ctor into
  its host module (same `emitKernelRegistration` the JIT helper runs, hooked into
  `Compiler::compile` after Phase-1/2 quiescence); `--xpu-arch=<sm_xx>` selects the
  SM target; `--xpu-emit=ptx|cubin` also drops a per-kernel artifact for
  inspection. Default `--xpu-backend=none` leaves the host-only path unchanged.
  `XpuAotCliTests` (GPU-free for the PTX case; cubin gated on ptxas).
- **Broaden `NvptxKernelLowering`'s construct coverage. DONE (increment H)** —
  the device lowerer now handles general single-kernel compute bodies (loops,
  unlabeled break/continue, compound assignment, the full int/float operator set,
  unary/prefix/postfix `++`/`--`, numeric casts, `Barrier.workgroup()`), with
  entry-block-alloca mutable slots promoted by mem2reg before PTX emit. Verified
  on-device (`XpuLoopDeviceTests`) and in PTX text (`XpuNvptxLoopEmitTests`).
  Still `XPU-N01` (next increment): Wave shuffles/ballots, calls to user
  `@Device` helpers, for-each loops, and labeled break/continue.
- **Workgroup shared memory. DONE (increment I)** — a third placement keyword
  `shared` (sibling of `heap`/`stack`): `Shared<T> tile = shared T[N];` lowers to
  one per-block `internal addrspace(3)` global of compile-time-constant size N;
  indexing/assignment reuse the buffer path (LLVM tracks the address space on the
  pointer). Device-only — the host codegen path rejects `shared` with `XPU-K03`.
  Verified on-device (256-wide tree reduction, `XpuSharedDeviceTests`) and in PTX
  text / via the AOT CLI (`.shared`/`ld.shared`/`st.shared`/`bar.sync`,
  `XpuNvptxSharedEmitTests`).
- **Dynamic shared memory. DONE (increment J)** — a runtime-sized `shared T[expr]`
  (non-constant size) lowers to an external unsized `addrspace(3)` global
  (`.extern .shared`), sized at launch. A new `sharedBytes:` launch-config key
  (named to dodge the `shared` keyword + match CUDA's `sharedMemBytes`) threads
  the byte count through `__cajeta_xpu_launch` (now 5-arg) → `cuLaunchKernel`'s
  `sharedMemBytes`; `CudaDriver::launch` gains a defaulted `sharedMemBytes`. One
  dynamic region per kernel. Verified on-device (dynamic tree reduction sized at
  launch, `XpuSharedDeviceTests`) + PTX `.extern .shared` + launch lowering
  (`XpuLaunchCodegenTests`). The shared-aliasing borrow rule (overlapping `&mut`
  slices, spec §11 case 2) remains a separate deferred item.

---

## The NVIDIA∩AMD overlap reckoning

The AMD second backend (cajeta-amd.md) was built to *measure* the
NVIDIA∩AMD intersection, not assume it: the seam was extracted by threading
AMDGPU through the originally NVIDIA-only lowerer, so the methods that ended
up on the abstraction ARE the variance surface. With two backends now real
and on-device, here is what actually forked vs. what stayed shared.

### What forked — the `LoweringTarget` vtable (the whole variance surface)

The ~885-line kernel-body AST walk is shared
(`src/cajeta/xpu/lowering/KernelLowering.cpp`); only these decisions differ,
and they are the *entire* `LoweringTarget` interface
(`src/cajeta/xpu/lowering/LoweringTarget.h`):

| `LoweringTarget` method | NVPTX | AMDGPU |
|-------------------------|-------|--------|
| `allocaAddressSpace()` | 0 (generic) | **5 (private)** — the one address-space fork; an AS-0 alloca is invalid IR on AMDGPU |
| `threadId(dim)` | `nvvm.read.ptx.sreg.tid.*` | `amdgcn.workitem.id.*` |
| `workgroupId(dim)` | `nvvm.read.ptx.sreg.ctaid.*` | `amdgcn.workgroup.id.*` |
| `workgroupDim(dim)` | `nvvm.read.ptx.sreg.ntid.*` | **dispatch-packet load** (`amdgcn.dispatch.ptr` + i16 @ offset 4/6/8) — block dim is NOT an intrinsic on AMD |
| `workgroupBarrier()` | `nvvm.barrier.cta.sync.aligned.all` | `amdgcn.s.barrier` wrapped in workgroup-scoped release/acquire fences (LDS visibility) |
| `decorateKernel()` | `ptx_kernel` CC **+ `nvvm.annotations`** metadata | `amdgpu_kernel` CC, **no metadata** |
| `globalId(dim)` | *shared default* — `workgroupId*workgroupDim + threadId` holds on both; not a fork |

Backend-module decisions outside the AST walk (the `*Backend` /
`*Registration` / driver files), also forked but mechanically:

| Decision | NVPTX | AMDGPU |
|----------|-------|--------|
| Device triple | `nvptx64-nvidia-cuda` | `amdgcn-amd-amdhsa` |
| Assemble | PTX → `ptxas` → cubin | object emitted directly by the TargetMachine → `ld.lld -shared` → hsaco (**no external assembler**) |
| Driver / loader | `nvcuda` (`cuModuleLoadData` / `cuLaunchKernel`) | HIP (`hipModuleLoadData` / `hipModuleLaunchKernel`) |
| Dynamic shared sizing | `cuLaunchKernel` `sharedMemBytes` | `hipModuleLaunchKernel` `sharedMemBytes` (= groupMemBytes) — *same launch-config shape* |

### What stayed shared (≈90% — the measured core)

- The **entire kernel-body AST walk**: statements, control flow
  (`if`/`for`/`while`/`do`, unlabeled `break`/`continue`), the full int/float
  operator set, short-circuit `&&`/`||`, unary/prefix/postfix, numeric casts,
  the mutable-slot scalar model + mem2reg-before-emit.
- **Buffer/array params → `addrspace(1)` pointers** — identical on both.
- The **`shared` keyword → `addrspace(3)` globals**: static (internal
  `[N x T]`) and dynamic (external unsized `[0 x T]`, launch-sized) — the IR
  is byte-for-byte the same; only the launch-time byte count plumbs through a
  different driver call.
- **Address-space numbers** (`AddressSpace.h`): `amdNumberFor ≡
  nvidiaNumberFor` confirmed — Global=1, Shared=3 agree. AS is *not* a fork
  point for buffers or shared memory (only for allocas).
- **Registration shape**: embed device bytes as a host constant + an
  `llvm.global_ctors` entry calling the **backend-neutral**
  `__cajeta_xpu_register_module(name, bytes, len)` — keyed by entry name.
  Only the binary format behind it (cubin→hsaco) changed.
- **All frontend work** — `@Kernel`/`@Device` recognition, `KernelArg`
  validation (`XPU-K01`), launch grammar, MIR scaffolding, launch-borrow
  checking — never touched; AMD inherited it for free.

### What this measured that was previously a guess

- **Address space is a near-non-fork.** The doc predicted Global/Shared agree;
  confirmed. The *only* AS divergence is the alloca/private space (0 vs 5) —
  exactly the "classic first AMDGPU bug" cajeta-amd.md §2 warned about.
- **Block dim is the single most divergent coordinate.** Every other
  coordinate read is a 1:1 intrinsic swap; only `workgroupDim` is structurally
  different (AMD has no `ntid` intrinsic — it reads the HSA dispatch packet).
- **The launch/registration surface is genuinely universal.** The
  name-keyed runtime symbol and the launch-config shape (incl. `sharedMemBytes`)
  did not fork at all — only the binary format and the driver entry points did.
- **AMD needs no external assembler.** The TargetMachine emits the object
  directly; `lld` links it — simpler than the NVPTX `ptxas` round-trip.

This table is the empirical NVIDIA∩AMD core, and the input to the later "how
much of this core extends to Vulkan/SPIR-V" question.

### Extending to Vulkan/SPIR-V — the third backend's answer

Vulkan was then threaded through the same seam ([`cajeta-vulkan.md`](cajeta-vulkan.md)),
which *answered* that question with measurements rather than projection:

- **The coordinate leaf reads generalize cleanly** — better than AMD, even.
  `workgroupDim` (AMD's ugliest fork, a dispatch-packet load) is a *native*
  single intrinsic on SPIR-V (`llvm.spv.workgroup.size`), and `globalId` is
  native too (`llvm.spv.thread.id`). `addrspace(3)` shared memory maps straight
  to the Workgroup storage class — a non-fork, exactly as predicted.
- **But the kernel-argument model forks hard, and that's new.** LLVM 22's SPIR-V
  backend has **no raw-pointer kernel ABI and no Buffer Device Address path**, so
  buffers must be descriptor-set storage buffers. Unlike AMD (which forked only
  `LoweringTarget`'s leaf reads), Vulkan forks the kernel **signature** (`void
  main()`, no params) *and* the body's **buffer element access** (`getpointer`
  vs GEP). The seam grew by three hooks — `createKernel`, `materializeParam`,
  `bufferElementPtr` — all with NV/AMD-preserving defaults.
- **Two measured limitations, recorded not hidden:** the workgroup barrier emits
  Vulkan-forbidden SequentiallyConsistent semantics (runs on RADV, fails strict
  `spirv-val`); and block dim is fixed at SPIR-V compile time (Vulkan has no free
  per-dispatch workgroup size).

So the NVIDIA∩AMD∩Vulkan core is *smaller* than NVIDIA∩AMD's ≈90% — the
kernel-argument ABI dropped out of the shared set — but the body walk, control
flow, operators, coordinate reads, shared memory, and the entire frontend +
name-keyed registration still hold across all three. The full per-feature
breakdown is [`cajeta-xpu-matrix.md`](cajeta-xpu-matrix.md).

### AMD on-device increments (cajeta-amd.md) — all landed

| Increment | What landed | Tests |
|-----------|-------------|-------|
| 0 | `XpuBackend::Amdgpu` + CLI (`--xpu-backend=amdgpu`, `gfx1151` default) + backend dispatch seam (`xpu::emitKernelRegistration`) | NVIDIA stays green |
| 1 | `AmdgpuBackend` (TargetMachine, `emitIsa`, `assembleHsaco` via lld) | `XpuAmdgpuEmitTests` |
| 2 | `LoweringTarget` seam + `AmdgpuKernelLowering` over the shared AST walk; NVPTX lowerer refactored onto the same shared lowerer (behavior-preserving) | `XpuAmdgpuLoopEmitTests` (+ all NVPTX emit tests still green) |
| 3 | `AmdgpuRegistration` (hsaco + global_ctors) + AOT `--xpu-emit=isa\|hsaco` | `XpuAmdgpuAotCliTests` |
| 4 | `HipDriver` (dlopen libamdhip64) + on-device SAXPY | `XpuSaxpyAmdDeviceTests` (gfx1151) |
| 5 | static + dynamic LDS reductions on-device | `XpuSharedAmdDeviceTests` (gfx1151) |

> **Box gotcha (environment, not Cajeta).** This Strix Halo box's
> `ROCM_PATH` / `LD_LIBRARY_PATH` point at `/opt/rocm-7.x` (7.2.53210), whose
> `libhsa-runtime64` **segfaults at code-object load** (deep in
> `ReleaseQueueMainScratch` during the executable freeze). The
> alternatives-canonical `/opt/rocm` (7.2.2) works. `HipDriver` defends
> against this by pinning the canonical `/opt/rocm/lib/libhsa-runtime64.so.1`
> with `RTLD_GLOBAL` before loading `libamdhip64`, so a poisoned
> `LD_LIBRARY_PATH` can't bind the broken HSA runtime — the on-device tests
> pass under the default (broken) env with no fiddling. Worth fixing the
> shell env regardless (`/opt/rocm-7.x` looks like a broken dev build).

**Still AMD-side refinement (not blocking the phase):** wave ops (the first
variance-shaped feature that justifies the seam — `nvvm.shfl.sync` vs
`amdgcn.ds.bpermute`/swizzle), the JIT host-source launch path for AMD (the
AMD analog of `XpuHostLaunchDeviceTests` — the device tests currently drive
`amd::` directly), multi-arch bundling, and `@Device` helper calls.

---

## Branch provenance

This branch restores the XPU work that was reverted on `main` by
`211e93e` ("revert: roll back XPU work to unblock v0.1.0 release"). The
reverted work — 5 commits, 38 files, +2936 lines — was cherry-picked
onto post-revert `main` and is **byte-identical** to the original
pre-revert commits (verified against `fb4d8d3`).

The 5 commits on this branch (oldest → newest):

| Commit    | Maps to steps | Summary |
|-----------|---------------|---------|
| `6f91da4` | (docs)        | CajetaXPU variance discipline + PR template |
| `4112e8e` | 1             | `@Kernel`/`@Device`/`@Host`/`@Wave` attribute recognition |
| `6325c7b` | 2             | `cajeta.xpu.core` stdlib declarations + runtime stubs |
| `c1ecd2c` | 3, 4, 5       | KernelArg validation + MIR scaffolding + address-space recognition |
| `76665b6` | 6, 7          | SAXPY end-to-end on CPU-emulation path |

Original (reverted) equivalents on the pre-revert line, for reference:
`7001e2c`, `8ab8dca`, `3e8c7ec`, `13453e3`, `fb4d8d3`.

---

## What was done — steps 1–7

### Step 1 — Attribute recognition  ✅  (`4112e8e`)
- `@Kernel` / `@Device` / `@Host` / `@Wave(width: N)` / `@Backend(...)`
  recognized via the existing `Annotatable` system — no visitor work.
- Grammar tweak (**step 1b**): `elementValuePair` now accepts `:` as an
  alias for `=`, so `@Wave(width: 32)` and `@Wave(width = 32)` both parse
  (`antlr4/CajetaParser.g4`).
- `src/cajeta/xpu/core/XpuAttributes.h` — short-name constant registry +
  `isKernel` / `isDevice` / `isHost` predicates.
- `src/cajeta/xpu/core/XpuKernelAttr.{h,cpp}` — typed view collecting
  `@Wave(width)` + `@Backend` (single or list) off a `@Kernel` method.
- Tests: `test/xpu/XpuAttributesTests.cpp` (9).
- **Naming note:** code uses PascalCase `@Kernel` (matches
  `@Native`/`@Component`); the spec spells it lowercase `@kernel`. To be
  reconciled in docs.

### Step 2 — `xpu.core` stdlib + runtime stubs  ✅  (`6325c7b`)
- 13 `.cajeta` declaration files under
  `runtime/src/cajeta/xpu/core/`: `Stream`, `Event`, `Fence`, `Thread`,
  `Workgroup`, `Barrier`, `Wave`, `Buffer`, `Capabilities`,
  `AddressSpace`, `KernelArg`, `KernelError`.
- `@Native` C runtime stubs in `runtime/native/cajeta_runtime.c`
  (search `cajeta.xpu.core runtime stubs`, ~line 3861). **Every stub
  returns zero / no-ops** — LLJIT materializes externs eagerly, so they
  must exist before any real implementation does. Calling them yields a
  null Stream / zero coordinate / no-op barrier — not a crash.
- Tests: `test/xpu/XpuCoreStdlibTests.cpp`.

### Step 3 — KernelArg validation  ✅  (`c1ecd2c`)
- `src/cajeta/xpu/core/KernelArgTrait.{h,cpp}` — `isKernelArgAdmissible`
  + `validateKernelParams`. Admits: primitives, primitive arrays
  (recursive on element type), `Buffer<T>` (canonical-name prefix), and
  any class implementing the `cajeta.xpu.core.KernelArg` marker
  interface. Non-admissible params throw `cajeta::Exception` with
  errorId **`XPU-K01`**.
- Wired into `Method::generateCode` (`src/cajeta/method/Method.cpp`,
  after the idempotent `llvmBasicBlock` guard, so it runs once/method).
- **Not yet admitted:** POD structs without explicit `implements
  KernelArg`, `Texture<...>` / `Sampler` (types not declared),
  `@PushConstant` (Vulkan-only, deferred).
- Tests: `test/xpu/XpuKernelArgTests.cpp` (5).

### Step 4 — XPU MIR scaffolding  ✅  (`c1ecd2c`)
- `src/cajeta/xpu/mir/`: `XpuMirOp.h` (9-op set), `XpuMirType.h`,
  `XpuMir.h` (Kernel/LaunchSite/Module containers), `XpuMirBuilder.{h,cpp}`,
  `XpuMirPrinter.{h,cpp}`.
- MIR is **thin by design**: it carries the structural envelope
  (canonical name, address-space-qualified param list, `@Wave`/`@Backend`
  attrs) around kernel bodies. Bodies still lower through the existing
  AST→LLVM visitor; MIR does not replace them.
- **Placeholders (intentionally empty in v1):** `XpuMirKernel::bodyOps`
  (leaf-builtin walker not run), `XpuMirLaunchSite` fields
  (grid/block/stream/args not captured).
- Tests: `test/xpu/XpuMirBuilderTests.cpp` (7) — incl. printer round-trip.

### Step 5 — Address-space recognition  ✅  (`c1ecd2c`)
- `src/cajeta/xpu/core/AddressSpace.h` — 5 qualifiers
  (`Generic/Global/Shared/Constant/Private`) + per-backend numbering
  tables (`nvidiaNumberFor` / AMD shares it; `spirvNumberFor` is a stable
  test integer, real SPIR-V storage classes mapped later). The literal
  artifact behind variance-doc rows 4–5.
- `isAddressSpaceCanonical` recognizes
  `cajeta.xpu.core.{Global,Shared,...}[<...>]` names; `XpuMirBuilder`
  uses it to tag param types.
- **Deferred:** the leaf-builtin body-op walker (populating `bodyOps`
  with `Op_ThreadId` etc.) and the real LLVM-IR address-space lowering —
  those land with the NVPTX backend (step 9).
- Tests: `test/xpu/XpuAddressSpaceTests.cpp` (2).

### Step 6 — Launch surface (minimal form)  ✅  (`76665b6`)
- KernelArg admissibility extended to **primitive arrays** (`float32[]`,
  `int32[]`, …) so a SAXPY-shaped kernel signature compiles.
- **Deferred:** the real `saxpy.launch(stream, grid:, block:)(args)`
  postfix syntax needs an expression-grammar extension (postfix `(args)`
  after an expression result) — *not yet implemented*. See "Carried-over
  deferrals" below.

### Step 7 — CPU-emulation SAXPY end-to-end  ✅  (`76665b6`)  ← current
- A `@Kernel` compiles from Cajeta source via JIT; host driver (also
  Cajeta source) iterates the index space and calls the kernel per
  index; output matches the CPU reference (`y[i] = a*x[i] + y[i]`,
  sum = 32.0 over 8 elements).
- Because the real `launch()` syntax is deferred, the "launch"
  abstraction degenerates to a host-side loop. The kernel takes the
  thread index `i` as an **explicit parameter** rather than calling
  `xpu.thread.global_id_x()` — so the coordinate-reader stubs (still
  returning 0) are sidestepped, not exercised.
- Tests: `test/xpu/XpuLaunchAndSaxpyTests.cpp`.

---

## Carried-over deferrals (must be picked up regardless of step numbering)

These were shipped as stubs/placeholders to reach the milestone. They
are prerequisites for real backend codegen and shouldn't get lost in the
step-renumbering:

1. **Real `launch(stream, grid:, block:)(args)` grammar** — postfix
   `(args)` call after an expression. Blocks real launch-site
   recognition and stream/event scheduling.
2. **Launch-site resolution** — populate `XpuMirLaunchSite`
   (grid/block/stream/arg references). Currently a placeholder.
3. **MIR body-op walker** — populate `XpuMirKernel::bodyOps` with leaf
   builtins (`Op_ThreadId`, `Op_BarrierWorkgroup`, …). Currently empty.
4. **Deferred-borrow / launch-borrow-scope checking** — the borrow
   checker should treat `launch` as a borrow held until the next
   `Stream.sync()` / `Event.wait()` (spec §3.5, §11). Not implemented.
5. **Real runtime impls** behind the zero-stubs in `cajeta_runtime.c`:
   thread/workgroup coordinate readers (TLS set by the dispatch loop),
   `Stream`/`Event`/`Fence`, and a `Buffer<T>` host allocator. The
   CPU-emulation path needs these to run kernels that actually read
   `xpu.thread.*` or use `Buffer<T>`.
6. **Spec/code spelling reconciliation** — `@Kernel` (code) vs `@kernel`
   (spec).

---

## What's left — steps 8–11 (NVIDIA / NVPTX native backend)

> **Confidence note.** Steps **9** and **11** are anchored by explicit
> forward-references in the code (see citations). Steps **8** and **10**
> are *reconstructed* — the original 11-step plan's exact wording lived
> in the prior conversation, not in the repo, so the 8/10 boundaries
> below are my best reconstruction from [`CajetaXPU.md` §12](cajeta-docs/CajetaXPU.md)
> (design phase 2) and the code's hints. **Adjust the 8/10 split if it
> doesn't match your intended plan.**

### Step 8 — Frontend/MIR completion (reconstructed)
Close the carried-over deferrals that real codegen depends on: the
`launch()` postfix grammar (#1), launch-site resolution (#2), the MIR
body-op leaf walker (#3), and launch-borrow-scope checking (#4). After
this, there is a *complete* MIR (signature envelope **plus** body ops
**plus** launch sites) for a backend pass to consume.

### Step 9 — NVPTX lowering pass (anchored)
> Code: `XpuAddressSpaceTests.cpp` ("real lowering on LLVM IR lands with
> the NVPTX backend (step 9)"); `XpuCoreStdlibTests.cpp`,
> `KernelArgTrait.h`, `XpuMirPrinter.h` ("step 9+").

Lower MIR → LLVM IR with: NV address spaces (the `nvidiaNumberFor`
table), `ptx_kernel` calling convention, and target intrinsics for
thread/workgroup IDs, barriers, and wave ops (the `xpu.thread.x` →
`llvm.nvvm.read.ptx.sreg.tid.x` family in `CajetaXPU.md` §5.4).
Capability traits become Target-resolved-at-codegen here.

### Step 10 — NVPTX assembly + CUDA runtime (reconstructed)
`llc` → `.ptx` → `ptxas` → `.cubin`; fatbin embedding (`--xpu-arch=sm_*`,
`CajetaXPU.md` §5.2/§10.2). Real CUDA-driver dispatch runtime replacing
the zero-stubs on the NVPTX path: `cuMemAlloc`-backed `Buffer<T>`,
`cuStream`-backed `Stream`/`Event`, and a real kernel launch.

### Step 11 — NVPTX SAXPY device launch (anchored)
> Code: `XpuLaunchAndSaxpyTests.cpp` ("On the NVPTX backend (step 11)
> the same source compiles into a real device launch").

The step-7 SAXPY source compiles unchanged into a real on-device launch
(`cajeta` → `.cubin` → launch). This is the variance-discipline payoff:
one source, CPU-emulation **and** NVPTX. Milestone = design phase 2
("NVIDIA backend, compute only") done.

After step 11, the design roadmap continues with AMD (phase 3), tensor
cores (phase 4), Vulkan (phases 5–6), etc. — those are beyond the
current 11-step plan and tracked in `CajetaXPU.md` §12.

---

## Resuming on a new machine

1. **Check out the branch:** `git checkout cajeta-xpu`.
2. **Build** (both flags are load-bearing):
   ```
   cd build && cmake -G Ninja -DLLVM_DIR=/usr/lib/llvm-20/lib/cmake/llvm .. && ninja
   ```
   On Windows the LLVM path differs — `main` already carries the
   MSYS2/MinGW release fixes (LLP64 type widths, path-separator
   normalization, `_aligned_malloc`, etc.); follow those.
3. **Verify the restored work compiles and the XPU tests pass** before
   starting step 8 — this work was reverted to unblock a release, so a
   green build on the target machine is the first checkpoint. Run the
   XPU subset:
   ```
   ctest -j $(nproc) -R Xpu
   ```
   (Tests: `XpuAttributesTests`, `XpuCoreStdlibTests`, `XpuKernelArgTests`,
   `XpuMirBuilderTests`, `XpuAddressSpaceTests`, `XpuLaunchTests`.)
4. **Then** start step 8 (frontend/MIR completion) per above.
