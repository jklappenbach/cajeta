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

---

## Provenance & how to read this

The three platform columns do **not** have equal evidentiary weight yet, and the
matrix says so explicitly rather than pretending otherwise:

| Column | Status | Basis |
|--------|--------|-------|
| **NVIDIA** | **Measured** — live backend, on-device | NVPTX → cubin, runs via CUDA driver. Emit + on-device tests green. |
| **AMD** | **Measured** — live backend, on-device | AMDGPU → hsaco, runs on gfx1151 (Strix Halo) via HIP. Emit + on-device tests green. |
| **Vulkan** | **Verified-feasible** — *not yet built / not yet on-device* | LLVM 22 SPIR-V emission + the SPIR-V leaf-intrinsic mapping confirmed by read-only probe on 2026-05-30. The backend has **not** been threaded through cajeta and has **not** run on a device. Cells are projections *by construction*, not measurements. |

**Cell vocabulary**

- `native` — the platform has a direct hardware/IR primitive; no abstraction layer needed.
- `abstraction: …` — no direct primitive, but Cajeta can synthesize the capability cleanly; the `…` says how.
- `forks: …` — supported, but the implementation diverges from the others in a way that lands on the seam; the `…` says where.
- `not possible: …` — cannot be provided on this platform as specified; the `…` says why.
- `—` — not applicable / not yet a capability on any platform.

**⚑** marks a Vulkan cell that the build must still confirm empirically (these
are exactly the increments in the
[plan](#status--what-remains-to-make-the-vulkan-column-measured)).

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
| Workgroup barrier | one `workgroupBarrier()` seam method | `native` · `nvvm.barrier.cta.sync.aligned.all` | `forks: ` `amdgcn.s.barrier` wrapped in workgroup-scoped release/acquire fences (LDS visibility) | `native` · `llvm.spv.group.memory.barrier.with.group.sync` — single intrinsic, ordering folded in |
| Num-workgroups (grid dim) read | not yet a Cajeta builtin | `native` (`nctaid`) | `native` (dispatch packet) | `native` · `llvm.spv.num.workgroups` |

**Reading:** every coordinate read is `native` on Vulkan, and *two* of them
(`workgroupDim`, `globalId`) are native single-intrinsic reads where AMD needed a
structural workaround. The leaf-read variance surface holds for a third backend —
in fact Vulkan is the *least* divergent of the three on this axis.

---

## 2. Address spaces & memory model

Numeric address spaces from `src/cajeta/xpu/core/AddressSpace.h`. NVIDIA and AMD
share the LLVM numbering; SPIR-V uses storage classes (the integers below are the
Khronos storage-class enum values, not LLVM address spaces).

| Address space | Core (Cajeta `AddressSpace`) | NVIDIA `addrspace` | AMD `addrspace` | Vulkan storage class |
|---------------|------------------------------|--------------------|-----------------|----------------------|
| Global (device buffers) | `Global` | 1 | 1 | `native` · StorageBuffer (12) — or **PhysicalStorageBuffer** under BDA, see §3 |
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

## 3. Kernel-argument ABI ⚑ — the one genuinely new Vulkan seam coordinate

NVIDIA and AMD both pass kernel args as a flat `kernelParams` array: buffers as
raw `addrspace(1)` device pointers, scalars by value. Vulkan has **no
raw-pointer kernel-argument ABI**. This is the architectural fork the Vulkan
bring-up turns on.

| Feature | Core | NVIDIA | AMD | Vulkan |
|---------|------|--------|-----|--------|
| `Buffer<T>` arg as raw device pointer | `addrspace(1) T*` function param | `native` · pointer in `kernelParams[i]` | `native` · pointer in `kernelParams[i]` | `abstraction: ` **Buffer Device Address** — `VK_KHR_buffer_device_address` (core VK 1.2) + SPIR-V `PhysicalStorageBuffer`. Buffer → 64-bit `VkDeviceAddress` in a push-constant block, reconstituted as a `PhysicalStorageBuffer` pointer in the entry block. **Kernel body GEPs are unchanged** — same shape as `addrspace(1)`. |
| Scalar arg by value | scalar function param | `native` | `native` | `abstraction: ` packed into the same push-constant block |
| Where the fork lands on the seam | param-**materialization** hook (default = direct `addrspace(1)` params, preserves NV/AMD) | default | default | `forks: ` the new hook emits the push-constant block + pointer reconstitution. **Only the kernel *signature/prologue* forks — not the body walk.** |
| Fallback if `bufferDeviceAddress` unsupported | — | — | — | `abstraction (fallback): ` descriptor-set / SSBO — each buffer an `OpVariable StorageBuffer` decorated DescriptorSet/Binding. Heavier (forks the signature *and* the launch path); only taken if a device lacks BDA. |

**Reading:** Vulkan's missing raw-pointer ABI is the headline divergence, and it
is fully bridgeable by an abstraction (BDA) that keeps the ≈90% kernel-body core
shared. The cost is exactly one new seam coordinate — parameter materialization —
versus the leaf-read methods that already existed. The descriptor-set fallback is
the "if we can't do BDA, here's why it's heavier" branch.

---

## 4. Launch / dispatch / runtime

| Feature | Core | NVIDIA | AMD | Vulkan |
|---------|------|--------|-----|--------|
| Grid dim (number of blocks) | launch-config `grid` | `native` · `cuLaunchKernel` gridX | `native` · `hipModuleLaunchKernel` gridX | `native` · `vkCmdDispatch(gridX,1,1)` |
| **Block dim** (workgroup size) | launch-config `block` | `native` · free per-launch param | `native` · free per-launch param | `abstraction: ` `LocalSizeId` **specialization constant resolved at pipeline-creation** — Vulkan does not take block dim as a free per-dispatch value. ⚑ Requires **per-(kernel, blockDim) pipeline caching**. |
| Module load | name-keyed registration (§6) | `native` · `cuModuleLoadData` | `native` · `hipModuleLoadData` | `forks: ` `vkCreateShaderModule` → pipeline-layout → compute-pipeline (heaviest of the three) |
| Argument binding | marshal `argv` | `native` · `kernelParams` array | `native` · `kernelParams` array | `forks: ` push-constant write (BDA, §3) recorded into a command buffer |
| Dispatch + sync | `Stream.sync()` | `native` · `cuLaunchKernel` + sync | `native` · launch + `hipDeviceSynchronize` | `forks: ` `vkQueueSubmit` + `vkQueueWaitIdle` |
| Driver acquisition | dlopen vendor lib | `native` · `libcuda.so.1` | `native` · `libamdhip64.so` (+ pinned `libhsa`) | `abstraction: ` dlopen `libvulkan.so.1`; pick a physical device advertising `bufferDeviceAddress` (radeon ICD reaches the Strix Halo APU) |

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
| Static shared `Shared<T> t = shared T[N]` | internal `[N x T] addrspace(3)` global | `native` | `native` | `native` ⚑ · Workgroup-storage-class array, fixed N |
| Dynamic shared `shared T[expr]` | external unsized `[0 x T] addrspace(3)`, sized at launch | `native` · `cuLaunchKernel` `sharedMemBytes` | `native` · `hipModuleLaunchKernel` `sharedMemBytes` (= groupMemBytes) | `abstraction: ` **spec-constant array length resolved at pipeline creation**, pipeline-cache keyed on shared size. ⚑ `not possible (as a free per-dispatch byte count): ` Vulkan workgroup arrays are sized at pipeline-creation, not per-`vkCmdDispatch` — so dynamic LDS is a pipeline-cache key, not a launch scalar. |

**Reading:** static shared is a clean `native` three-way. Dynamic shared is the
second place (after block dim, §4) where a value Cajeta treats as launch-time on
NV/AMD becomes pipeline-creation-time on Vulkan — bridgeable by a spec constant +
pipeline cache, but it cannot be a free per-dispatch scalar, and the matrix says
why.

---

## 8. Wave / subgroup ops (the later `@Wave` feature — not yet built on any backend)

Deferred on NVIDIA too; listed here because wave ops are the archetypal
"hardware on one vendor, software-equivalent on another" feature and the cleanest
*future* stress test of the seam.

| Feature | Core | NVIDIA | AMD | Vulkan |
|---------|------|--------|-----|--------|
| Wave width | `@Wave(width: N)` | 32 (warp) | 64 (wavefront) — *trait-gated* | implementation-defined; query `VkPhysicalDeviceSubgroupProperties` |
| Shuffle / permute | — | `native` · `llvm.nvvm.shfl.sync.*` (hw) | `native` · `llvm.amdgcn.ds.bpermute` / `ds.swizzle` (hw) | `native` · `VK_KHR_shader_subgroup` (`OpGroupNonUniformShuffle`, hw) |
| Ballot / reduce / scan | — | `native` (hw) | `native` (hw) | `native` · subgroup arithmetic/ballot ops (hw) |

**Reading:** wave ops are native on all three at the hardware level — the
divergence is *width* (32 vs 64 vs queried), which is exactly why the design
guardrail is to **trait-gate** wave width rather than assume 32. Build them only
once the seam exists (it does); they are the first variance-*shaped* feature that
will exercise it.

---

## 9. Atomics & memory ordering

| Feature | Core | NVIDIA | AMD | Vulkan |
|---------|------|--------|-----|--------|
| Atomic RMW on global / shared | LLVM `atomicrmw` + scope | `native` | `native` | `native` ⚑ · SPIR-V atomic ops with explicit Scope + Memory-Semantics |
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

- **Vulkan · raw-pointer buffer args** → Buffer Device Address (`PhysicalStorageBuffer` + push-constant addresses). Keeps the kernel body identical to NV/AMD. *(§3)*
- **Vulkan · runtime block dim** → `LocalSizeId` specialization constant + per-blockDim pipeline cache. *(§4)*
- **Vulkan · dynamic shared memory** → spec-constant array length + pipeline cache keyed on shared size. *(§7)*
- **AMD · workgroup dim read** → dispatch-packet load (no `ntid` intrinsic, but the value is recoverable). *(§1)*

**Not cleanly possible (and why):**

- **Vulkan · generic/flat address space** → SPIR-V has no Generic storage class outside the OpenCL `Kernel` capability. **Benign**: Cajeta's lowerer tracks every pointer's address space explicitly and never relies on flat generic deref, so nothing is actually lost. *(§2)*
- **Vulkan · dynamic shared as a *free per-dispatch* byte count** → workgroup arrays are sized at pipeline creation, not per-`vkCmdDispatch`. We provide *dynamic* shared, but it becomes a pipeline-cache key rather than a launch scalar — the semantics are slightly narrower than CUDA's `sharedMemBytes`. *(§7)*

**Needed nowhere (already universal):** the entire kernel-body AST walk, the
coordinate/barrier leaf reads, address-space numbering for buffers/shared, the
name-keyed registration symbol, and the whole frontend. This is the measured
NVIDIA∩AMD core (≈90%), and every signal in this analysis says it extends to
Vulkan as a third column.

---

## Status — what remains to make the Vulkan column *measured*

The Vulkan column is **verified-feasible, not measured**. To promote each ⚑ cell
from projection to measurement, the Vulkan backend must be threaded through the
seam exactly as AMD was (full plan in the approved implementation plan; mirrors
`cajeta-amd.md` increments 0–5):

| Increment | Promotes | Tier |
|-----------|----------|------|
| 0 | `Backend::Spirv` enum + `--xpu-backend=vulkan` CLI | refactor |
| 1 | SPIR-V emission (`SpirvBackend`) — §5 | Tier-0 (GPU-free) |
| 2 | `SpirvTarget` leaf reads (§1) + the BDA param hook (§3) | Tier-0 |
| 3 | `VulkanRegistration` + AOT `--xpu-emit=spv` (§6) | Tier-0 |
| 4 | `VulkanDriver` + on-device SAXPY (§3,§4) | Tier-1 (on-device) |
| 5 | static + dynamic workgroup-shared reductions (§7) | Tier-1 |

Until those land, treat every Vulkan cell as *by construction*. The NVIDIA and
AMD columns are measured and on-device today.

---

*Generated 2026-05-30. NVIDIA/AMD: measured (two live on-device backends).
Vulkan: verified-feasible via LLVM 22 SPIR-V emission + intrinsic mapping;
backend not yet built. See [`cajeta-xpu.md`](cajeta-xpu.md) §"The NVIDIA∩AMD
overlap reckoning" and [`cajeta-amd.md`](cajeta-amd.md).*
