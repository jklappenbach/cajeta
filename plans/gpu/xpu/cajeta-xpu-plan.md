# Cajeta XPU — compute spec

**`cajeta-xpu` is the heterogeneous-*compute* layer** — kernels, dispatch, and the
parallel execution model across CPU + GPU — for numerical workloads (science, ML,
stats, general parallel compute). It is the cross-PU (any-processing-unit) layer: it's
the one that genuinely spans CPU *and* GPU (the LLJIT CPU backend, wave = SIMD, the
grid→threads model). Graphics is GPU-only and lives in `cajeta-gfx`.

```
cajeta-gpu        (foundation — value types, math, textures, device/codegen/memory)
   ▲
   │ depends on
cajeta-xpu        (this spec — compute execution: kernels, dispatch, waves, barriers)
```

**Scope boundary.** Foundation concerns — `Vector`/`Matrix`/`Quaternion`, math
intrinsics, textures/samplers, the codegen pipeline, the device/driver layer, the
memory/buffer model — belong to **`cajeta-gpu`** and are *not* re-specified here; this
spec depends on them. What lives here is the **compute execution model**: the kernel,
how it's dispatched, and the coordinate/wave/barrier/shared-memory machinery a kernel
uses. (Code today is under `src/cajeta/xpu/` and the `__cajeta_xpu_*` ABI; the gpu/xpu
physical split is staged — see the refactor-strategy note in `cajeta-gpu-plan.md`.)

Checkbox legend: `[x]` landed+tested · `[~]` partial · `[ ]` not started.
Backends: **NV** NVPTX→cubin · **AMD** AMDGPU→hsaco · **VK** SPIR-V · **CPU** LLJIT.
"on-device" = real hardware (AMD gfx1151 Strix Halo; VK RADV; NV gated on CUDA HW).
**Apple/macOS:** Tier 1 = the VK backend via **MoltenVK** (Vulkan→Metal, ~zero
backend work — the runtime already dlopen's Vulkan); Tier 2 = a native **`metal`**
backend (SPIRV-Cross→MSL) for what MoltenVK can't do (cooperative matrix, reliable
ray query). Full strategy + sequencing in `cajeta-gpu-plan.md` § Platforms — Apple/macOS.
**Working agreement:** one increment at a time, tests + docs + commit checkpoint;
never a miscompile; commit only when asked; **no attribution trailer**; stage files
explicitly. Companion docs (under `cajeta-docs/gpu/xpu/`): the capability matrix
`CajetaXPU-Matrix.md`, the CPU-backend reference `CajetaCPU.md`, the XPU spec
`CajetaXPU.md`, and the cross-backend discipline `CajetaXPU-Variance.md`.

---

## Part I — Compute substrate ✅ (the floor)

Four backends, one shared kernel-lowering walk, a runtime dispatcher, and a capability
set deep enough to write real kernels. **Done** — the ground the numerics/ML stack
stands on. (Foundation pieces these depend on — codegen, device layer, buffers — are
tracked in `cajeta-gpu`.)

### Stage 0 — CPU backend bring-up (Increments 1–10) ✅
- [x] CPU lowering/codegen/driver — kernels run via LLVM LLJIT
- [x] Coordinate ABI — 12 trailing `i32` coords (`tid.xyz, ctaid.xyz, ntid.xyz, nctaid.xyz`); grid→threads model, no hardware intrinsics
- [x] Inc 5A — multi-core threading of the grid launch
- [x] Inc 5B — CPU SIMD via loop-vectorization + an `--opt` pipeline
- [x] Inc 5C — wave-op SIMD via the LLVM Vector Function ABI (wave = host SIMD lane)
- [x] Inc 6 — workgroup barriers via POCL-style work-item-loop fission
- [x] Inc 7 — multiple barriers per uniform loop; register accumulator across a loop
- [x] Inc 8 — nested uniform loops with barriers
- [x] Inc 9 — wave ops + barriers composed in one kernel
- [x] Inc 10 — dynamic (runtime-sized) shared memory + a barrier

### Stage 1 — Runtime dispatcher: kernel launch routing ✅
- [x] Kernel module registration `__cajeta_xpu_register_module(name, bytes, len)`, name-keyed
- [x] Routes **launch** + `Buffer<T>` device memory to the winning backend (device acquisition/selection itself is a `cajeta-gpu` concern)
- [x] Vulkan launch-ABI asymmetry handled (scalars wrapped in transient SSBOs from per-kernel param metadata)

### Stage 2 — 2D/3D launch (Item 1) ✅
- [x] 3-D launch ABI `__cajeta_xpu_launch(name, gx,gy,gz, bx,by,bz, sharedBytes, argv)`; site parses `grid:`/`block:` `[x]`/`[x,y]`/`[x,y,z]`
- [x] NV/AMD pass full 3-D grid+block; VK 3-D grid (block baked → Stage 4)
- [x] CPU non-barrier 3-D — `run_slice` over the 3-D block grid; wrapper nests `for tid.z,y,x`
- [x] CPU barrier fission over a multi-dim block — 3-D nest per region, linear context index, inner `tid.x` stays the SIMD/wave loop (no `urem` regression)

### Stage 3 — `@Device` helper calls (Item 2) ✅
- [x] Resolve a non-builtin call to a sibling `@Device` method; cached `alwaysinline` device function; recursion guard (→ `XPU-N01`)
- [x] Scalar params + scalar/void return; helper-calls-helper chains
- [x] `Buffer<T>` params in helpers — `LoweringTarget::bufferParamType` seam; verified on AMD & VK
- [ ] **Follow-up:** clean cross-class resolution (helpers are same-class today)
- [ ] **Follow-up:** explicit recursion-rejection emit test

### Stage 4 — Vulkan spec-constant workgroup size (Item 3) ✅
- [x] `injectWorkgroupSizeSpecConstant` post-emit patch (SpecId 0/1/2 + `OpSpecConstantComposite` `BuiltIn WorkgroupSize`); runtime sets block dims via `VkSpecializationInfo`; on-device at block=128
- [ ] **Perf follow-up:** per-`(bx,by,bz)` pipeline cache (created/destroyed per launch today)

### Stage 5 — Vulkan dynamic shared memory (Item 5) ✅
- [x] Concrete internal `[N x T]` Workgroup array via `dynamicSharedNeedsConcreteSize()`; `injectDynamicSharedSpecConstant` (SpecId 3) repoints the `OpTypeArray` length; runtime threads `sharedBytes` → `VkSpecializationInfo`; on-device
- [ ] **Limits to lift:** one dynamic array per kernel; 4-byte element only

### Stage 6 — `for-each` parallel loops (Item 6) ✅
- [x] Grid-stride lowering `for (i, T v : buf.range(n))` ⇒ `for (i=globalId.x; i<n; i+=gridSize.x){ v=buf[i]; … }`
- [x] `LoweringTarget::gridSize(dim)` — NV `nctaid·ntid`, AMD dispatch-packet `grid_size`, VK `NumWorkgroups·WorkgroupSize`; on AMD & VK
- [x] CPU coord ABI extended 9→12 (`nctaid.xyz`); `gridSize = nctaid·ntid`; verified (grid<n forces the stride)
- [x] Guardrails `XPU-N02` (non-`range` iterable, non-buffer receiver, write to element binding)

### Stage 7 — POD structs as kernel args (Item 7) ✅
- [x] `isPodStruct` admissibility (no inheritance, ≥1 field, all-primitive) — no `implements KernelArg` needed
- [x] Launch-site marshalling: vtable-stripped, field-by-field, declaration-order packed buffer
- [x] Device read via `extractvalue` / `OpCompositeExtract` off the SSA aggregate (no alloca); on AMD & VK
- [ ] **Follow-up:** writable struct params; nested-POD / array / Buffer fields; sub-word fields

### Stage 8 — Wave / subgroup ops ✅
- [x] `Wave.shuffleSync`/`ballotSync`/`reduceSum`/`laneId` via the 5-method `LoweringTarget` wave seam; emit on all GPU backends, on-device AMD + VK; wave = SIMD lane on CPU (Inc 5C)
- [x] `Wave.width()` on VK routed to selectable `llvm.spv.subgroup.size` (SubgroupSize builtin) — runs on RADV (the one that the SPIR-V backend couldn't select otherwise)
- [x] `Wave.rotate` (subgroup cyclic shift) — fork; on-device VK/AMD (`CajetaXPU-Matrix.md` §8; doc `SubgroupRotate.md`)
- [x] `Wave.reduceMax/Min/And/Or/Xor` + `reduceProduct` uniform group arithmetic — fork; on-device VK/AMD (doc `WaveReductions.md`)
- [x] `Wave.prefixSum`/`prefixProduct` exclusive subgroup scans — fork; on-device VK/AMD (doc `WavePrefixScan.md`)
- [x] Auto **maximal reconvergence** correctness companion emitted with the wave ops (no backend patch needed)

> **Compute uses of `cajeta-gpu` features, already built:** `Texture2D.sample` in
> kernels (Item 8) and `Vector<T,N>` in kernels (S5, device codegen) are *exercised*
> by compute but **owned by `cajeta-gpu`** (the type + sampling seam). Tracked there.

---

## Part II — Completing the compute layer (forward)

The remaining execution-model capabilities the numerics/ML stack will demand. Finishing
Part II + the Part I follow-ups is the **definition of done** for `cajeta-xpu`.

### Stage 9 — Atomics & synchronization
- [x] `atomicAdd/Min/Max/And/Or/Xor/Exchange/CompareExchange` on global & shared (integer; on-device CPU/VK/AMD — doc `IntegerAtomics.md`)
- [x] Float atomics (NV/AMD native; VK `SPV_EXT_shader_atomic_float_*` capability check) — on-device, doc `FloatAtomics.md`
- [x] Functional kernel-side memory fence — `Barrier.workgroupMemory()` / `.deviceMemory()` (the `memoryFence(scope)` seam): a scoped memory barrier with no thread rendezvous, fixed AcquireRelease. OpMemoryBarrier (VK, `spirv-val` clean) / scoped `acq_rel` fence (AMD `agent`/`workgroup`) / `membar.gl`/`membar.cta` (NVPTX, emit-only) / system fence (CPU). Device-verified CPU/VK/AMD. *(Named under `Barrier`, not the host-facing `Fence` class — separate concern. Explicit memory-order surface is the next item.)*
- [~] Memory-order surface (`acquire`/`release`/`relaxed`/`seq_cst`) on the seam — internal model exists (VK Device-scope + AcquireRelease; monotonic default) but is **not yet surfaced** as a user-facing API
- [x] On-device tests: histogram, reduction-by-atomics, spin-free counters (Tour `histogram` + `reduceAtomic` + shared-atomic-counter demos)

### Stage 10 — Streams & async compute
- [ ] `Stream` real semantics — overlap copy/compute, dependency ordering (the copy *primitive* is `cajeta-gpu`; the compute-overlap scheduling is here)
- [ ] Multiple in-flight launches; event-based dependencies between kernels
- [ ] CPU: map streams onto the threadpool; GPU: native queues

### Stage 11 — Kernel-language completeness
- [ ] Labeled `break`/`continue` in kernels (deferred `XPU-N01`)
- [ ] Cross-class `@Device` helpers (Stage 3 follow-up) + true call ABI (not always-inline) where backends allow
- [ ] Function pointers / device-side dispatch (bounded)
- [ ] Printf-style device debug (`printf` on NV/AMD; VK debug-printf extension)
- [ ] Kernel specialization constants surfaced to the language (compile-time `const` params)

### Stage 12 — Compute launch & FFI handoff
The minimum surface a separate plan needs to *drive* compute. (Device enumeration/
selection and the alloc/buffer FFI are `cajeta-gpu`; the **launch/marshalling** contract
is here.)
- [ ] Stable C ABI / FFI launch entry points so external code (the numerics/ML ports) can register kernels and dispatch
- [ ] A documented, versioned **launch + kernel-arg marshalling** contract (frozen so downstream targets a stable interface)
- [ ] Explicit per-launch device/queue targeting over the dispatcher (multi-GPU dispatch; device handles come from `cajeta-gpu`)

---

## Acceptance targets — what `cajeta-xpu` must be able to support

Out of scope as *work* (each is its own plan), but the **target that defines "complete."**
`cajeta-xpu` is finished when the substrate can support building each — i.e. when every
compute capability they demand exists and is tested. Listed as the requirements lens:
target → demand → satisfying stage (xpu unless noted `gpu`).

| Acceptance target (own plan) | Compute demand it imposes | Satisfied by |
|---|---|---|
| **Numerics — NumPy/SciPy** | broadcast-shaped element-wise + reduction kernels, GEMM tiling, RNG kernels, strided gather/scatter, a stable launch FFI | Stages 9, 10, 12 (+ gpu: value types, math, memory) |
| **PyTorch port** (DL engine) | conv/matmul/norm kernels, **atomics** for scatter/grad-accum, multi-GPU dispatch, async overlap | Stages 9, 10, 12 (+ gpu: math, fp16/bf16) |
| **Keras port** | none beyond the torch port | inherits |
| **ETE port** | mostly host-side; little compute demand | — |
| **Toffee** — new ML framework; **primary focus: SPELA forward training** | **fused per-layer (forward + local cosine-loss + update) kernels**, per-layer/early-exit eval, on-device/continual-learning dispatch | Stages 9, 11, 12 (+ gpu: unit-sphere "symmetric-vector" ops) |

> **SPELA** (`ml/spela-training`): *Solo Pass Embedded Learning Algorithm* — per-layer
> local cosine-similarity loss vs fixed unit-sphere "symmetric vectors", detached
> inter-layer inputs ⇒ **forward-only, no global backprop**. The compute demand it adds
> over ordinary DL is **fusing a layer's forward + local-loss + weight update into one
> device pass** (Stage 11 kernel-language reach + Stage 12 launch contract; the
> symmetric-vector math is `cajeta-gpu`). The training *framework* is the separate Toffee plan.

If a Part II stage exists **only** because a target needs it, that's correct. A compute
capability no target demands is a candidate to cut or defer.

---

## Definition of done for `cajeta-xpu`

- [ ] Part I follow-ups closed (or deferred with a tracked reason): the per-stage follow-ups in Stages 3/4/5/7.
- [ ] Part II Stages 9–12 landed, each test-gated on CPU + on-device (AMD/VK; NV once `cajeta-gpu` Stage B5 lands hardware).
- [ ] A thin **proof-of-support compute probe per target** runs on the substrate (not the full library): a broadcast+reduction kernel (numerics); a matmul + atomic grad-scatter kernel (torch); a fused per-layer forward+cosine-loss+update pass (Toffee/SPELA).
- [ ] The capability matrix (`CajetaXPU-Matrix.md`) is honest and current for the compute rows.
- [ ] The Stage 12 launch/FFI contract is documented and frozen.
- [ ] `cajeta-gpu` has cleared *its* definition of done (compute depends on it).

---

## Cross-cutting tracks (run alongside Part II)

- [ ] **Testing** — per-stage gtest discipline; on-device gates for AMD/VK (NV when HW lands)
- [ ] **Benchmarking** — kernel-throughput regression tracking as atomics/streams land
- [ ] **Documentation** — keep `CajetaXPU-Matrix.md` honest (emit-only vs on-device); document the Stage 12 launch contract
- [ ] **CI** — multi-backend runners (CPU always; AMD/VK self-hosted; NV when available)

---

*Part I is the done compute floor; Part II is the remaining `cajeta-xpu` work. The
foundation it stands on is `cajeta-gpu`; the numerics stack, framework ports, and Toffee
that stand on *it* are out of scope here (each its own plan), gated on this plan's
definition of done. Graphics is `cajeta-gfx`, a sibling over the same `cajeta-gpu` base.*
