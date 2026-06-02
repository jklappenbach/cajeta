# CajetaXPU — forward plan

The remaining XPU frontier after the CPU backend reached feature-completeness
(Increments 1–10: lowering/codegen/driver, runtime dispatcher, threading +
wave=SIMD, and barrier fission with every scope cut lifted). These items are
backend-neutral capability gaps and Vulkan refinements drawn from the deferred
list (§10) and the ⚑ caveats in [`cajeta-xpu-matrix.md`](cajeta-xpu-matrix.md).

Companion docs: [`cajeta-xpu.md`](cajeta-xpu.md) (NVIDIA∩AMD reckoning),
[`cajeta-amd.md`](cajeta-amd.md), [`cajeta-vulkan.md`](cajeta-vulkan.md),
[`cajeta-cpu.md`](cajeta-cpu.md).

**Working agreement:** one item at a time, each its own increment with tests +
docs + a commit checkpoint, never a miscompile (unsupported shapes degrade to a
clean diagnostic + fallback). Status is kept current in the table below.

### ✅ Pre-flight CLEARED (2026-05-31) — Vulkan is green on `cajeta-xpu`

All 12 `*Vulkan*`/`*Spirv*` tests pass on this branch, including
`XpuVulkanEmitTests.workgroupBarrierIsSpecValid` (the barrier-fixup test the failed
task was about) **and the on-device Vulkan tests** (Strix Halo APU via RADV). The
exit-144 background task was a `cajeta-two` worktree artifact, not a regression here.
Vulkan items (#3/#5) are safe to build on.

<details><summary>original flag</summary>

A background task — *"Verify fixup semantics value + run all Vulkan Tier-0
tests"* — failed with **exit 144** during this session. It is in the Vulkan
barrier-fixup area (`SpirvBackend::fixupControlBarriers`, the
`WorkgroupMemory|AcquireRelease` = 0x108 semantics value). It may be a
`cajeta-two` artifact rather than a real regression, but **before starting Vulkan
items #3/#5, re-run the Vulkan Tier-0 suite on this branch and confirm green** (or
fix the regression first). Don't build new Vulkan capability on a red base.

</details>

## Status

| # | Item | Class | Effort | Status |
|---|------|-------|--------|--------|
| 1 | **2D/3D launch** | capability | large | ✅ done (ABI + GPU 3-D + CPU 3-D grid/block/fission) |
| 2 | **`@Device` helper calls** | capability | medium-large | ✅ done (scalar + Buffer params, same class, helper-chains; verified on AMD & Vulkan) |
| 3 | **Vulkan block dim — spec-constant workgroup size** | vulkan | medium | ✅ done (verified on-device, block 128) |
| 4 | **Multi-arch bundling (fatbin)** | deployment | medium | ◐ AMD done + verified on-device; NVIDIA untestable here (no ptxas/fatbinary) |
| 5 | **Vulkan dynamic shared memory** | vulkan | medium | ✅ done (verified on-device) |
| 6 | **`for-each` parallel loops** | capability | medium | ✅ done (grid-stride; NVPTX/AMD/SPIR-V + frontend verified on AMD & Vulkan; CPU coord-ABI extended, verified) |
| 7 | **POD structs as kernel args** | capability | small-medium | ✅ done (by-value, all 4 backends; verified on AMD & Vulkan) |
| 8 | **Texture / Sampler types** | capability | large | ✅ done (2-D sampled, bilinear; CPU + Vulkan + AMD verified on-device, NVIDIA emit-only; needs LLVM 23) |
| 9 | **`Wave.width()` on-device (Vulkan)** | vulkan | ✅ done | routed to selectable `spv.subgroup.size` (SubgroupSize builtin); runs on RADV |

Ordering rationale: foundational/highest-leverage first (2D/3D launch unblocks
whole workload classes and other items index against it), then kernel modularity
(`@Device`), then the self-contained Vulkan/deployment refinements, then the
specialized types. #9 is externally blocked on the LLVM SPIR-V backend and is
tracked, not scheduled.

---

## 1. 2D/3D launch — *the big one*

**Today:** everything is **1-D** — `tid.x` only, `ntid.y/z == 1`, `tid.y/z == 0`.
Launch sites, the coordinate reads, and the CPU fission/wrapper were all built
1-D on purpose.

**Goal:** `grid: [gx, gy, gz]` / `block: [bx, by, bz]` with working `Thread.y/z`,
`Workgroup.y/z`, `globalIdY/Z`. Real workloads (image filters, matmul, 2-D/3-D
stencils) need this.

**Touch-points (to map empirically before coding):**
- Launch ABI: `__cajeta_xpu_launch` carries `gridX/blockX` only — extend to y/z.
  The per-backend launchers (`cajeta_xpu_launch_{cuda,hip,vulkan,cpu}`) and the
  coord vector.
- Coordinate reads: already per-dim in the `LoweringTarget` seam (`threadId(dim)`,
  `workgroupId(dim)`, `workgroupDim(dim)`) — the GPU intrinsics are 3-D-native;
  the constraint is the launch ABI + CPU.
- **CPU**: the per-block wrapper loops `tid.x` over `[0, ntid.x)`. 2-D/3-D means a
  nested work-item loop over `(tid.z, tid.y, tid.x)`; barrier fission's region
  loops, context-array indexing (`tid.x` placeholder), and wave/laneId must
  generalize to a linearized work-item index.
- Vulkan: block dim is baked (see #3) — 3-D `LocalSize` interacts with that.

**Mapped wiring (1-D today):**
- Launch site (`CallExpression.cpp`): `lowerDim` reads only `arr->getElements()[0]`
  → X only; emits `__cajeta_xpu_launch(name, gridX, blockX, sharedBytes, argv)`.
- Runtime: `cuLaunchKernel(fn, gridX,1,1, blockX,1,1, …)` and
  `hipModuleLaunchKernel(…, gridX,1,1, blockX,1,1, …)` — **3-D native, y/z hardcoded
  1**. `vkCmdDispatch(cmd, groups,1,1)` — 3-D groups possible, **block dim baked**
  (#3). CPU: `coord[10]` = tid.xyz, ctaid.xyz, ntid.xyz, dynShared; wrapper loops
  `tid.x ∈ [0,ntid.x)`; `run_slice` iterates `ctaid.x` over `[cxStart,cxEnd)`.

**Stages:**
1. **Launch ABI + site.** `lowerDim(arr, idx)` extracts dims 0/1/2 (default 1);
   `__cajeta_xpu_launch(name, gx,gy,gz, bx,by,bz, sharedBytes, argv)`; thread to all
   four backend launchers. CUDA/HIP: pass the real y/z (native). Vulkan: 3-D groups,
   block stays baked (1-D) until #3. CPU: see stages 3–4.
2. **GPU emit/dispatch.** NV/AMD 3-D grid+block; Vulkan 3-D grid. Emit tests (no
   local GPU → device tests skip; emit asserts the dims reach the launch call).
3. **CPU non-barrier 3-D.** `run_slice` iterates the 3-D block grid (ctaid.xyz);
   the wrapper nests `for tid.z,tid.y,tid.x`; `ntid.xyz` from coord. **Runnable**
   proof: a 2-D kernel (e.g. transpose / 2-D index write) on CPU.
4. **CPU barrier fission 3-D.** Generalize the `tid.x` placeholder + context-array
   indexing to a **linearized** work-item index `tid.z*ntid.y*ntid.x +
   tid.y*ntid.x + tid.x`; wave/laneId over the linear id. The region work-item
   loops iterate `[0, ntid.x*ntid.y*ntid.z)`. Test: a 2-D shared+barrier kernel.
5. **Docs + matrix** (drop the "1-D indexing" deferred row).

**Progress:**
- ✅ **Stage 1** — launch ABI is 3-D end to end: `__cajeta_xpu_launch(name, gx,gy,gz,
  bx,by,bz, sharedBytes, argv)`; the site parses `grid:`/`block:` `[x]`/`[x,y]`/`[x,y,z]`
  (missing dims → 1). CUDA/HIP pass full 3-D grid+block (native); Vulkan dispatches a
  3-D grid (groups), block stays baked (→ #3); CPU runs a **multi-dim grid** of 1-D
  blocks (ctaid.xyz via a linearized-block decode). A multi-dim **block** on CPU
  (ntid.y/z > 1) gets a clean diagnostic + no-run (no silent miscompile) until stage 3.
  Test `XpuCpuDispatchTests.multiDimGridOnCpu` (4×3 grid). Full Xpu* green (138/7-skip/0).
- ✅ **Stage 3** — the CPU non-barrier per-block wrapper is a 3-D work-item loop nest
  (tid.z/y/x); the inner tid.x loop stays the vectorizable/wave loop, so 1-D is
  unchanged. A multi-dim BLOCK now runs on CPU. The block guard is now per-kernel:
  barrier (fission) kernels are marked `no3d` at registration (still 1-D), so a
  multi-dim-block launch of them gets a diagnostic, not a miscompile; non-barrier
  kernels run the nest. Test `multiDimBlockOnCpu` (4×3 block).
- ◐ **Stage 4** — CPU barrier fission over a multi-dim block (lifts the `no3d` mark on
  barrier kernels). **The deepest change in the codebase; design fixed, not yet built:**
  - **Do NOT linearize naively.** Iterating one region loop over `[0, ntid.x·y·z)` with
    `tid.x = lin % ntid.x` puts a `urem` on the index *inside* the vectorized region
    loop, which scalarizes the shared-memory accesses (gather/scatter) and regresses
    the Inc-6 reduction + every 1-D barrier/wave kernel from SIMD to per-lane. Rejected.
  - **Design — 3-D nest per region (keeps `tid.x` the contiguous inner IV, no `urem`):**
    - Step 1–2: three placeholders `phX/phY/phZ`; `vmap` tid.x→phX, tid.y→phY, tid.z→phZ
      (not 0). Seed the taint fixpoint from all three.
    - Step 7: context arrays sized `ntid.x·y·z` (compute once in the entry), indexed by a
      **linear** index `tz·(ntidY·ntidX) + ty·ntidX + tx`. The `tz·..+ty·..` base is
      loop-invariant in the inner `tx` loop → hoisted, so the inner GEP is `base + tx`
      (an add, SIMD-friendly).
    - Step 9: wrap each region in a **3-D loop nest** (outer tz, mid ty, inner tx) instead
      of one loop; the inner `tx` latch is the one passed to `forceLoopVectorWidth`
      (wave/SIMD). `RegionJob` carries 3 IVs.
    - Step 10: rewrite phX→region.tx, phY→region.ty, phZ→region.tz; ctx GEP by the linear
      index. Guardrails (divergence/post-dominance/tid-dependent trip count) generalize
      to the 3-placeholder taint.
  - **Verify:** a 2-D shared+barrier kernel (e.g. an 8×8 tile transpose-with-barrier or a
    2-D block reduction) on CPU; the Inc-6 1-D reduction stays SIMD (no urem in its region
    loop); wave+barrier still W-lane. Then drop the `no3d` mark on barrier kernels.
  - ✅ **DONE.** Three placeholders (tid.x/y/z), context arrays sized `ntid.x·y·z` and
    indexed by the linear index `tz·ntidYX + ty·ntidX + tx` (base loop-invariant in the x
    loop → uniform, so the inner GEP is `base + tx`, contiguous/SIMD). Each region is a
    tid.z→tid.y→tid.x nest; the inner tid.x latch is the wave/forced-VF loop. The `no3d`
    mark + runtime guard are removed. Test `barrierKernelOver2dBlock` (8×8 shared transpose);
    all 19 barrier tests green (Inc-6 reduction, nested, multi-barrier, wave+barrier,
    dynamic-shared unregressed).

---

## 2. `@Device` helper calls

**Today:** a kernel calling a user-defined `@Device` helper throws `XPU-N01` —
kernels must be monolithic.

**Goal:** lower `@Device` methods as ordinary device functions the kernel can
call; inline or keep as calls per backend. Enables normal code factoring.

**Touch-points:** `@Device` attribute recognition; lowering `@Device` methods
into the device module; call-site lowering in the kernel-body walk; per-backend
function-call ABI (all three GPU targets support device functions; CPU is just a
host call that inlines). Recursion / call graph; address-space-correct pointer
params.

**Mapped wiring:** the kernel body is lowered by `DeviceLowerer` (in
`KernelLowering.cpp`) — a per-function lowerer holding `fn`, `builder`, entry-block
alloca `locals`, `loopTargets`, and `target` (the backend seam). Every method call
in `lowerExpr` routes to `lowerBuiltinCall`, which matches `Thread`/`Workgroup`/
`Barrier`/`Wave` and throws `XPU-N01` (`unsupported("device builtin …")`) for
anything else — that's where a `@Device` call dies today.

**Design (stages):**
1. **Detect a user call.** In `lowerBuiltinCall` (or a new `lowerUserCall` before the
   final `unsupported`), when `recv`/`name` don't match a builtin, resolve the target
   `MethodPtr` (a static method on the kernel's class, or `recv` = a class name) and
   check `isDevice(*m)`. If not `@Device`, keep the `XPU-N01`.
2. **Lower the `@Device` method to a device function**, cached. Add a shared
   `std::map<Method*, llvm::Function*>` device-function cache threaded into
   `DeviceLowerer`. On first call, create the function (params: scalars by value,
   `Buffer<T>`/array params as `addrspace(1)` pointers — reuse `collectParams` +
   `createKernel`-style signature but with a real return type, non-kernel linkage,
   `alwaysinline` for GPU/CPU), then recurse: a fresh `DeviceLowerer` over that fn
   lowers its body (params → entry allocas, statements, `return expr`). Handles a
   helper calling another helper (the cache breaks cycles; reject true recursion with
   a clean `XPU-N01`).
3. **Emit the call** at the site: lower each arg, coerce to the param type, `call` the
   cached function, return its value.
4. **Per backend:** NVPTX/AMDGPU/SPIR-V all support device functions; mark
   `alwaysinline` so SPIR-V (no real call ABI issues) and the others inline cleanly.
   CPU: same IR, inlines into the work-item loop. The `LoweringTarget` may need a
   small hook for the function's calling convention/decoration (mirror
   `decorateKernel`).
5. **Tests:** a kernel calling a `@Device` helper (scalar args + a return) runs on CPU
   (executable) and emits on the GPU backends; a helper-calls-helper chain; a helper
   taking a `Buffer` param (addrspace correctness); reject recursion with `XPU-N01`.

**Risk:** medium-large — it adds the first non-builtin call path to kernel lowering
and a function cache/recursion guard, but reuses the existing `DeviceLowerer` body
walk wholesale. Lower risk than 2D/3D stage 4.

**Progress:**
- ✅ **Core done.** `DeviceLowerer` gains a `@Device` context (the kernel's class +
  a shared `Method*→Function*` cache). `lowerBuiltinCall`, before its `XPU-N01`,
  resolves a non-builtin call to a sibling `@Device` method (same class, by arity),
  lowers it to a cached `alwaysinline` device function (scalar params + scalar/void
  return; a fresh `DeviceLowerer` over the helper's body, `ReturnStatement` now emits
  the value), and emits the `call` with coerced args. A nullptr cache entry catches
  recursion (→ `XPU-N01`). Tests `deviceHelperCallOnCpu` (out[i]=i*i) and
  `deviceHelperChainOnCpu` (helper-calls-helper, out[i]=i+2) run on CPU; full Xpu*
  suite green (141 passed / 7 GPU-skipped / 0 failed — the shared lowerer change
  didn't regress any backend's emit).
- ✅ **Buffer params in helpers done + verified on-device (2026-06-01).** A `@Device`
  helper can now take `Buffer<T>` params. New seam `LoweringTarget::bufferParamType`
  gives the by-value buffer-arg type per backend: `ptr addrspace(1)` (NVPTX/AMDGPU,
  the base `createKernel` reuses it for lockstep), `ptr addrspace(0)` (CPU), and the
  `spirv.VulkanBuffer` storage-buffer HANDLE (Vulkan, matching the kernel's writable
  binding). `DeviceLowerer` gained `setParamsAsArgs(true)` for helpers — their params
  are plain fn args (read via `fn->getArg`), NOT re-materialized, so Vulkan doesn't
  bind a fresh descriptor per helper param. Two host-side fixes were needed: (a) the
  frontend now host-stubs a `@Device` method with a `Buffer` param exactly as it
  already did for `@Kernel` (buffer indexing is device-only — else host Phase-2
  codegen null-derefs on `in[i]`); (b) the SPIR-V emit path runs `AlwaysInlinerPass`
  before instruction selection — a descriptor handle can't cross a call boundary in
  the in-tree SPIR-V selector (`selectStore`→`loadHandleBeforePosition` crashes), so
  the alwaysinline helper must be folded into the kernel first (NVPTX/AMDGPU/CPU
  tolerate the pointer arg across a call, but inlining is the helper's intent anyway).
  Tests: `deviceHelperBufferParamOnCpu`, `deviceBufferParamHelperRoutesToHipOnDevice`
  (AMD gfx1151), `deviceBufferParamHelperOnDevice` (Vulkan RADV) — a helper reading
  one buffer + writing another, `out[i]=in[i]*3`, all green on real hardware.
- ☐ **Follow-ups:** a clean cross-class resolution (helpers must be same-class today);
  an explicit recursion-rejection emit test.

---

## 3. Vulkan block dim — spec-constant `LocalSizeId`

**Today:** first cut bakes a fixed `LocalSize` (`kVulkanLocalSizeX = 64`) via
`hlsl.numthreads`; the launch must match the baked value.

**Goal:** emit a spec-constant `LocalSizeId` so workgroup size is set at pipeline
creation from the launch's `block`, with a per-blockDim pipeline cache. Unblocks
arbitrary block sizes on Vulkan. Self-contained to the Vulkan backend + its
runtime dispatch.

**Feasibility (investigated 2026-05-31):** the size is baked by
`fn->addFnAttr("hlsl.numthreads", "64,1,1")` (`SpirvKernelLowering.cpp`) →
`OpExecutionMode LocalSize 64,1,1`. **There is no LLVM-IR path to a spec-constant
`LocalSizeId`** (same class of limitation as the barrier semantics — no IR
intrinsic exists at all, unlike `Wave.width()` which was fixed by switching to a
selectable intrinsic). So this needs a **post-emit SPIR-V binary patch** — the pattern
`SpirvBackend::fixupControlBarriers` already uses: rewrite the module to add a
`WorkgroupSize` `OpSpecConstantComposite` (or `LocalSizeId` + three
`OpSpecConstant` ids with `SpecId` decorations), switch `OpExecutionMode LocalSize`
→ `OpExecutionModeId LocalSizeId`, and at the runtime: set those spec constants from
the launch `block` via `VkSpecializationInfo` at pipeline creation, keyed into a
per-(bx,by,bz) pipeline cache. **Substantial but well-precedented** (the barrier
fixup is the template). Not started — a focused increment of its own.

**✅ DONE (2026-05-31).** `injectWorkgroupSizeSpecConstant` (SpirvBackend.cpp,
called from `emitSpirv` after the barrier fixup): word-stream surgery that adds
three `OpSpecConstant uint` (SpecId 0/1/2, default = the baked LocalSize dims) + an
`OpSpecConstantComposite v3uint` decorated `BuiltIn WorkgroupSize` (reusing the
existing uint/v3uint types, creating v3uint if absent), inserting the constants
before the first `OpFunction` and the decorations after the last existing one. The
LocalSize execution mode is retained as the default. Runtime: the block dims are
threaded dispatcher → `cajeta_xpu_launch_vulkan` → `cajeta_xpu_vk_launch`, which
sets `VkSpecializationInfo` (SpecId 0/1/2 = block.x/y/z) on the compute pipeline
stage. No pipeline cache needed — the pipeline is already created+destroyed per
launch (a per-(bx,by,bz) cache is a deferred perf optimization). Verified: the
patched SPIR-V passes strict `spirv-val` and runs on-device at the default 64
(12 Vulkan tests green), and `XpuVulkanDispatchDeviceTests.arbitraryBlockSizeOnDevice`
launches **block=128** on the Strix Halo APU and gets the right result (sum 4096;
a stuck-at-64 workgroup would give 3072). The C++ `VulkanDriver` still defaults to
64 (its direct-driver test is unchanged); the production runtime path is the one
that sets the block dim.

---

## 4. Multi-arch bundling (fatbin)

**Today:** single arch per emit. **Goal:** bundle multiple device arches (per
backend) so one artifact runs across a hardware range — NV fatbin / AMD
multi-target / Vulkan is arch-neutral SPIR-V (so mostly an NV/AMD concern).

**✅ AMD done + verified on-device (2026-05-31).** Feasibility was confirmed by a
direct experiment first: `hipModuleLoadData` accepts a clang-offload-bundle and the
HIP runtime selects the running device's arch. Implementation: `assembleHsacoBundle`
(AmdgpuBackend.cpp) assembles a per-arch hsaco from a fresh module clone
(`assembleHsaco` mutates the module via the AMDGPU structurizer — a second
in-place assembly hits `Cannot select llvm.amdgcn.if`) and bundles them with
`clang-offload-bundler` (`-type=o`, targets `host-… , hipv4-amdgcn-amd-amdhsa--gfxXXXX`).
`--xpu-arch` now accepts a comma list (`splitArchList`); `AmdgpuRegistration` +
the `--xpu-emit=hsaco` path call `assembleHsacoBundle`; the embedded bundle loads
through the unchanged runtime (`hipModuleLoadData`). Single arch is unchanged (the
bundle path is just `assembleHsaco`). Tests: `XpuSaxpyAmdDeviceTests.multiArchBundleRunsOnDevice`
(backend: build gfx1100+gfx1151 → load → select → launch → verify on Strix Halo)
and `XpuHipDispatchDeviceTests.multiArchBundleRoutesToHipOnDevice` (full
`--xpu-arch=gfx1100,gfx1151` compiler path → on-device).

**☐ NVIDIA (untestable here).** Parallel structure — `ptxas` per `sm_XX` → cubins
→ `fatbinary` → one fatbin (`cuModuleLoadData` accepts fatbins). **`ptxas`,
`fatbinary`, and `nvcc` are all absent on this box**, so it can't be built or
verified here; deferred rather than ship untested binary-format code.

---

## 5. Vulkan dynamic shared memory

**Today:** deferred — Vulkan sizes workgroup arrays at pipeline creation, not
per-`vkCmdDispatch`, so dynamic LDS is a pipeline-cache key, not a launch scalar.
(CPU dynamic shared landed in Inc 10; NV/AMD are native.) **Goal:** spec-constant
array length keyed into the pipeline cache by the launch's `sharedBytes:`.

**Investigated (2026-05-31): the current path is BROKEN, not just deferred.** A
`shared int32[n]` (runtime `n`) lowers to an external unsized `[0 x T]`
addrspace(3) global; the SPIR-V backend emits it as `OpVariable Workgroup` with
`LinkageAttributes "…" Import` + `OpCapability Linkage`, which `spirv-val` rejects
(`Capability Linkage is not allowed by Vulkan 1.3`). So a dynamic-shared kernel
produces invalid SPIR-V today. **Scope is bigger than Item 3** (which only *added*
ops): the fix needs (1) the IR/lowering to emit a **concrete internal** Workgroup
array for the Vulkan dynamic-shared case (a default `[N x T]`, not an external
import) so the SPIR-V is valid; (2) a post-emit patch turning the array's length
operand into an `OpSpecConstant` (SpecId) — i.e. **modify** the `OpTypeArray`
length and add a spec constant, harder than the add-only Item-3 patch; (3) thread
`sharedBytes` to the Vulkan launcher (it's dropped today — `cajeta_xpu_launch_vulkan`
takes grid/block but not `sharedBytes`) and set the length spec constant
(`sharedBytes/sizeof(T)`) via `VkSpecializationInfo` at pipeline creation. A
substantial, intricate increment of its own.

**✅ DONE + verified on-device (2026-05-31).** And the access path was the real
gotcha: `lowerSharedDecl` decays the shared array to a flat `T*`, so the SPIR-V
backend drops the `OpTypeArray` entirely (the variable became a scalar `i32*`) —
no array to size. Fix in three parts: (1) a `LoweringTarget::dynamicSharedNeedsConcreteSize()`
hook (Vulkan-only) so the dynamic-shared global is a concrete **internal** `[256 x T]`
(>1 so it stays an array, not a decayed scalar), named `cajeta_dynsh_…`; (2) keep
it **typed** — a new `arrayShared` map + a `lowerLValueAddr` branch index it as
`gep arrTy, gv, {0, i}` so the `OpTypeArray` survives; (3) `injectDynamicSharedSpecConstant`
(SpirvBackend) finds that array via its OpName, adds an `OpSpecConstant` (SpecId 3,
default = the baked 256) inserted **before** the `OpTypeArray` (types can't
forward-reference) and repoints the length operand; (4) the runtime threads
`sharedBytes` to the Vulkan launcher and sets SpecId 3 = `sharedBytes/4` via
`VkSpecializationInfo` (alongside Item 3's workgroup-size SpecId 0/1/2). Test
`XpuVulkanDispatchDeviceTests.dynamicSharedOnDevice` (`shared int32[n]`, barrier,
cross-lane read on the Strix Halo APU). **Limitations:** one dynamic shared array
per kernel; 4-byte element (int32/float32) — the runtime divides bytes by 4 (a
non-4-byte element would need the elem size plumbed to the launcher). spirv-val
passes; the patched SPIR-V also runs at the default 256 if `sharedBytes` is unset.

---

## 6. `for-each` parallel loops

**Today:** deferred (`XPU-N01` at `KernelLowering.cpp` — the `EnhancedForStatement`
already parses, only device lowering rejects it). **Goal (decided):** the
**grid-stride loop**, the universal in-kernel idiom (CUDA/HIP/OpenCL/Metal). Surface:

```
for (uint32 i, float32 v : in.range(n)) { out[i] = v * 2.0f; }
```

lowers to

```
for (uint32 i = Thread.globalIdX(); i < n; i += gridDim.x*blockDim.x) {
    float32 v = in[i];      // element binding (a read of in[i])
    out[i] = v * 2.0f;      // index i available for writes
}
```

The iterable is `buffer.range(count)` — a device-recognized pseudo-method (no
grammar change; `in.range(n)` already parses as a `MethodCallExpression`). The
Cajeta iterator extension `for (idx, T elem : …)` supplies the index binding `i`;
the element binding `elem` is `in[i]` (a value read). Any other for-each iterable
in a kernel → clean `XPU-N02` (device buffers are unsized pointers — exactly as in
every GPU language — so an explicit `.range(count)` is required).

**The one real cost: a new coordinate on the lowering seam.** The grid-stride
stride is `gridDim.x * blockDim.x` (total work-items in x), which the seam doesn't
expose today. Add `LoweringTarget::gridSize(b, m, dim)` → i32 (total work-items in
dim):
- **NVPTX:** `nctaid.{x,y,z}` sreg × existing `ntid` (`workgroupDim`).
- **AMDGPU:** one load — `grid_size_{x,y,z}` (uint32) is at HSA dispatch-packet
  offset `12 + 4·dim`; it already equals gridDim·blockDim (total work-items).
- **SPIR-V:** `spv.num.workgroups(dim)` × `spv.workgroup.size(dim)` (both valid in
  GLCompute; `GlobalSize` would need the OpenCL Kernel cap, so avoid it).
- **CPU:** the heavy one — the kernel ABI's 9 trailing coords are
  `[tid.xyz, ctaid.xyz, ntid.xyz]`, with **no grid dimension (block count)**. The
  stride `gx·bx` must be threaded runtime-slice → coord[] → launch-thunk → per-block
  wrapper → kernel, alongside the `dynShared` slot. Invasive on the same coord chain
  the barrier-fission pass owns.

**Staging.** (1) seam `gridSize` + NVPTX/AMD/SPIR-V + the frontend for-each lowering
+ `.range()` recognition + guardrails; device-tested on AMD & Vulkan (Strix Halo);
CPU emits a clean `XPU-N01` fallback in the interim (never a miscompile). (2) ✅ the
CPU coord-ABI extension (`nctaid.xyz` as the 4th coord group, 9→12 params) + CPU
grid-stride test (grid deliberately smaller than `n`, so the stride is exercised, not
just grid-cover).

**Guardrails (`XPU-N02`, before mutation):** iterable not of the form
`ident.range(expr)`; the receiver not a known buffer param; nested for-each over the
same buffer (one level for now); for-each combined with a barrier in the same region
(defer — interacts with fission). Element binding is read-only (writes go via the
index); a write to the element binding name → `XPU-N02`.

**Verification.** AMD + Vulkan on-device: `n=1024`, launch a grid **smaller** than
`n` (e.g. grid=4, block=64 ⇒ 256 work-items < 1024) so the grid-stride loop must
iterate; `out[i] = in[i]*2` for all `i` ⇒ checksum proves every element ran. Emit
test: the kernel IR contains a counted loop whose IV starts at the global id and
increments by `gridSize`. Stage 2 adds the CPU exec test.

**✅ Stage 1 DONE + verified on-device (2026-06-01).** `gridSize(dim)` added to the
`LoweringTarget` seam (pure-virtual); NVPTX (`nctaid·ntid`), AMDGPU (one
dispatch-packet load of `grid_size`, offset 12+4·dim), SPIR-V
(`num_workgroups·workgroup_size`). Frontend: `lowerEnhancedFor` in
`KernelLowering.cpp` recognizes `buf.range(count)`, binds the optional index +
element (a value copy of `buf[i]`), and emits the grid-stride loop
(`i=globalId.x; i<count; i+=gridSize.x`); non-`range` iterables and non-buffer
receivers → `XPU-N02`. `EnhancedForStatement` gained element/iterator accessors.
Tests: `XpuVulkanDispatchDeviceTests.gridStrideForEachOnDevice` (RADV) and
`XpuHipDispatchDeviceTests.gridStrideForEachRoutesToHipOnDevice` (gfx1151), both
grid=4·block=64 over n=1024 ⇒ sum 2048 (every element ran via the stride). CPU
emits a clean `XPU-N01` (host-stub fallback) until Stage 2 threads `gx·bx` through
the coord ABI. Full `Xpu*` suite green (148/7-skipped).

**✅ Stage 2 DONE — CPU grid-stride (2026-06-01).** The CPU kernel coord ABI grew
from 9 to 12 trailing i32 params: the 4th group, `nctaid.xyz` (gridDim = block
count), is threaded from the launch ABI through the whole chain — runtime
`run_slice` coord array (now 13: `[tid, ctaid, ntid, nctaid, dynShared]`, dynShared
moved to `coord[12]`), `CpuDriver`, the launcher thunk (`coord[3..11]` → wrapper's 9
block-coord params), the per-block wrapper (passes `nctaid` into both the barrier-free
work-item-loop call and the fission `vmap`), and the kernel itself. `CpuTarget::
gridSize(dim)` now returns `nctaid(dim)·ntid(dim)` instead of throwing `XPU-N01`. Test:
`XpuCpuDispatchTests.gridStrideForEachOnCpu` — `n=1024`, grid=2·block=64 ⇒ gridSize=128
< n, so each work-item strides over 8 elements; `in[i]=i`, `out[i]=v`, verifying
`out[i]==i` for **all** i proves full coverage with the correct per-element index (a
stride of just `ntid`=64 would miscover). `XpuCpuExecTests` SAXPY signature + the two
coord-count comments in `XpuCpuEmitTests` updated to 12. Full CPU suite green (46).

> ✅ **Root-caused & fixed (2026-06-01).** The intermittent `LLVM ERROR: Unable to
> get address space id` abort (seen once, in `XpuVulkanEmitTests.lowersSaxpyToSpirv`)
> was a **TargetMachine reuse** bug in two SPIR-V emit unit tests
> (`lowersSaxpyToSpirv`, `lowersStridedSumLoop`): each ran *two* codegen passes
> (`emitSpirvText` + `emitSpirv`) through **one** TM. LLVM's SPIR-V backend
> accumulates codegen state (`SPIRVGlobalRegistry`) on the TM — the production
> `VulkanRegistration`/`Compiler` paths use a fresh TM per kernel/emit for exactly
> this reason (documented in `VulkanRegistration.cpp`), but these tests didn't. The
> corrupted registry intermittently emits a garbage address space. Fix: a fresh TM
> per emit in both tests (matching the production invariant); the anti-pattern
> existed *only* in these two tests. Corroborating: ASAN found no heap corruption
> (consistent with SPIR-V-internal logical state, not a C++ heap bug). The original
> event is rare (1 in ~30 unfixed-binary runs), so a 16-round differential couldn't
> reproduce it on the old binary either (inconclusive by statistics) — but the fix
> removes a documented anti-pattern and the fixed binary is clean across ~28
> full-suite runs.

---

## 7. POD structs as kernel args

**Today:** kernel args need explicit `implements KernelArg`. **Goal:** accept
plain POD structs by value as kernel arguments (layout-compatible marshalling
through the `kernelParams` ABI).

**✅ DONE + verified on-device (2026-06-01).** A plain `class P { <all-primitive
fields> }` — no inheritance, no marker interface — is now admissible by value as
an `@Kernel` arg; the kernel reads its fields with `p.field`.

- **Admit (front-end):** `isKernelArgAdmissible` gains an `isPodStruct` branch
  (`KernelArgTrait.cpp`) — a non-interface, non-`Buffer`, **no-inherited-fields**
  class with ≥1 field, all of whose instance fields are primitives. Non-POD
  classes (non-primitive/inherited fields) still need the explicit
  `implements KernelArg` opt-in, so `XPU-K01` is unchanged for them. Exported as
  `isPodStructType` so the launch-site marshaller shares the exact predicate.
- **Marshal (launch site, `CallExpression.cpp`):** the host class carries a
  vtable pointer at LLVM slot 0; that word is **stripped**. Each field is copied
  out of the host instance (`StructGEP` at its host index) into a packed,
  declaration-order buffer — the exact shape the device reads. (Field-by-field,
  so no host-vs-device padding assumption.) `argv[i]` points at that buffer.
- **Lower (device, `KernelLowering.cpp`):** `collectParams` classifies a struct
  param to a vtable-stripped device `StructType` of its primitive fields
  (`deviceStructInfo`); it rides the existing by-value (non-buffer) path —
  `createKernel` emits it as an aggregate kernel param, `collectKernelParamInfo`
  reports its alloc-size for the Vulkan SSBO wrap. Field reads are an
  **`extractvalue`** from the materialized SSA aggregate — crucially **no alloca
  round-trip**: an aggregate store to a Function-storage pointer is rejected by
  the SPIR-V backend (`spirv-val`: *"not a logical pointer"*), so the struct is
  kept as an SSA value and `OpCompositeExtract` reads each field. Struct params
  are **read-only** in v1 (`name.field = …` → clean `XPU-N01`).
- **Per backend:** NVPTX/AMDGPU take the struct by value in `kernelParams`
  (native); CPU's launcher-thunk loads the aggregate from `argv[i]`; Vulkan wraps
  it in a single descriptor-bound storage buffer (the scalar-SSBO mechanism with
  element type = the struct) and reads fields via `OpCompositeExtract`.
- **Tests:** `XpuKernelArgTests.{podStructAdmissible,nonPodUserTypeRejected}`
  (front-end), `XpuCpuDispatchTests.podStructArgOnCpu` (runnable),
  `Xpu{Nvptx,Amdgpu}LoopEmitTests.lowersPodStructArg` +
  `XpuVulkanEmitTests.lowersPodStructArgToSpirv` (emit + `spirv-val`), and on
  real hardware `XpuHipDispatchDeviceTests.podStructArgRoutesToHipOnDevice`
  (gfx1151) + `XpuVulkanDispatchDeviceTests.podStructArgOnDevice` (RADV) —
  `out[i] = i*scale + bias`, scale=2/bias=1/n=256 ⇒ Σ(2i+1) = 65536.
- **v1 limits:** all-primitive fields only (no nested structs / arrays / Buffer
  fields), no inheritance, read-only in the kernel. 4-byte and 8-byte primitive
  fields are layout-compatible across host + all device targets (natural
  alignment); sub-word fields are untested. Follow-ups: writable struct params;
  nested-POD fields.

---

## 8. Texture / Sampler types — ✅ done

**`Texture2D` + `Sampler` kernel-arg types with `tex.sample(sampler, u, v)`** —
2-D sampled texture, float32 texels, normalized coords, nearest/bilinear
filtering, clamp/wrap addressing, explicit LOD 0 (compute-valid). The
`sampleTexture` seam on `LoweringTarget` is the per-backend variance point; each
backend reaches its native hardware texture unit (or emulates it):

| Backend | Texture handle | Sample lowering | Status |
|---------|----------------|-----------------|--------|
| **CPU** | host texobj ptr | `__cajeta_xpu_cpu_tex_sample` (C bilinear) | ✅ runs |
| **Vulkan** | `spirv.Image` + `spirv.Sampler` descriptors | `llvm.spv.resource.samplelevel` → `OpImageSampleExplicitLod` | ✅ on-device (RADV) |
| **AMD** | `ptr addrspace(4)` HIP texture object | `__ockl_image_sample_2D` (ROCm device lib) → `image_sample` | ✅ on-device (gfx1151) |
| **NVIDIA** | i64 `cudaTextureObject_t` | `llvm.nvvm.tex.unified.2d.v4f32.f32` → PTX `tex.2d` | ◐ emit-only (no NV HW) |

**Marshalling:** a texture flows like a `Buffer` (the int64 `deviceHandle`); a
`Sampler` flows like a by-value POD `{i32 filterMode, i32 addressMode}`. On
Vulkan the sampler is its own descriptor; on AMD/NVIDIA the sampler state is
baked into the texture object at creation (so the kernel's separate `Sampler`
arg is unused on-device there). The Vulkan + AMD launch paths translate the
texture arg into the native object per launch (descriptor write / texture object).

**Key dependencies / decisions:**
- **LLVM 23** — Vulkan needs `llvm.spv.resource.samplelevel` (absent in LLVM 22).
- **AMD: hybrid device-library linking** — `ockl.bc` (+ `oclc_isa_version_<gfx>.bc`)
  is linked *only* into kernels that sample (other AMD kernels, and the
  ROCm-bitcode dependency, are untouched/opt-in). This reuses ROCm's correct SRD
  construction + coord normalization rather than hand-building gfx descriptors;
  it's also the same mechanism that would later unlock `ocml` math intrinsics.

**v1 limits (follow-ups):** 2-D only (no 1-D/3-D/cube/array); single float
channel (no RGBA/integer/normalized-int formats); no mipmaps/LOD selection,
depth/compare, or anisotropy; one sampler paired per texture; NVIDIA sampling is
emit-only here (no hardware to verify the dispatch).

---

## 9. `Wave.width()` on-device (Vulkan) — ✅ done

Originally ⛔: `spv.wave.get_lane_count` lowers from IR but the SPIR-V backend
**cannot select it** (`intrinsic selection not implemented` — still true through
LLVM 23). The fix is Cajeta-side: route `waveWidth` to **`llvm.spv.subgroup.size`**
instead — the selectable sibling of the `spv.subgroup.local.invocation.id` already
used by `Wave.laneId()`. The SPIR-V instruction selector lowers it via
`loadBuiltinInputID(BuiltIn::SubgroupSize)` → `OpLoad` of the `SubgroupSize`
builtin (verified: `llc -mtriple=spirv-unknown-vulkan-compute` emits
`OpDecorate … BuiltIn SubgroupSize`). `Wave.width()` now runs natively on Vulkan
(`XpuWaveDeviceTests.vulkanWaveWidthRunsOnDevice`, on RADV/gfx1151), alongside the
existing NV/AMD/CPU paths. No upstream change or post-emit workaround needed.
