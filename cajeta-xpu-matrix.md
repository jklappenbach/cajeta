# Cajeta XPU capability matrix — NVIDIA · AMD · Vulkan · Core

A per-feature matrix of the Cajeta GPU-compute (XPU) backend across the three
target platforms, plus a **Core** column for what the shared layer provides
regardless of vendor. The point of the matrix is to make the *variance surface*
legible: where all three have a native primitive, where one lacks it but Cajeta
supplies an abstraction, and where an abstraction is not cleanly possible (with
the reason).

This is a companion to the "NVIDIA∩AMD overlap reckoning" in
[`cajeta-xpu.md`](cajeta-xpu.md) and the AMD bring-up log
[`cajeta-amd.md`](cajeta-amd.md). It extends that two-backend reckoning with the
Vulkan/SPIR-V column.

> **A fourth backend — CPU — and a runtime dispatcher now exist** (see
> [`cajeta-cpu.md`](cajeta-cpu.md)). The CPU is deliberately *not* given a
> column here: the three columns below measure the **GPU** variance surface, and
> the CPU is a different shape — it has no hardware grid or coordinate
> intrinsics, so its sole seam fork is the *coordinate source* (the kernel gains
> 9 trailing `i32` coordinate params; `Thread`/`Workgroup` reads pull from those
> — the grid→threads model), buffers are flat `addrspace(0)`, the wave is the
> host SIMD vector (Inc 5C), and workgroup barriers run via work-item loop
> fission (Inc 6 — split the kernel at each barrier, loop each region over the
> block; per-block shared memory + context arrays). The body walk is the same ~90%
> Core. Separately, the **runtime dispatcher** (in the C runtime, the sole launch
> path for compiled programs) selects among bundled+available backends at startup
> (`CUDA → HIP → Vulkan → CPU`) and routes launch + `Buffer<T>` device memory to
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
| **NVIDIA** | **Measured** — live backend, on-device | NVPTX → cubin, runs via CUDA driver. Emit + on-device tests green. |
| **AMD** | **Measured** — live backend, on-device | AMDGPU → hsaco, runs on gfx1151 (Strix Halo) via HIP. Emit + on-device tests green. |
| **Vulkan** | **Measured** — live backend, on-device | SPIR-V (descriptor-set SSBOs) → `vkCmdDispatch`. Built 2026-05-30 (see [`cajeta-vulkan.md`](cajeta-vulkan.md)); SAXPY + static-shared tree reduction run on the Strix Halo APU via the radeon (RADV) ICD, and the emitted modules pass strict `spirv-val`. One build-discovered finding shaped the design: **BDA is unavailable**, so the buffer model is descriptor sets (§3). (LLVM 22's barrier emits Vulkan-invalid semantics; a post-emit fixup corrects it — §1.) |

> **Build-discovered correction (2026-05-30).** An earlier draft of this matrix
> projected the Vulkan column on the assumption that **Buffer Device Address**
> would carry buffers as raw pointers (keeping the kernel body identical to
> NV/AMD). The build overturned that: LLVM 22's SPIR-V backend has **no
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
| Thread (local) id `x/y/z` | one `threadId(dim)` seam method | `native` · `nvvm.read.ptx.sreg.tid.*` | `native` · `amdgcn.workitem.id.*` | `native` · `llvm.spv.thread.id.in.group` (LocalInvocationId) |
| Workgroup (block) id `x/y/z` | one `workgroupId(dim)` seam method | `native` · `nvvm.read.ptx.sreg.ctaid.*` | `native` · `amdgcn.workgroup.id.*` | `native` · `llvm.spv.group.id` (WorkgroupId) |
| Workgroup (block) **dim** `x/y/z` | one `workgroupDim(dim)` seam method | `native` · `nvvm.read.ptx.sreg.ntid.*` | `forks: ` **dispatch-packet load** (`amdgcn.dispatch.ptr` + i16 @ off 4/6/8) — no `ntid` intrinsic exists on AMD | `native` · `llvm.spv.workgroup.size` (WorkgroupSize) — the coordinate that was AMD's ugliest fork is a **direct read** here |
| Global id `x/y/z` | `globalId(dim)` default = `workgroupId*workgroupDim + threadId` | `native` (computed default holds) | `native` (computed default holds) | `native` · `llvm.spv.thread.id` (GlobalInvocationId) — **overrides** the computed default with a single hardware read |
| Workgroup barrier | one `workgroupBarrier()` seam method | `native` · `nvvm.barrier.cta.sync.aligned.all` | `forks: ` `amdgcn.s.barrier` wrapped in workgroup-scoped release/acquire fences (LDS visibility) | `abstraction: ` `llvm.spv.group.memory.barrier.with.group.sync` + a **post-emit fixup**. LLVM 22 emits `OpControlBarrier` with Vulkan-forbidden **SequentiallyConsistent** semantics (`VUID-StandaloneSpirv-MemorySemantics-10866`); `SpirvBackend::fixupControlBarriers` rewrites each barrier's memory-semantics to `WorkgroupMemory\|AcquireRelease` (0x108) by adding one uint constant + repointing. The module then passes strict `spirv-val` **and** runs on-device (verified: `XpuVulkanEmitTests.workgroupBarrierIsSpecValid` + the on-device reduction). |
| Num-workgroups (grid dim) read | not yet a Cajeta builtin | `native` (`nctaid`) | `native` (dispatch packet) | `native` · `llvm.spv.num.workgroups` |

**Reading:** every coordinate *read* is `native` on Vulkan, and *two* of them
(`workgroupDim`, `globalId`) are native single-intrinsic reads where AMD needed a
structural workaround — on this axis Vulkan is the least divergent of the three.
The one wrinkle is the **barrier**: LLVM 22 emits Vulkan-forbidden semantics, so
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
| `Buffer<T>` arg as raw device pointer | `addrspace(1) T*` function param | `native` · pointer in `kernelParams[i]` | `native` · pointer in `kernelParams[i]` | `abstraction: ` **descriptor-set SSBO** — each `Buffer<T>` is an `OpVariable StorageBuffer` (set 0, binding = arg index) accessed via `llvm.spv.resource.handlefrombinding` → `getpointer`. (BDA was the intended path but is **not possible** — see the row below.) |
| Scalar arg by value | scalar function param | `native` | `native` | `abstraction: ` carried as a **single-element SSBO** at its own binding (first measured cut). A push-constant block (`llvm.spv.pushconstant.getpointer`) is the natural refinement. |
| Why not Buffer Device Address | — | — | — | `not possible: ` LLVM 22's SPIR-V backend exposes **no PhysicalStorageBuffer/BDA intrinsic** — the `PhysicalStorageBuffer64EXT` strings are capability enum names only. So raw-pointer kernel args can't be reconstituted from IR; descriptor sets are the only model. |
| Where the fork lands on the seam | param-materialization + buffer-access hooks (default preserves NV/AMD) | default | default | `forks: ` **both** the kernel signature (`void main()`, no params; `createKernel` + `materializeParam` hooks) **and** the body's buffer element access (`bufferElementPtr` → `getpointer` instead of GEP). Bigger than AMD: the body walk forks, not just the prologue. |

**Reading:** Vulkan's missing raw-pointer ABI is the headline divergence — and
the build proved it's *not* bridgeable by Buffer Device Address (no IR path),
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
| **Block dim** (workgroup size) | launch-config `block` | `native` · free per-launch param | `native` · free per-launch param | ⚑ `abstraction: ` first cut bakes a **fixed `LocalSize` (64)** into the SPIR-V via `hlsl.numthreads` — Vulkan fixes workgroup size at compile time, so the launch must match the baked value (`kVulkanLocalSizeX`). A spec-constant `LocalSizeId` + per-blockDim pipeline cache is the refinement. |
| Module load | name-keyed registration (§6) | `native` · `cuModuleLoadData` | `native` · `hipModuleLoadData` | `forks: ` `vkCreateShaderModule` → pipeline-layout → compute-pipeline (heaviest of the three) |
| Argument binding | marshal `argv` | `native` · `kernelParams` array | `native` · `kernelParams` array | `forks: ` push-constant write (BDA, §3) recorded into a command buffer |
| Dispatch + sync | `Stream.sync()` | `native` · `cuLaunchKernel` + sync | `native` · launch + `hipDeviceSynchronize` | `forks: ` `vkQueueSubmit` + `vkQueueWaitIdle` |
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
| External assembler/linker | — | `ptxas` (PTX → cubin) | `ld.lld -shared` (ELF → hsaco) | `not possible / not needed: ` **none** — LLVM 22 emits the final SPIR-V binary itself. Simplest of the three. |
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
| Dynamic shared `shared T[expr]` | external unsized `[0 x T] addrspace(3)`, sized at launch | `native` · `cuLaunchKernel` `sharedMemBytes` | `native` · `hipModuleLaunchKernel` `sharedMemBytes` (= groupMemBytes) | ⚑ `deferred / not possible as a free per-dispatch byte count: ` Vulkan workgroup arrays are sized at pipeline-creation (spec-constant array length), not per-`vkCmdDispatch` — so dynamic LDS would be a pipeline-cache key, not a launch scalar. **Not implemented this pass** (static shared is the measured proof). |

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
GPU backends, run on-device on AMD + Vulkan; **wave = SIMD lane on the CPU backend
(Inc 5C, 2026-05-31)**.

| Feature | Core | NVIDIA | AMD | Vulkan | CPU (Inc 5C) |
|---------|------|--------|-----|--------|--------------|
| Wave width | `Wave.width()` → i32 | `native` · `read.ptx.sreg.warpsize` (32) | `native` · `amdgcn.wavefrontsize` (32/64) | ⚑ `emit-only: ` `spv.wave.get_lane_count` lowers from IR but LLVM 22's SPIR-V backend **cannot select it**, so `Wave.width()` does not run on-device yet | the host's native SIMD width W (16 AVX-512 / 8 AVX2); folded to a constant in a vectorized kernel |
| Lane id | `Wave.laneId()` → i32 | `native` · `read.ptx.sreg.laneid` | `native` · `amdgcn.mbcnt.{lo,hi}` | `native` · `spv.subgroup_local_invocation_id` (validated) | `tid.x % W` |
| Shuffle / readlane | `Wave.shuffleSync(v, lane)` | `native` · `nvvm.shfl.sync.idx.i32` | `native` · `amdgcn.readlane` | `native` · `spv.wave.readlane` (→ `OpGroupNonUniformShuffle`) | VFABI variant · per-lane gather `val[src[i]]` |
| Ballot | `Wave.ballotSync(pred)` → i64 | `native` · `nvvm.vote.ballot.sync` (i32→i64) | `native` · `amdgcn.ballot.i32` (i32→i64) | `native` · `spv.wave.ballot` (`<4 x i32>`, low 64 → i64) | VFABI variant · `bitcast <W x i1> → iW` |
| Reduce (sum) | `Wave.reduceSum(v)` → i32 | `native` · `nvvm.redux.sync.add` (sm_80+) | `native` · `amdgcn.wave.reduce.add` | `native` · `spv.wave.reduce.sum` (→ `OpGroupNonUniformIAdd`) | VFABI variant · `broadcast(vector.reduce.add)`; masked for divergence |

**Reading:** wave ops are native on all three. **The reduce probe overturned its own
hypothesis.** The guess (recorded here in the prior pass) was that reduce would
*invert* comprehensiveness — one native intrinsic on Vulkan vs. a shuffle/DPP
butterfly sequence on NV/AMD. The build showed the opposite: **all three expose a
single hardware wave-reduce intrinsic in LLVM 22**, so reduce maps as cleanly as
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
`XpuWaveDeviceTests` (shuffle/ballot/reduce on-device on AMD + Vulkan; the reduce
check is width-agnostic — sum of 1s over a full wave == wave width ∈ {32, 64}).

---

## 9. Atomics & memory ordering

| Feature | Core | NVIDIA | AMD | Vulkan |
|---------|------|--------|-----|--------|
| Atomic RMW on global / shared | LLVM `atomicrmw` + scope | `native` | `native` | `native` · SPIR-V atomic ops with explicit Scope + Memory-Semantics (by construction; not exercised on-device this pass) |
| Scoped fences | `Fence` (type declared, not yet functional) | `native` | `native` (workgroup release/acquire already used for the barrier) | `native` · `OpMemoryBarrier` with scope |

---

## 10. Deferred / not-yet capabilities (no backend has these)

Tracked here so the matrix is honest about the frontier, not just the floor.
These are backend-neutral gaps from `cajeta-xpu.md`, unaffected by the Vulkan column.

| Capability | Status |
|------------|--------|
| Real `launch(stream, grid:, block:)(args)` postfix grammar | deferred |
| Launch-site resolution into `XpuMirLaunchSite` | placeholder |
| MIR body-op walker (`Op_ThreadId`, `Op_BarrierWorkgroup`, …) | empty |
| `@Device` user-defined helper calls | deferred (XPU-N01) |
| for-each parallel loops | deferred (XPU-N01) |
| Labeled `break` / `continue` | deferred (XPU-N01) |
| 2D/3D launch (currently 1-D indexing) | deferred |
| Multi-arch bundling (fatbin) | deferred — single arch per emit |
| Texture / Sampler types | deferred |
| `@PushConstant` (Vulkan-only surface) | deferred — note: BDA already *uses* a push-constant block internally (§3), so the plumbing arrives early on Vulkan |
| POD structs as kernel args without explicit `implements KernelArg` | deferred |

---

## 11. The abstraction-layer ledger (the user's core question)

Where a platform lacks a native primitive — can Cajeta provide it, and if not, why?

**Provided cleanly (abstraction layer bridges the gap):**

- **Vulkan · raw-pointer buffer args** → **descriptor-set SSBOs** (`resource.handlefrombinding` + `getpointer`); scalars as single-element SSBOs. Forks signature + body buffer access; the rest of the body stays shared. *(§3)*
- **Vulkan · workgroup barrier** → `group.memory.barrier.with.group.sync` + a one-instruction post-emit fixup (`SpirvBackend::fixupControlBarriers`) that corrects LLVM 22's Vulkan-forbidden SequentiallyConsistent semantics to `WorkgroupMemory|AcquireRelease`. Now passes strict `spirv-val` and runs on-device. *(§1)*
- **Vulkan · block dim** → fixed compile-time `LocalSize` (first cut); spec-constant `LocalSizeId` is the refinement. *(§4)*
- **AMD · workgroup dim read** → dispatch-packet load (no `ntid` intrinsic, but the value is recoverable). *(§1)*

**Not cleanly possible (and why):**

- **Vulkan · Buffer Device Address (raw pointers)** → LLVM 22's SPIR-V backend has **no PhysicalStorageBuffer/BDA intrinsic** from IR. This forced the descriptor-set model above — the central build-discovered finding. *(§3)*
- **Vulkan · generic/flat address space** → SPIR-V has no Generic storage class outside the OpenCL `Kernel` capability. **Benign**: the lowerer tracks every pointer's address space explicitly and never relies on flat generic deref. *(§2)*
- **Vulkan · dynamic shared as a *free per-dispatch* byte count** → workgroup arrays are sized at pipeline creation, not per-`vkCmdDispatch`. Deferred this pass. *(§7)*

**Needed nowhere (already shared across all three):** the entire kernel-body AST
walk, the coordinate leaf reads, `addrspace(3)` shared memory (→ Workgroup on
SPIR-V, a non-fork), the name-keyed registration symbol, and the whole frontend.

---

## Status — Vulkan column now *measured*

The Vulkan backend was built 2026-05-30 by threading SPIR-V through the same
`LoweringTarget` seam AMD uses (full log: [`cajeta-vulkan.md`](cajeta-vulkan.md)).
All increments landed; the on-device tests run on the Strix Halo APU via RADV.

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

*Generated 2026-05-30; Vulkan column updated after the on-device build.
NVIDIA/AMD/Vulkan: all three are measured on live backends (NVIDIA on-device
gated on CUDA hardware). See [`cajeta-xpu.md`](cajeta-xpu.md) §"The NVIDIA∩AMD
overlap reckoning" and [`cajeta-amd.md`](cajeta-amd.md).*
