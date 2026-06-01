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

### ⚑ Pre-flight before any Vulkan work (#3, #5, #9)

A background task — *"Verify fixup semantics value + run all Vulkan Tier-0
tests"* — failed with **exit 144** during this session. It is in the Vulkan
barrier-fixup area (`SpirvBackend::fixupControlBarriers`, the
`WorkgroupMemory|AcquireRelease` = 0x108 semantics value). It may be a
`cajeta-two` artifact rather than a real regression, but **before starting Vulkan
items #3/#5, re-run the Vulkan Tier-0 suite on this branch and confirm green** (or
fix the regression first). Don't build new Vulkan capability on a red base.

## Status

| # | Item | Class | Effort | Status |
|---|------|-------|--------|--------|
| 1 | **2D/3D launch** | capability | large | ✅ done (ABI + GPU 3-D + CPU 3-D grid/block/fission) |
| 2 | **`@Device` helper calls** | capability | medium-large | ☐ not started |
| 3 | **Vulkan block dim — spec-constant `LocalSizeId`** | vulkan | medium | ☐ not started |
| 4 | **Multi-arch bundling (fatbin)** | deployment | medium | ☐ not started |
| 5 | **Vulkan dynamic shared memory** | vulkan | medium | ☐ not started |
| 6 | **`for-each` parallel loops** | capability | medium | ☐ not started |
| 7 | **POD structs as kernel args** | capability | small-medium | ☐ not started |
| 8 | **Texture / Sampler types** | capability | large | ☐ not started |
| 9 | **`Wave.width()` on-device (Vulkan)** | vulkan | blocked | ⛔ external (LLVM 22 SPIR-V can't select `spv.wave.get_lane_count`) |

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

---

## 3. Vulkan block dim — spec-constant `LocalSizeId`

**Today:** first cut bakes a fixed `LocalSize` (`kVulkanLocalSizeX = 64`) via
`hlsl.numthreads`; the launch must match the baked value.

**Goal:** emit a spec-constant `LocalSizeId` so workgroup size is set at pipeline
creation from the launch's `block`, with a per-blockDim pipeline cache. Unblocks
arbitrary block sizes on Vulkan. Self-contained to the Vulkan backend + its
runtime dispatch.

---

## 4. Multi-arch bundling (fatbin)

**Today:** single arch per emit. **Goal:** bundle multiple device arches (per
backend) so one artifact runs across a hardware range — NV fatbin / AMD
multi-target / Vulkan is arch-neutral SPIR-V (so mostly an NV/AMD concern).

---

## 5. Vulkan dynamic shared memory

**Today:** deferred — Vulkan sizes workgroup arrays at pipeline creation, not
per-`vkCmdDispatch`, so dynamic LDS is a pipeline-cache key, not a launch scalar.
(CPU dynamic shared landed in Inc 10; NV/AMD are native.) **Goal:** spec-constant
array length keyed into the pipeline cache by the launch's `sharedBytes:`.

---

## 6. `for-each` parallel loops

**Today:** deferred (`XPU-N01`). **Goal:** a parallel-loop construct that lowers
to grid-stride iteration (or maps to the launch grid).

---

## 7. POD structs as kernel args

**Today:** kernel args need explicit `implements KernelArg`. **Goal:** accept
plain POD structs by value as kernel arguments (layout-compatible marshalling
through the `kernelParams` ABI).

---

## 8. Texture / Sampler types

**Today:** deferred. **Goal:** texture/sampler kernel-arg types + sampling ops.
Large — backend-specific (CUDA texture objects, AMD image resources, Vulkan
sampled images / descriptor sets). Lowest priority of the capability items.

---

## 9. `Wave.width()` on-device (Vulkan) — ⛔ externally blocked

`spv.wave.get_lane_count` lowers from IR, but **LLVM 22's SPIR-V backend cannot
select it**, so `Wave.width()` doesn't run on-device on Vulkan (it emits but
fails selection). Not a Cajeta-side fix — tracked until the upstream backend
gains the pattern (or a post-emit workaround is found). NV/AMD/CPU all have
`Wave.width()`.
