# Cajeta XPU — Vulkan/SPIR-V third backend (bring-up log)

> **Status: DONE (2026-05-30).** Vulkan compute is a live XPU backend, built by
> threading SPIR-V through the same `LoweringTarget` seam AMD uses. SAXPY and a
> static workgroup-shared tree reduction run on the Strix Halo APU via the
> radeon (RADV) ICD. Companion to [`cajeta-amd.md`](cajeta-amd.md) (second
> backend) and the four-column [`cajeta-xpu-matrix.md`](cajeta-xpu-matrix.md).

## 0. Strategy (same discipline as AMD)

Per the locked strategy (cajeta-amd.md §0): **the build is the evaluation.** The
seam was *already* extracted by threading AMD through it; Vulkan is the third
implementation that tests whether that seam generalizes. The rule held: don't
design Vulkan abstractions in a vacuum — thread SPIR-V through concretely and let
the duplication show where the seam must grow. It grew in exactly one new place
(the kernel-argument model), which is the measured deliverable.

NVIDIA stays the most comprehensive column; AMD and Vulkan get equivalents.
Vulkan, it turned out, needs the most equivalents of the three.

## 1. The make-or-break probe (before any code)

A read-only probe of LLVM 22's in-tree SPIR-V backend settled the feasibility
question at the cheapest point:

- **Vulkan compute SPIR-V emits cleanly.** `void main()` + `hlsl.shader="compute"`
  + `hlsl.numthreads` → `OpCapability Shader` / `OpEntryPoint GLCompute` /
  `OpExecutionMode LocalSize`. The coordinate intrinsics (`llvm.spv.thread.id`
  etc.) lower to the right BuiltIns.
- **The triple is `spirv-unknown-vulkan1.3-compute`** (Logical GLSL450 memory
  model) — the Vulkan flavor, *not* the OpenCL-Kernel flavor.
- **Buffer Device Address is unavailable.** The Vulkan SPIR-V path is HLSL-
  frontend-shaped (the entry needs `hlsl.shader`); there is **no
  PhysicalStorageBuffer/BDA intrinsic** from IR. The `PhysicalStorageBuffer64EXT`
  strings in `libLLVM` are capability enum names only. So raw-pointer kernel args
  (the CUDA/HIP model, and the OpenCL-Kernel SPIR-V flavor) are not reachable for
  Vulkan — the buffer model is **descriptor-set storage buffers**.

This overturned the originally-approved BDA plan and was surfaced as a design
decision; the user chose to build the real Vulkan backend on the descriptor-set
path (the bigger fork — and the point of the measurement).

The exact descriptor-set IR pattern was then cracked with a standalone
LLVM-API probe (auto-mangling the target-extension-type intrinsics) and verified
with `spirv-val --target-env vulkan1.3` before any backend code was written.

## 2. The variance surface — what Vulkan added to the seam

AMD forked only the `LoweringTarget` coordinate leaf reads. Vulkan forks those
**and** the kernel signature **and** the body's buffer access — so three new hooks
joined `LoweringTarget`, all with NVPTX/AMDGPU-preserving defaults:

| Decision | NVPTX / AMDGPU (default) | SPIR-V (Vulkan override) |
|----------|--------------------------|--------------------------|
| `createKernel` | function takes `ptr addrspace(1)` per buffer + scalars, `decorateKernel` | `void main()`, **no params** + `hlsl.shader="compute"` / `hlsl.numthreads` |
| `materializeParam` | `fn->getArg(idx)` | `resource.handlefrombinding(set 0, binding idx)` — buffer → handle; scalar → single-element SSBO + load |
| `bufferElementPtr` | addrspace-preserving GEP | `resource.getpointer(handle, i32 idx)` for descriptor handles; GEP for shared-mem globals |
| `allocaAddressSpace` | 0 / 5 | 0 (Function storage) |
| `threadId` / `workgroupId` / `workgroupDim` | nvvm / amdgcn intrinsics | `llvm.spv.thread.id.in.group` / `group.id` / `workgroup.size` |
| `globalId` | computed default | `llvm.spv.thread.id` (native GlobalInvocationId) |
| `workgroupBarrier` | barrier.cta.sync / s.barrier+fences | `llvm.spv.group.memory.barrier.with.group.sync` (caveat §4) |
| `decorateKernel` | ptx CC + nvvm.annotations / amdgpu CC | no-op (markers set in `createKernel`) |
| `waveWidth` / `waveShuffle` / `waveBallot` / `waveReduceSum` | nvvm / amdgcn wave intrinsics | `llvm.spv.wave.{get_lane_count, readlane, ballot, reduce.sum}` (caveat §4) |

**What stayed shared:** the entire kernel-body AST walk, control flow, the
operator set, the mutable scalar-slot model, and `addrspace(3)` shared memory
(→ Workgroup storage class — a non-fork). The shared lowerer's one buffer-element
GEP site routes through `bufferElementPtr`, so the body is untouched apart from
that hook.

## 3. Increments (mirrors cajeta-amd.md 0–5)

| Inc | Landed | Tier | Test |
|-----|--------|------|------|
| 0 | `Backend::Spirv` + `XpuBackend::Vulkan` + CLI (`--xpu-backend=vulkan`, `--xpu-emit=spv\|spvasm`, `vulkan1.3` default) + dispatch seam | refactor | NV/AMD stay green |
| 1 | `SpirvBackend` (TargetMachine, `emitSpirvText`, `emitSpirv` — no external assembler) | Tier-0 | `XpuVulkanEmitTests` |
| 2 | `SpirvTarget` over the shared lowerer + the signature/buffer-access fork | Tier-0 | `XpuVulkanEmitTests` (+ all NV/AMD emit tests green) |
| 3 | `VulkanRegistration` (SPIR-V bytes + global_ctors → neutral `__cajeta_xpu_register_module`) + AOT emit | Tier-0 | `XpuVulkanAotCliTests` |
| 4 | `VulkanDriver` (dlopen libvulkan; descriptor sets + compute pipeline + `vkCmdDispatch`) + on-device SAXPY | Tier-1 | `XpuSaxpyVulkanDeviceTests` (2²⁰ elems, `y[i]=2i+1`) |
| 5 | static workgroup-shared tree reduction on-device | Tier-1 | `XpuSharedVulkanDeviceTests` (sum = 64·63/2) |

Tier-0 (0–3) is GPU-free. Tier-1 (4–5) runs on the Strix Halo APU via RADV;
skips cleanly when no Vulkan device is present.

## 4. Findings & limitations

- **Workgroup barrier — found broken, fixed.** LLVM 22's only barrier intrinsic
  emits `OpControlBarrier` with **SequentiallyConsistent** memory semantics, which
  the Vulkan spec forbids (`VUID-StandaloneSpirv-MemorySemantics-10866`), and
  there is no IR-level way to request the valid `WorkgroupMemory | AcquireRelease`.
  RADV ran it anyway, but `spirv-val` rejected it and a stricter driver might too.
  **Fixed** by `SpirvBackend::fixupControlBarriers`, a one-instruction post-emit
  pass: it adds a single `OpConstant %uint 0x108` (WorkgroupMemory|AcquireRelease)
  and repoints every `OpControlBarrier` semantics operand to it (rather than
  mutating the existing constant, which a user literal could share). Emitted
  modules now pass strict `spirv-val` and still run on-device
  (`XpuVulkanEmitTests.workgroupBarrierIsSpecValid` + the on-device reduction).
- **Block dim is baked at compile time.** Vulkan fixes the workgroup size in the
  SPIR-V (`hlsl.numthreads` → `OpExecutionMode LocalSize`). The first cut bakes a
  fixed `kVulkanLocalSizeX = 64`; the launch must match it. Spec-constant
  `LocalSizeId` (per-launch block dim) is the refinement.
- **Scalars cross as single-element SSBOs** (their own binding) rather than push
  constants — simplest correct cut; `llvm.spv.pushconstant.getpointer` is the
  refinement.
- **Dynamic shared memory deferred.** Vulkan sizes workgroup arrays at pipeline
  creation (spec-constant length), not per-dispatch — so dynamic LDS would be a
  pipeline-cache key, not a launch scalar. Static shared is the measured proof.
- **`@Wave` ops — `reduce` overturned the matrix's guess.** `Wave.shuffleSync` /
  `ballotSync` / `reduceSum` lower to `spv.wave.readlane` / `spv.wave.ballot`
  (`<4 x i32>` → i64) / `spv.wave.reduce.sum` (→ `OpGroupNonUniformIAdd`). The matrix
  had predicted reduce would be richer on Vulkan than on NV/AMD; in fact all three
  expose one native wave-reduce intrinsic, so it maps 1:1 like shuffle/ballot.
  Emit-verified + `spirv-val`-clean, and `reduceSum` runs on-device on RADV
  (`XpuWaveDeviceTests.vulkanReduceSumRunsOnDevice`, width-agnostic: sum of 1s over a
  full subgroup == subgroup size ∈ {32, 64}).
- **`Wave.width()` is emit-only on Vulkan.** `spv.wave.get_lane_count` lowers from IR
  but LLVM 22's SPIR-V backend cannot *select* it (`Intrinsic selection not
  implemented`) — so `Wave.width()` crashes on-device and isn't exercised in the
  device tests (which derive width-agnostic invariants instead). A backend gap to
  revisit when LLVM gains the lowering; not an abstraction-layer problem.

## 5. Files

New `src/cajeta/xpu/vulkan/`: `SpirvBackend.{h,cpp}`,
`SpirvKernelLowering.{h,cpp}`, `VulkanRegistration.{h,cpp}`,
`VulkanDriver.{h,cpp}`. Seam growth: three new `LoweringTarget` hooks
(`createKernel` / `materializeParam` / `bufferElementPtr`) with NV/AMD-preserving
defaults in `lowering/{LoweringTarget.h,KernelLowering.cpp}`. Wiring:
`XpuTarget.{h,cpp}`, `compile/Compiler.{h,cpp}`, `main.cpp`. Tests under
`test/xpu/XpuVulkan*` and `XpuSaxpyVulkanDeviceTests` / `XpuSharedVulkanDeviceTests`.

`VulkanDriver` includes `<vulkan/vulkan.h>` only behind `__has_include` (no
link-time libvulkan dependency — entry points resolve via dlopen +
`vkGetInstanceProcAddr`/`vkGetDeviceProcAddr`); a box without the Vulkan SDK
still builds, with `available()` returning false.

## Runtime port (CajetaXPU Increment 4.3 — `cajeta-cpu.md`)

The C++ `VulkanDriver` is **compiler/test-only**; compiled Cajeta programs launch
through the C runtime (`runtime/native/cajeta_runtime.c`), which is the sole launch
path. So the descriptor-set compute launch + the host-coherent buffer table were
**ported into C** there (same `__has_include`/dlopen shape), behind the runtime
backend dispatcher. The one ABI fork the uniform launch path exposed: Vulkan's
compute entry takes no params, only descriptor bindings, so the `kernelParams` argv
(buffer handles + raw scalars) can't be passed through — the rung registers
**per-kernel parameter metadata** (`__cajeta_xpu_register_kernel_params`) and, at
launch, binds buffer args to their storage buffers and wraps scalar args in
**transient single-element SSBOs**. The block dim stays compile-time-fixed
(`kVulkanLocalSizeX = 64`); the launch dispatches `gridX` work-groups.

Two build findings from the AOT path: a Vulkan kernel must get a **fresh SPIR-V
`TargetMachine` per kernel** — reusing one corrupts LLVM 22's `SPIRVGlobalRegistry`
and crashes on the second kernel (`SPIRVEmitIntrinsics::buildAssignPtr`); and the
AOT `--emit=obj` path needed `TargetOptions::UseInitArray = true` so the
registration constructors actually run (modern glibc runs `.init_array`, not the
legacy `.ctors` LLVM emitted by default). Both are fixed. Runnable end-to-end via
`samples/Tour/xpu/run-xpu.sh vulkan,cpu`.
