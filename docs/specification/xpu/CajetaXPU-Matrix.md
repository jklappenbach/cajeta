# Cajeta XPU capability matrix — NVIDIA · AMD · Vulkan · Core

A per-feature matrix of the Cajeta GPU-compute (XPU) backend across the three
target platforms, plus a **Core** column for what the shared layer provides
regardless of vendor. The point of the matrix is to make the *variance surface*
legible: where all three have a native primitive, where one lacks it but Cajeta
supplies an abstraction, and where an abstraction is not cleanly possible (with
the reason).

This is a companion to the backend-variance discipline in
[`CajetaXPU-Variance.md`](CajetaXPU-Variance.md); it extends the original
NVIDIA∩AMD two-backend reckoning with the Vulkan/SPIR-V column. The **stable
launch + kernel-arg FFI contract** these backends dispatch through (the register
trio, the `argv` marshalling per parameter kind, per-launch device targeting, and
the ABI version policy) is frozen in [`CajetaXPU-FFI.md`](CajetaXPU-FFI.md).

> **A fourth backend — CPU — and a runtime dispatcher now exist** (see
> [`CajetaCPU.md`](CajetaCPU.md)). The CPU is deliberately *not* given a
> column here: the three columns below measure the **GPU** variance surface, and
> the CPU is a different shape — it has no hardware grid or coordinate
> intrinsics, so its sole seam fork is the *coordinate source* (the kernel gains
> 9 trailing `i32` coordinate params; `KernelThread`/`Workgroup` reads pull from those
> — the grid→threads model), buffers are flat `addrspace(0)`, the wave is the
> host SIMD vector (Inc 5C), and workgroup barriers run via work-item loop
> fission (Inc 6 — split the kernel at each barrier, loop each region over the
> block; per-block shared memory + context arrays; Inc 7 — multiple barriers per
> uniform loop; Inc 8 — nested uniform loops with barriers; Inc 9 — wave ops +
> barriers composed; Inc 10 — dynamic-sized shared memory). The body walk is the same ~90%
> Core. Separately, the **runtime dispatcher** (in the C runtime, the sole launch
> path for compiled programs) selects among bundled+available backends at startup
> (`CUDA → HIP → Vulkan → CPU`) and routes launch + `KernelBuffer<T>` device memory to
> the winner — so "run anywhere, degrade to CPU" is ordinary dispatch reaching
> its guaranteed terminal. The one launch-ABI asymmetry it exposed is Vulkan's:
> the uniform `kernelParams` argv (buffer handles + raw scalars) doesn't map to
> Vulkan's descriptor-set model, so scalars are wrapped in transient SSBOs from
> per-kernel parameter metadata (rows in §2 already note the descriptor-set fork).

---

## Provenance & how to read this

The three platform columns do **not** have equal evidentiary weight yet, and the
matrix says so explicitly rather than pretending otherwise:

| Column | Status | Basis |
|--------|--------|-------|
| **NVIDIA** | **Measured** — live backend, on-device (the **compute substrate**) | NVPTX → cubin (`ptxas`) → `cuLaunchKernel` via the dlopen'd `nvcuda`. **On-device verified on an RTX 4090 (sm_89, native Windows CUDA — no WSL2/B5 needed)**, 2026-06-16: SAXPY, grid-stride for-each, `@Device` buffer-param helpers, POD-by-value args, `KernelBuffer.slice`, relaxed atomics, scoped memory fences, static+dynamic workgroup-shared reductions, **real CUDA streams + async copies**, **events/fences (cross-stream)**, **managed (`cuMemAllocManaged`) + pinned (`cuMemHostAlloc`) zero-copy memory**, **bindless `KernelBuffer<T>[]`**, and **host spec-constant override** (constant-memory `cuModuleGetGlobal`). Tests: `XpuCudaDispatchDeviceTests` (17), `XpuCudaSpecProbeTests`, plus the C++-driver `XpuSaxpy/Shared/Loop/HostLaunchDeviceTests` — 23/23 green in one process. **Now also on-device verified (2026-06-16):** the **texture+surface runtime** (`cuArrayCreate`/`cuTexObjectCreate`/`cuSurfObjectCreate` — closes the AMD/Vulkan parity gap), the full **`@Wave`/subgroup** family (shuffle/ballot/reduceSum/reduce-family/laneId/width/rotate/prefix-scan via `shfl`/`vote.ballot`/`redux.sync`/sregs), **ray-query** over the portable software BVH (`NvptxTarget.accelImpl() == SoftwareBvh` + a CUDA noun provider that uploads the BVH to a device buffer — AABB/triangle/nearest/barycentrics/front-face all match the CPU path), and **cooperative-matrix** on BOTH the portable flat-tile tier AND the **native tensor-core (`wmma`) tier** — f16/f16→f32 16×16×16 bit-exact on the RTX 4090's tensor cores via the NVVM `wmma.load`/`wmma.mma`/`wmma.store` intrinsics (warp-collective; the opaque fragment layout is handled by the intrinsics, unlike AMD's hand-marshalled RDNA3 layout). Tests: `XpuCudaDispatchDeviceTests.{texture,image}*`, `XpuWaveDeviceTests.nvptx*` (7), `CarameloSpatialIndexDeviceTests.*OnNvptxSoftwareBvh` (5), `XpuCooperativeMatrixDeviceTests.{portable,native}MatmulOnNvptxDevice` + `nvptxCoopMatrixLowersToWmma`. **Now also on-device verified (2026-06-17):** the **OptiX RT-core ray-query verb** — a `RayQuery` `@Kernel` against an OptiX-impl AS (opt-in `CAJETA_GPU_AS_IMPL=optix`) lowers to a separate OptiX program-PTX module (`NvptxOptixRayQuery` + `NvptxRegistration`, the `_optix_*` asm bypasses the cubin) dispatched by `optixLaunch`; four canonical shapes (AABB candidate-count, triangle nearest-hit, triangle candidate getters, committed-triangle per-launch [triangle-count + front-face, a per-launch dynamic ray]) match the software oracle (777) on the 4090, with `Device.supports(Capability.RayQueryRtCore)` reporting the path. Tests: `XpuNvptxOptixEmitTests` (5), `CarameloSpatialIndexDeviceTests.{aabbCount,nearestHit,candidateGetters,triangleCount,frontFace}RayQueryOnOptixDevice`, `OptiXRayQueryProbe` (6). **Future on NVPTX:** AUTO→OptiX flip (deferred — opt-in today) + broader OptiX shapes (non-const-ray getters, AABB generate-intersection), int8/bf16-only and non-16×16×16 / column-major tensor-core configs (v1 native wmma is row-major f16→f32), bounded device-dispatch tables. |
| **AMD** | **Measured** — live backend, on-device | AMDGPU → hsaco, runs on gfx1151 (Strix Halo) via HIP. Emit + on-device tests green. |
| **Vulkan** | **Measured** — live backend, on-device | SPIR-V (descriptor-set SSBOs) → `vkCmdDispatch`. Built 2026-05-30; SAXPY + static-shared tree reduction run on the Strix Halo APU via the radeon (RADV) ICD, and the emitted modules pass strict `spirv-val`. One build-discovered finding shaped the design: **BDA is unavailable**, so the buffer model is descriptor sets (§3). (LLVM 23's barrier emits Vulkan-invalid semantics; a post-emit fixup corrects it — §1.) |

> **Build-discovered correction (2026-05-30).** An earlier draft of this matrix
> projected the Vulkan column on the assumption that **KernelBuffer Device Address**
> would carry buffers as raw pointers (keeping the kernel body identical to
> NV/AMD). The build overturned that: LLVM 23's SPIR-V backend has **no
> PhysicalStorageBuffer/BDA path from IR**, so the only Vulkan buffer model is
> **descriptor-set storage buffers**, which fork the kernel signature *and* the
> body's buffer access — a bigger fork than AMD. The cells below reflect what
> was actually built and measured, not the original projection.

**Cell vocabulary**

- `native` — the platform has a direct hardware/IR primitive; no abstraction layer needed.
- `abstraction: …` — no direct primitive, but Cajeta can synthesize the capability cleanly; the `…` says how.
- `forks: …` — supported, but the implementation diverges from the others in a way that lands on the seam; the `…` says where.
- `not possible: …` — cannot be provided on this platform as specified; the `…` says why.
- `—` — not applicable / not yet a capability on any platform.

**⚑** marks a Vulkan cell that carries a measured *caveat or limitation* the
build surfaced (the fixed compile-time block dim, deferred dynamic shared) — see
the [status section](#status--vulkan-column-now-measured). (The barrier
limitation the build found is now fixed by a post-emit pass — §1.)

The **Core** column describes what the shared `KernelLowering` walk + the
backend-neutral registration provide independent of vendor — the ≈90% that did
*not* fork between NVIDIA and AMD, and which the Vulkan analysis expects to hold
for a third time.

---

## 1. Execution model — coordinate reads & barriers

These are the entire `LoweringTarget` leaf-read interface
(`src/cajeta/xpu/lowering/LoweringTarget.h`). The kernel-body AST walk that
*consumes* them is shared and does not fork.

| Feature | Core | NVIDIA | AMD | Vulkan |
|---------|------|--------|-----|--------|
| KernelThread (local) id `x/y/z` | one `threadId(dim)` seam method | `native` · `nvvm.read.ptx.sreg.tid.*` | `native` · `amdgcn.workitem.id.*` | `native` · `llvm.spv.thread.id.in.group` (LocalInvocationId) |
| Workgroup (block) id `x/y/z` | one `workgroupId(dim)` seam method | `native` · `nvvm.read.ptx.sreg.ctaid.*` | `native` · `amdgcn.workgroup.id.*` | `native` · `llvm.spv.group.id` (WorkgroupId) |
| Workgroup (block) **dim** `x/y/z` | one `workgroupDim(dim)` seam method | `native` · `nvvm.read.ptx.sreg.ntid.*` | `forks: ` **dispatch-packet load** (`amdgcn.dispatch.ptr` + i16 @ off 4/6/8) — no `ntid` intrinsic exists on AMD | `native` · `llvm.spv.workgroup.size` (WorkgroupSize) — the coordinate that was AMD's ugliest fork is a **direct read** here |
| Global id `x/y/z` | `globalId(dim)` default = `workgroupId*workgroupDim + threadId` | `native` (computed default holds) | `native` (computed default holds) | `native` · `llvm.spv.thread.id` (GlobalInvocationId) — **overrides** the computed default with a single hardware read |
| Workgroup barrier | one `workgroupBarrier()` seam method | `native` · `nvvm.barrier.cta.sync.aligned.all` | `forks: ` `amdgcn.s.barrier` wrapped in workgroup-scoped release/acquire fences (LDS visibility) | `abstraction: ` `llvm.spv.group.memory.barrier.with.group.sync` + a **post-emit fixup**. LLVM 23 emits `OpControlBarrier` with Vulkan-forbidden **SequentiallyConsistent** semantics (`VUID-StandaloneSpirv-MemorySemantics-10866`); `SpirvBackend::fixupControlBarriers` rewrites each barrier's memory-semantics to `WorkgroupMemory\|AcquireRelease` (0x108) by adding one uint constant + repointing. The module then passes strict `spirv-val` **and** runs on-device (verified: `XpuVulkanEmitTests.workgroupBarrierIsSpecValid` + the on-device reduction). |
| Num-workgroups (grid dim) read | not yet a Cajeta builtin | `native` (`nctaid`) | `native` (dispatch packet) | `native` · `llvm.spv.num.workgroups` |

**Reading:** every coordinate *read* is `native` on Vulkan, and *two* of them
(`workgroupDim`, `globalId`) are native single-intrinsic reads where AMD needed a
structural workaround — on this axis Vulkan is the least divergent of the three.
The one wrinkle is the **barrier**: LLVM 23 emits Vulkan-forbidden semantics, so
a one-instruction post-emit fixup (see the cell above) rewrites it — after which
the execution-model column is fully native + spec-valid on all three backends.

---

## 2. Address spaces & memory model

Numeric address spaces from `src/cajeta/xpu/core/AddressSpace.h`. NVIDIA and AMD
share the LLVM numbering; SPIR-V uses storage classes (the integers below are the
Khronos storage-class enum values, not LLVM address spaces).

| Address space | Core (Cajeta `AddressSpace`) | NVIDIA `addrspace` | AMD `addrspace` | Vulkan storage class |
|---------------|------------------------------|--------------------|-----------------|----------------------|
| Global (device buffers) | `Global` | 1 | 1 | `native` · StorageBuffer (storage-class 12; LLVM addrspace 11), bound by descriptor set — see §3 |
| Shared (workgroup) | `Shared` | 3 | 3 | `native` · Workgroup (4) |
| Constant (uniform) | `Constant` | 4 | 4 | `native` · Uniform (2) |
| Private (per-thread) | `Private` | 5 | 5 | `native` · Function (7) |
| Alloca / entry-block slots | `allocaAddressSpace()` seam | 0 (generic) | **5 (private)** — AS-0 alloca is invalid IR on AMDGPU | Function (7) — one-liner from `spirvNumberFor(Private)` |
| Generic / flat (default-deref) | `Generic` | 0 (native flat) | 0 (native flat) | `not possible: ` Vulkan SPIR-V has **no Generic storage class** (that's an OpenCL/`Kernel`-capability feature). Benign for Cajeta — the lowerer already tracks every pointer's address space *explicitly*, so it never relies on flat generic deref. |

**Reading:** address space is a near-non-fork across all three (NV/AMD identical;
Vulkan's only real difference is the absence of a generic flat space, which the
explicit-AS lowering sidesteps). The single alloca fork (0/5/7) stays a one-line
seam method.

---

## 3. Kernel-argument ABI — the Vulkan fork (signature + buffer access)

NVIDIA and AMD both pass kernel args as a flat `kernelParams` array: buffers as
raw `addrspace(1)` device pointers, scalars by value. Vulkan has **no
raw-pointer kernel-argument ABI**. This is the architectural fork the Vulkan
bring-up turns on.

| Feature | Core | NVIDIA | AMD | Vulkan |
|---------|------|--------|-----|--------|
| `KernelBuffer<T>` arg as raw device pointer | `addrspace(1) T*` function param | `native` · pointer in `kernelParams[i]` | `native` · pointer in `kernelParams[i]` | `abstraction: ` **descriptor-set SSBO** — each `KernelBuffer<T>` is an `OpVariable StorageBuffer` (set 0, binding = arg index) accessed via `llvm.spv.resource.handlefrombinding` → `getpointer`. (BDA was the intended path but is **not possible** — see the row below.) |
| Scalar arg by value | scalar function param | `native` | `native` | `abstraction: ` carried as a **single-element SSBO** at its own binding (first measured cut). A push-constant block (`llvm.spv.pushconstant.getpointer`) is the natural refinement. |
| POD struct arg by value | aggregate function param; marshalled vtable-stripped field-by-field through `kernelParams` (Item 7) | `native` (by-value kernarg) | `native` (by-value kernarg) | `abstraction: ` carried as a **single descriptor-SSBO** whose element type IS the struct; fields read via `OpCompositeExtract` off the loaded SSA aggregate (no alloca — an aggregate store to Function storage is `spirv-val`-invalid). Read-only, all-primitive fields, no inheritance in v1. |
| Why not KernelBuffer Device Address | — | — | — | `not possible: ` LLVM 23's SPIR-V backend exposes **no PhysicalStorageBuffer/BDA intrinsic** — the `PhysicalStorageBuffer64EXT` strings are capability enum names only. So raw-pointer kernel args can't be reconstituted from IR; descriptor sets are the only model. |
| Where the fork lands on the seam | param-materialization + buffer-access hooks (default preserves NV/AMD) | default | default | `forks: ` **both** the kernel signature (`void main()`, no params; `createKernel` + `materializeParam` hooks) **and** the body's buffer element access (`bufferElementPtr` → `getpointer` instead of GEP). Bigger than AMD: the body walk forks, not just the prologue. |

**Reading:** Vulkan's missing raw-pointer ABI is the headline divergence — and
the build proved it's *not* bridgeable by KernelBuffer Device Address (no IR path),
only by descriptor-set SSBOs. That is the "bigger fork than AMD" the pass set
out to measure: where AMD forked only the coordinate leaf reads, Vulkan forks
the kernel **signature** (`void main()`, descriptor binds) *and* the body's
**buffer element access** (`getpointer` vs GEP). The coordinate reads, control
flow, operators, scalar-slot model, and `addrspace(3)` shared memory still stay
shared — so the core is smaller than NV∩AMD's ≈90%, but still the majority.

---

## 4. Launch / dispatch / runtime

| Feature | Core | NVIDIA | AMD | Vulkan |
|---------|------|--------|-----|--------|
| Grid dim (number of blocks) | launch-config `grid` | `native` · `cuLaunchKernel` gridX | `native` · `hipModuleLaunchKernel` gridX | `native` · `vkCmdDispatch(gridX,1,1)` |
| **Block dim** (workgroup size) | launch-config `block` | `native` · free per-launch param | `native` · free per-launch param | `abstraction: ` **spec-constant `WorkgroupSize`** — a post-emit SPIR-V patch (`injectWorkgroupSizeSpecConstant`, the `fixupControlBarriers` pattern) adds three `OpSpecConstant` (SpecId 0/1/2, default = baked 64) + an `OpSpecConstantComposite` decorated `BuiltIn WorkgroupSize`; the runtime sets them from the launch `block` via `VkSpecializationInfo` at pipeline creation. So the block dim is now a **free per-launch value** (verified on-device with block 128). LLVM 23 has no IR path to this, hence the binary patch. |
| Module load | name-keyed registration (§6) | `native` · `cuModuleLoadData` | `native` · `hipModuleLoadData` | `forks: ` `vkCreateShaderModule` → pipeline-layout → compute-pipeline (heaviest of the three) |
| Argument binding | marshal `argv` | `native` · `kernelParams` array | `native` · `kernelParams` array | `forks: ` push-constant write (BDA, §3) recorded into a command buffer |
| Dispatch + sync | `KernelStream.sync()` | `native` · `cuLaunchKernel` + sync | `native` · launch + `hipDeviceSynchronize` | `forks: ` `vkQueueSubmit` + `vkQueueWaitIdle` |
| Driver acquisition | dlopen vendor lib | `native` · `libcuda.so.1` | `native` · `libamdhip64.so` (+ pinned `libhsa`) | `abstraction: ` dlopen `libvulkan.so.1`, resolve entry points via `vkGetInstanceProcAddr`/`vkGetDeviceProcAddr`; pick the first physical device with a compute queue (radeon/RADV ICD reaches the Strix Halo APU) |

**Reading:** the *launch-config shape* (grid/block + a buffer/scalar arg list)
stays universal at the Cajeta surface, but block dim partially migrates from
launch-time (NV/AMD) to pipeline-creation-time (Vulkan), and the runtime driver
forks heavily. The fork is contained to the driver layer; the language-level
`kernel.launch(...)` syntax stays backend-neutral.

---

## 5. Codegen pipeline (triple · assembler · binary format)

| Stage | Core | NVIDIA | AMD | Vulkan |
|-------|------|--------|-----|--------|
| Device triple | `LoweringTarget` + `*Backend` | `nvptx64-nvidia-cuda` | `amdgcn-amd-amdhsa` | `spirv64-unknown-vulkan1.3-compute` |
| Lowered text form | `addPassesToEmitFile(AssemblyFile)` | PTX | AMDGCN ISA | SPIR-V disassembly (`spirv-dis`-style) |
| Object form | `addPassesToEmitFile(ObjectFile)` | (n/a — PTX is text) | relocatable ELF | **Khronos SPIR-V binary directly** |
| External assembler/linker | — | `ptxas` (PTX → cubin) | `ld.lld -shared` (ELF → hsaco) | `not possible / not needed: ` **none** — LLVM 23 emits the final SPIR-V binary itself. Simplest of the three. |
| Final binary | host-embedded bytes | cubin | hsaco | `.spv` |
| Kernel decoration | `decorateKernel()` seam | `ptx_kernel` CC **+ `nvvm.annotations`** | `amdgpu_kernel` CC, no metadata | `forks: ` `OpEntryPoint GLCompute` + execution-mode marking (LocalSize) |

**Reading:** Vulkan is the *only* one of the three that needs no external
assembler or linker — the TargetMachine produces the shippable binary directly.

---

## 6. Registration & frontend (the backend-neutral core)

| Feature | Core | NVIDIA | AMD | Vulkan |
|---------|------|--------|-----|--------|
| In-process module registration | `__cajeta_xpu_register_module(name, bytes, len)` via `llvm.global_ctors`, keyed by entry name | `native` (cubin bytes) | `native` (hsaco bytes) | `native` (spv bytes) — symbol unchanged, only the byte format differs |
| `@Kernel` / `@Device` / `@Host` recognition | frontend | `native` | `native` (inherited free) | `native` (inherited free) |
| `KernelArg` admissibility (`XPU-K01`) | frontend | `native` | `native` | `native` |
| Launch grammar / borrow-scope checking | frontend | `native` | `native` | `native` |
| `shared` placement keyword | frontend → `addrspace(3)` | `native` | `native` | `native` (→ Workgroup storage class) |
| Control flow, operators, casts, mem2reg slot model | shared AST walk | `native` | `native` | `native` (body walk does not fork) |

**Reading:** the entire frontend + the name-keyed registration are vendor-neutral
and were *never touched* when AMD landed; the Vulkan analysis expects the same —
the third backend inherits all of it for free.

---

## 7. Shared (workgroup) memory

| Feature | Core | NVIDIA | AMD | Vulkan |
|---------|------|--------|-----|--------|
| Static shared `Shared<T> t = shared T[N]` | internal `[N x T] addrspace(3)` global | `native` | `native` | `native` · `addrspace(3)` → Workgroup storage class (a **non-fork**: the shared lowerer's hardcoded `addrspace(3)` is exactly Workgroup on SPIR-V). **Measured on-device** — the tree reduction returns the correct sum, with a spec-valid barrier (§1). |
| Dynamic shared `shared T[expr]` | external unsized `[0 x T] addrspace(3)`, sized at launch | `native` · `cuLaunchKernel` `sharedMemBytes` | `native` · `hipModuleLaunchKernel` `sharedMemBytes` (= groupMemBytes) | `abstraction: ` **spec-constant array length** — the lowerer emits a concrete internal `[N x T]` Workgroup array (kept typed, not decayed to `T*`), and a post-emit patch (`injectDynamicSharedSpecConstant`) makes its `OpTypeArray` length an `OpSpecConstant` (SpecId 3); the runtime sets it from the launch's `sharedBytes:` (÷ elem size) via `VkSpecializationInfo` at pipeline creation. So it is sized **per pipeline**, not per-`vkCmdDispatch` (one dynamic array, 4-byte elem for now). Verified on-device (`XpuVulkanDispatchDeviceTests.dynamicSharedOnDevice`). |

**Reading:** static shared is a clean `native` three-way. Dynamic shared is the
second place (after block dim, §4) where a value Cajeta treats as launch-time on
NV/AMD becomes pipeline-creation-time on Vulkan — bridgeable by a spec constant +
pipeline cache, but it cannot be a free per-dispatch scalar, and the matrix says
why.

---

## 8. Wave / subgroup ops (the `@Wave` feature — readlane + ballot + reduce built & measured)

Wave ops are the archetypal variance-shaped feature and the headline test of the
seam — and the seam held. `Wave.shuffleSync` (readlane / shuffle-by-index),
`Wave.ballotSync`, `Wave.reduceSum`, and `Wave.laneId` lower through the shared AST
walk with only five `LoweringTarget` methods (`waveWidth`/`waveShuffle`/`waveBallot`/
`waveReduceSum`/`waveLaneId`) forking. Built 2026-05-30; emit-verified on all three
GPU backends, run on-device on AMD + Vulkan + **NVIDIA (RTX 4090, 2026-06-16 —
shuffle/ballot/reduceSum/reduce-family/laneId/width/rotate/prefix-scan)**; **wave =
SIMD lane on the CPU backend (Inc 5C, 2026-05-31)**.

| Feature | Core | NVIDIA | AMD | Vulkan | CPU (Inc 5C) |
|---------|------|--------|-----|--------|--------------|
| Wave width | `Wave.width()` → i32 | `native` · `read.ptx.sreg.warpsize` (32) | `native` · `amdgcn.wavefrontsize` (32/64) | `native: ` `spv.subgroup.size` → `OpLoad` of the **SubgroupSize** builtin (`loadBuiltinInputID`). NOT `spv.wave.get_lane_count`, which the SPIR-V backend still can't select through LLVM 23 — the sibling `spv.subgroup.size` is selectable, so `Wave.width()` now runs on-device (verified on RADV/gfx1151). | the host's native SIMD width W (16 AVX-512 / 8 AVX2); folded to a constant in a vectorized kernel |
| Lane id | `Wave.laneId()` → i32 | `native` · `read.ptx.sreg.laneid` | `native` · `amdgcn.mbcnt.{lo,hi}` | `native` · `spv.subgroup_local_invocation_id` (validated) | `tid.x % W` |
| Shuffle / readlane | `Wave.shuffleSync(v, lane)` | `native` · `nvvm.shfl.sync.idx.i32` | `native` · `amdgcn.readlane` | `native` · `spv.wave.readlane` (→ `OpGroupNonUniformShuffle`) | VFABI variant · per-lane gather `val[src[i]]` |
| Ballot | `Wave.ballotSync(pred)` → i64 | `native` · `nvvm.vote.ballot.sync` (i32→i64) | `native` · `amdgcn.ballot.i32` (i32→i64) | `native` · `spv.wave.ballot` (`<4 x i32>`, low 64 → i64) | VFABI variant · `bitcast <W x i1> → iW` |
| Reduce (sum) | `Wave.reduceSum(v)` → i32 | `native` · `nvvm.redux.sync.add` (sm_80+) | `native` · `amdgcn.wave.reduce.add` | `native` · `spv.wave.reduce.sum` (→ `OpGroupNonUniformIAdd`) | VFABI variant · `broadcast(vector.reduce.add)`; masked for divergence |

**Reading:** wave ops are native on all three. **The reduce probe overturned its own
hypothesis.** The guess (recorded here in the prior pass) was that reduce would
*invert* comprehensiveness — one native intrinsic on Vulkan vs. a shuffle/DPP
butterfly sequence on NV/AMD. The build showed the opposite: **all three expose a
single hardware wave-reduce intrinsic in LLVM 23**, so reduce maps as cleanly as
shuffle/ballot (`waveReduceSum` is a one-liner per backend). The real asymmetries are
narrower and were only visible by running it:
- **NVPTX `redux.sync` is gated on sm_80+** (Ampere); below that a butterfly-shuffle
  fallback would be needed (out of scope — the emit test targets `sm_89`).
- **AMDGPU folds a *uniform-constant* operand** to `wave.reduce.add` back to the
  operand instead of summing it (`reduceSum(1)` returned 1 on-device); a *divergent*
  operand takes the real reduction path. The device test feeds a buffer of 1s so the
  reduction actually runs.
- **Ballot shape still forks** (NV i32, AMD i32 wave32, Vulkan `<4 x i32>` 128-bit —
  the Core API normalizes to i64).

Tests: `XpuWaveEmitTests` (3 backends + `spirv-val`, shuffle/ballot/reduce),
`XpuWaveDeviceTests` (shuffle/ballot/reduce on-device on AMD + Vulkan + **NVIDIA**;
the `nvptx*` arms cover shuffle/ballot, reduceSum, laneId, width, rotate, the
reduce family, and prefix scans; the reduce/width checks are width-agnostic — sum
of 1s over a full wave == wave width ∈ {32, 64}, which is 32 on NVIDIA).

---

## 9. Atomics & memory ordering

| Feature | Core | NVIDIA | AMD | Vulkan |
|---------|------|--------|-----|--------|
| Atomic RMW on global / shared | LLVM `atomicrmw` + scope | `native` | `native` | `native` · SPIR-V atomic ops with explicit Scope + Memory-Semantics — **int atomics on-device RTX 4090 (NVIDIA Vulkan)** + RADV |
| Int atomics (`KernelBuffer<int32>.atomic{Add,Sub,Min,Max,And,Or,Xor,Exchange,CompareExchange}`) | core `OpAtomicI*` (no ext) | `native` | `native` | `native` · core ops — on-device (RTX 4090 + RADV) |
| Float atomic **add** (`KernelBuffer<float32>.atomicAdd`) | `atomicrmw fadd` | `native` | `native` | `native` · `OpAtomicFAddEXT` (VK_EXT_shader_atomic_float / `shaderBufferFloat32AtomicAdd`) — **enabled at device creation; on-device RTX 4090** (`floatAddAtomicsRunOnVulkanDevice`) + RADV. (The extension+feature MUST be enabled or NVIDIA device-losts; this was a latent bug.) |
| Float atomic **min/max** (`KernelBuffer<float32>.atomic{Min,Max}`) | `atomicrmw fmin/fmax` | `native` | `native` | `OpAtomicFMin/MaxEXT` needs **VK_EXT_shader_atomic_float2** — present on RADV (on-device), **ABSENT on NVIDIA**. No portable SPIR-V fallback: an integer-bit-trick / CAS loop both need an integer atomic on the float buffer, which logical addressing forbids (no pointer reinterpretation). So the device test **skips** when float2 is unavailable (`VulkanDriver::shaderAtomicFloatMinMaxAvailable`). |
| Scoped fences | `Barrier.workgroupMemory()` / `.deviceMemory()` — a memory fence with NO thread rendezvous (the `memoryFence(scope, order)` seam), AcqRel default + optional `MemoryOrder`. Device-verified CPU/VK/AMD/**NVIDIA**. The host-facing `Fence` class is a separate, unrelated thing (stream sync). | `native` · `membar.cta` (workgroup) / `membar.gl` (device) — **on-device RTX 4090** (`memoryFenceRoutesToCudaOnDevice`) | `native` · `agent`/`workgroup`-scoped `acq_rel` fence (no `s_barrier`) — on-device gfx1151 | `native` · `OpMemoryBarrier` via `llvm.spv.{group,device}.memory.barrier` — `spirv-val` clean + on-device RADV |
| Memory order (user-selectable) | `MemoryOrder` enum (`Relaxed`/`Acquire`/`Release`/`AcqRel`/`SeqCst`) — an optional **compile-time-constant** trailing arg on kernel atomics + fences (e.g. `out.atomicAdd(i, 1, MemoryOrder.Relaxed)`); default unchanged. Threaded through the atomic/fence seams. Prereq: enum constants now resolve in kernel bodies. | `native` · honours all five (relaxed → `monotonic` atomicrmw) — **relaxed-atomic counter on-device RTX 4090** (`relaxedAtomicCounterRoutesToCudaOnDevice`) | `native` · honours all five — relaxed-atomic counter on-device gfx1151 | **clamps** `Relaxed`/`SeqCst` → `AcqRel` (Vulkan rejects a bare-relaxed device atomic — needs storage-class acq/rel) — `spirv-val` clean + relaxed-counter on-device RADV |

---

## 10. Deferred / not-yet capabilities (no backend has these)

Tracked here so the matrix is honest about the frontier, not just the floor.
These are backend-neutral gaps, unaffected by the Vulkan column.

| Capability | Status |
|------------|--------|
| Real `launch(stream, grid:, block:)(args)` postfix grammar | **done** — the stream handle threads through to `cuLaunchKernel`/`hipModuleLaunchKernel`; async copies + cross-stream Event deps device-verified on HIP/CUDA. Vulkan/CPU accept the handle but serialize (no overlap yet). |
| Launch-site resolution into `XpuMirLaunchSite` | placeholder |
| MIR body-op walker (`Op_ThreadId`, `Op_BarrierWorkgroup`, …) | empty |
| `@Device` user-defined helper calls | ✅ scalar + KernelBuffer<T> params, same-class, helper-chains; alwaysinline-folded per backend (Vulkan: `bufferParamType` = the storage-buffer handle + AlwaysInliner before SPIR-V isel); verified on AMD & Vulkan |
| for-each parallel loops | **GPU ✅** — grid-stride `for (i, T v : buf.range(n))` ⇒ `for (i=globalId.x; i<n; i+=gridSize.x){ v=buf[i]; … }`; new `LoweringTarget::gridSize` (NVPTX nctaid·ntid, AMD dispatch-packet `grid_size`, SPIR-V NumWorkgroups·WorkgroupSize); verified on AMD & Vulkan (grid<n forces the stride). **CPU ✅** — coord ABI extended 9→12 (adds `nctaid.xyz` = gridDim), so `gridSize = nctaid·ntid` threads runtime→thunk→wrapper→kernel; verified `XpuCpuDispatchTests.gridStrideForEachOnCpu` (grid=2·block=64 over n=1024, 8 elems/work-item, full coverage). |
| Labeled `break` / `continue` | ✅ done — `label: for(…)` + `break label;`/`continue label;` jump to an outer loop; device-verified CPU + Vulkan (`labeledBreakContinueOn{Cpu,Device}`). |
| Bounded device-side dispatch (function pointers) | ✅ done — a function-typed value is an `i32` tag over a closed set of `@Device` statics; a call lowers to an **if/else-if chain of DIRECT calls** (SPIR-V has no fn pointers — identical on all four backends, no backend-specific code). New host **array-of-function type** `((T)->R)[]` is the surface: a literal table `((int32)->int32)[] ops = { A::f, B::g }` indexed at runtime `ops[i](x)` (+ single-ref variable form). Out-of-range index = defined no-op (zero result, no trap). Verified on **CPU + Vulkan/RADV + AMD/gfx1151** (`deviceDispatchTableOnDevice`); **NVIDIA emit-only** (`lowersDeviceDispatchToBranchChain` asserts zero indirect calls + valid PTX). v1: non-capturing `@Device` statics, compile-time-bounded set. |
| 2D/3D launch | ✅ done — 3-D launch ABI; CUDA/HIP 3-D grid+block; Vulkan 3-D grid (baked block, §4); CPU 3-D grid + block + barrier fission |
| Multi-arch bundling (fatbin) | **AMD ✅** — `--xpu-arch=gfx1100,gfx1151` → `clang-offload-bundle` via `assembleHsacoBundle`, `hipModuleLoadData` selects the device arch (verified on-device). NVIDIA fatbin parallel deferred (no `ptxas`/`fatbinary` on this box). |
| Texture / Sampler types | ✅ done — `Texture2D` + `Sampler` args + `tex.sample(s, u, v)` (2-D sampled, float32 texel, normalized coords, nearest/bilinear, clamp/wrap, explicit LOD 0). Per-backend `LoweringTarget::sampleTexture` seam — see §11. CPU + Vulkan (RADV) + AMD (gfx1151) verified on-device; NVIDIA emit-only. Needs LLVM 23 (Vulkan `samplelevel`). |
| `@PushConstant` (Vulkan-only surface) | deferred — note: BDA already *uses* a push-constant block internally (§3), so the plumbing arrives early on Vulkan |
| POD structs as kernel args without explicit `implements KernelArg` | ✅ done — a plain `class { <primitive fields> }` (no inheritance, no marker) is admitted by value (`isPodStruct`); marshalled vtable-stripped field-by-field through `kernelParams`; the kernel reads fields with `p.field` (an `extractvalue` / `OpCompositeExtract` off the SSA aggregate — **no alloca**, so SPIR-V logical addressing stays valid). NVPTX/AMDGPU by-value kernarg, CPU thunk aggregate load, Vulkan single descriptor-SSBO. Verified on AMD (gfx1151) + Vulkan (RADV). Read-only, all-primitive fields, no inheritance in v1. |

---

## 11. The abstraction-layer ledger (the user's core question)

Where a platform lacks a native primitive — can Cajeta provide it, and if not, why?

**Provided cleanly (abstraction layer bridges the gap):**

- **Vulkan · raw-pointer buffer args** → **descriptor-set SSBOs** (`resource.handlefrombinding` + `getpointer`); scalars as single-element SSBOs. Forks signature + body buffer access; the rest of the body stays shared. *(§3)*
- **Vulkan · workgroup barrier** → `group.memory.barrier.with.group.sync` + a one-instruction post-emit fixup (`SpirvBackend::fixupControlBarriers`) that corrects LLVM 23's Vulkan-forbidden SequentiallyConsistent semantics to `WorkgroupMemory|AcquireRelease`. Now passes strict `spirv-val` and runs on-device. *(§1)*
- **Vulkan · block dim** → fixed compile-time `LocalSize` (first cut); spec-constant `LocalSizeId` is the refinement. *(§4)*
- **AMD · workgroup dim read** → dispatch-packet load (no `ntid` intrinsic, but the value is recoverable). *(§1)*
- **All · `tex.sample(sampler, u, v)`** (Item 8) → one `LoweringTarget::sampleTexture` seam, four native realizations: CPU C bilinear (`__cajeta_xpu_cpu_tex_sample`); Vulkan `llvm.spv.resource.samplelevel` → `OpImageSampleExplicitLod` (image + sampler descriptors); AMD `__ockl_image_sample_2D` → `image_sample` (ROCm device-lib **hybrid-linked only for sampling kernels** — reuses ROCm's SRD build + coord normalization rather than hand-packing gfx descriptors); NVIDIA `llvm.nvvm.tex.unified.2d` → PTX `tex.2d`. Texture marshals like a `KernelBuffer` handle, `Sampler` like a by-value POD; on AMD/NVIDIA the sampler state rides the texture object (built per-launch). CPU+Vulkan+AMD on-device, NVIDIA emit-only. *(needs LLVM 23 for Vulkan)*

- **Texture dimensions** — `Texture1D/2D/3D/2DArray` + fetch/sample work on CPU + Vulkan/RADV + AMD/gfx1151 on-device (NVIDIA emit-only). **`TextureCube` sample and mipmapped `Texture2D` + explicit-LOD (`sampleLod`/`fetchLod`, incl. trilinear cross-level blend) now work on CPU + Vulkan/RADV + AMD/gfx1151 on-device** (NVIDIA emit-only). HIP's high-level array APIs don't support these on gfx1151 — `hipMalloc3DArray[hipArrayCubemap]` → invalid-arg, `hipMallocMipmappedArray` → `hipErrorNotSupported(801)`, unchanged across ROCm 7.2.2 + 7.11.0 (a documented HIP gap — the official "Texture Management [Not supported]" group — not a hardware limit). cajeta **emulates** both on AMD: cubemaps via a LAYERED `hipArray` + in-kernel face projection, and mipmaps via a **hand-built gfx11 image SRD over an addrlib-tiled `hipMalloc`** (option B), sampled through the unchanged `__ockl_image_sample_lod_2D` seam — proven bit-exact on-device (`plans/gpu/xpu/probes/mipprobe.cpp`). The mip emulation rides the optional `libcajeta_amdtex` helper (vendored addrlib, dlopen'd like libamdhip64); where it's absent or the gfx arch isn't in its config table the AMD mip path degrades to unsupported and the Vulkan backend remains the fallback. Scope: explicit-LOD only (no auto-LOD/derivatives — N/A in compute), no anisotropic, no seamless cube edges; v1 mip format = R32F. *(`XpuHipDispatchDeviceTests.{mipmapFetchAndSampleLod,mipTrilinearBlend,textureCubeSample}RoutesToHipOnDevice`; project memory `project_amd_mip_emulation_shipped`)*

**Not cleanly possible (and why):**

- **Vulkan · KernelBuffer Device Address (raw pointers)** → LLVM 23's SPIR-V backend has **no PhysicalStorageBuffer/BDA intrinsic** from IR. This forced the descriptor-set model above — the central build-discovered finding. *(§3)*
- **Vulkan · generic/flat address space** → SPIR-V has no Generic storage class outside the OpenCL `Kernel` capability. **Benign**: the lowerer tracks every pointer's address space explicitly and never relies on flat generic deref. *(§2)*
- **Vulkan · dynamic shared as a *free per-dispatch* byte count** → workgroup arrays are sized at pipeline creation, not per-`vkCmdDispatch`. Deferred this pass. *(§7)*

**Needed nowhere (already shared across all three):** the entire kernel-body AST
walk, the coordinate leaf reads, `addrspace(3)` shared memory (→ Workgroup on
SPIR-V, a non-fork), the name-keyed registration symbol, and the whole frontend.

---

## Status — Vulkan column now *measured*

The Vulkan backend was built 2026-05-30 by threading SPIR-V through the same
`LoweringTarget` seam AMD uses. All increments landed; the on-device tests run
on the Strix Halo APU via RADV.

| Increment | Delivered | Tier | Tests |
|-----------|-----------|------|-------|
| 0 | `Backend::Spirv` enum + `--xpu-backend=vulkan`/`--xpu-emit=spv\|spvasm` CLI | refactor | NV/AMD stay green |
| 1 | SPIR-V emission (`SpirvBackend`) — §5 | Tier-0 | `XpuVulkanEmitTests` |
| 2 | `SpirvTarget` leaf reads (§1) + the signature/buffer-access fork (§3) | Tier-0 | `XpuVulkanEmitTests` (+ NV/AMD green) |
| 3 | `VulkanRegistration` + AOT `--xpu-emit=spv` (§6) | Tier-0 | `XpuVulkanAotCliTests` |
| 4 | `VulkanDriver` (descriptor sets + pipeline + dispatch) + on-device SAXPY (§3,§4) | Tier-1 | `XpuSaxpyVulkanDeviceTests` |
| 5 | static workgroup-shared tree reduction on-device (§7); dynamic deferred | Tier-1 | `XpuSharedVulkanDeviceTests` |
| 5b | barrier post-emit fixup → spec-valid `OpControlBarrier` (§1) | Tier-0 | `XpuVulkanEmitTests.workgroupBarrierIsSpecValid` |

One cell carries a measured caveat — block dim is fixed at SPIR-V compile time
(§4); dynamic shared is deferred. The barrier limitation found during the build
is now **fixed** (§1). Everything else in the Vulkan column is on-device-measured
and spec-valid.

---

## Part C cutting-edge SPIR-V (via the `cajeta-llvm` fork, LLVM 23)

Both delivered on-device on the RADV / STRIX_HALO (Radeon 8060S) box (2026-06-04),
through the fork's `cajeta-spirv` branch. The native matrix-core path is now wired
on all three GPU backends (Vulkan `OpCooperativeMatrix`, AMD RDNA3 WMMA, **NVIDIA
tensor-core `wmma`**, 2026-06-16); the native RT-core ray-query seam is on-device-
validated on RADV (Linux) + the RTX 4090 (Windows) via Vulkan, **and on NVIDIA CUDA
via OptiX (2026-06-17)** — only the AMDGPU backend stays software-BVH-only.
**NVIDIA now runs both via the portable/software tiers on-device (RTX 4090,
2026-06-16):** cooperative-matrix on the portable flat-tile tier (f16/f16→f32
bit-exact), and ray-query over the portable software BVH (the same
`SoftwareRayQuery` walk the CPU uses — AABB/triangle/nearest/barycentrics/front-face
all match the CPU results). **NVIDIA cooperative-matrix now ALSO runs on the native
tensor-core (`wmma`) tier** — 16×16×16 f16→f32 row-major, bit-exact on the RTX 4090
via the NVVM `wmma.load`/`wmma.mma`/`wmma.store` intrinsics (the cajeta seam derives
the fragment struct type from the load intrinsic so load/mma/store agree; the warp-
collective launch uses a full warp). So all three GPU backends now have a native
cooperative-matrix path (Vulkan `OpCooperativeMatrix`, AMD RDNA3 WMMA, NVIDIA
tensor-core wmma). **The native RT-core ray-query path is now on-device-validated
on the RTX 4090's Windows Vulkan backend** (2026-06-16): AABB + triangle BLAS/TLAS
traced via `OpRayQuery` on the RT cores, results matching the software-BVH oracle,
with AUTO resolving to native and `AsImpl.Software`/`AsImpl.Native` selectable.
**NVIDIA CUDA also reaches its RT cores via OptiX (2026-06-17):** a `RayQuery`
`@Kernel` against an OptiX-impl AS lowers to a separate OptiX program-PTX module
(`NvptxOptixRayQuery`) dispatched by `optixLaunch` — four canonical shapes (AABB
candidate-count, triangle nearest-hit, triangle candidate getters, committed-triangle /
front-face) match the software oracle (777) on the 4090. **Under M3, AUTO on CUDA prefers
the RT cores** (`Device.supports(Capability.RayQueryRtCore)`): the AS is a multi-impl noun
that retains the software floor and builds the OptiX rep lazily on the first supported-shape
launch, with launch-time selection routing Unsupported shapes back to the floor (no fault,
no opt-in — see `docs/gpu/RayQuery.md` §5–§6). The software BVH stays the portable floor
(CPU/AMD, non-RT Vulkan, and any Unsupported-shape kernel).
**AMD gets
the symmetric ray-query software-BVH path** (`AmdgpuTarget.accelImpl() == SoftwareBvh`
+ a HIP noun provider) — code-complete + compile-verified, on-device-PENDING the
gfx1151 box (both NVPTX and AMDGPU had the same latent `accelImpl()` gap that made
`RayQuery` throw; now fixed). AMD's cooperative-matrix is already native WMMA.

| Feature | Vulkan (Cajeta's flavor) | On-device |
|---|---|---|
| **Cooperative matrix** `CooperativeMatrix<T,Rows,Cols,Use>` | `OpTypeCooperativeMatrixKHR` + load/store/mul-add via `llvm.spv.cooperative.matrix.*`; mandates `OpMemoryModel Logical VulkanKHR`. CM1–CM5 + multi-tile GEMM | ✅ f16/f16→f32 16×16×16 bit-exact on RDNA3 WMMA cores; 64×64×64 tiled GEMM bit-exact (needed the `SPIRVFixupMergePlacement` backend fix). CM6 LDS-staging deferred |
| **Ray query + accel structures** `RayQuery` / `AccelerationStructure` | `OpTypeRayQueryKHR` / `OpTypeAccelerationStructureKHR` + init/proceed/get via `llvm.spv.ray.query.*`; AS bound through the standard `handlefrombinding` path | ✅ spatial-index (RTNN) pattern over AABB + triangle geometry, on-device; consumed by Caramelo `SpatialIndex` (3c) |

`float16` is IEEE binary16 (`getHalfTy`, not bfloat) to match the device's
`VK_COMPONENT_TYPE_FLOAT16_KHR` cooperative-matrix config.

**Ray-query AS impl, by platform.** The `AccelerationStructure` noun has two impls
(CajetaGPU §1.5): the **native** Vulkan BLAS+TLAS (`OpRayQuery`, RT cores) and the
**portable software BVH** (the `SoftwareRayQuery` walk over a `KernelBuffer<float32>`,
shared with CPU/NVPTX/AMD). On any **ray-query-capable Vulkan device — RADV on Linux
AND the RTX 4090 on Windows** — AUTO resolves to **native**; the `caj_native_
rayquery_available` resolver and the `RayQueryNative` capability share one condition
(active backend is Vulkan + the device advertises ray query), so they never
disagree. On a non-RT Vulkan device (or the CPU/NVPTX/AMD backends, which have no
native inline ray-query seam) AUTO resolves to the software BVH floor. The impl is
selectable per AS: `AccelerationStructure.of(.., AsImpl.Native | AsImpl.Software)`
or the process-wide `CAJETA_GPU_AS_IMPL=native|software` override; the chosen impl
is recorded on the noun (`AccelerationStructure.implTag()`) and the verb follows it.

**CUDA gets a third impl — OptiX (`implTag` 2, 2026-06-17).** NVIDIA has no *inline*
ray-query seam, so its RT cores are reached through an OptiX **pipeline** that the
compiler emits as a lowering of the same inline `RayQuery` verb (`NvptxOptixRayQuery`).
**M3 made AUTO prefer OptiX safely** via a multi-impl noun + launch-time selection: an
`AccelerationStructure` always retains the software-BVH floor and (on CUDA, under AUTO)
builds the OptiX rep **lazily** on the first supported-shape launch — `implSet()` reports
the live set (`1<<impl` per rep; e.g. `Software|OptiX = 5`), while `implTag()` keeps the
single-primary read. At launch the runtime picks, per consuming kernel, the best impl it
can traverse (a registered OptiX program + the AS carrying OptiX → `optixLaunch`; else the
software cubin over the floor — an Unsupported shape can no longer be handed an `OptixAs*`
to misread). `=optix` forces eager OptiX (+ retained floor); `=software` forces the floor.
`Device.supports(Capability.RayQueryRtCore)` reports the OptiX path per device (distinct
from `RayQueryNative`, false on CUDA). This supersedes the earlier opt-in-only / deferred-
flip policy (the fault that made it unsafe is gone). See `documents/gpu-rayquery-optix/
rayquery-optix-m3-multiimpl-{spec,plan}.md`.

**On-device (RTX 4090, Windows, 2026-06-16):** native AABB + triangle ray query —
hits/primitive-index/T/barycentrics/front-face/nearest-hit — all match the software
oracle; `autoRecordsNativeImplOnDevice` proves AUTO records native (impl 1) on the
4090, and `forcedNativeOfApiOnDevice`/`forcedSoftwareOfApiOnDevice` prove both impls
are selectable and agree. Bugs fixed on the way to native: the native **triangle**
builder recorded a BLAS as the traceable AS (must be a TLAS — NVIDIA returns no
hits, RADV tolerated it; now wraps the BLAS in a TLAS like the AABB path), and the
runtime resolver + `RayQueryNative` capability were both Win32-gated (now removed —
the 4090's native RT-core path is wired and validated). AMD's **HIP** backend still
uses the software BVH. NVIDIA's **CUDA** backend is growing an **OptiX RT-core tier**
(`CAJ_AS_IMPL_OPTIX`): NVIDIA RT cores are reached via OptiX (a pipeline model, no
inline-RQ intrinsic), so M0 proved OptiX results match the software oracle on the
4090 and M1 landed the runtime AS provider (build + record the OptiX AS on-device,
`implTag` 2); the traversal verb (NVPTX→OptiX codegen + `optixLaunch`) is in progress
(M2). OptiX is a compile-time-only dependency — the engine is the driver's
`nvoptix.dll`. See `documents/gpu-rayquery-optix/`.

---

*Generated 2026-05-30; updated 2026-06-04 with Part C cooperative-matrix (CM1–CM5 +
GEMM) and ray-query (Inc 1–3c) on-device landings. **Updated 2026-06-16: the
NVIDIA compute substrate is now on-device-measured** on an RTX 4090 (sm_89, native
Windows CUDA — the B5 WSL2 runner turned out to be unnecessary). Two real bugs were
found and fixed by the on-device bring-up: (1) `NvptxRegistration` never emitted the
`__cajeta_xpu_register_kernel_params` ctor (AMD/Vulkan did), so `find_kparams`
returned NULL on CUDA and bindless/texture/image arg translation silently no-op'd;
(2) `__cajeta_xpu_register_module` dropped re-registration of an existing kernel
name (first-wins) instead of overwriting + resetting the cached module — a
use-after-free for a second JIT'd program reusing a kernel name. Both fixed; all
three GPU backends + CPU are now on-device-measured for the compute substrate.
**Follow-up landed the same day:** NVIDIA wave ops, the texture+surface runtime,
ray-query (portable software BVH — new `NvptxTarget.accelImpl() == SoftwareBvh` +
a CUDA noun provider), and cooperative-matrix (portable flat-tile tier) are now all
on-device-verified on the RTX 4090. What remains future on NVPTX is only the
*native* tensor-core (`wmma`/`mma.sync`) and RT-core seams — the portable/software
tiers run today and match the CPU/Vulkan results.
See [`CajetaXPU-Variance.md`](CajetaXPU-Variance.md) for the NVIDIA∩AMD variance
discipline and `cajeta-gpu-plan.md` Part C.*
