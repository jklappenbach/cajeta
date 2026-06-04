# Cajeta GPU — foundation spec

The **shared GPU foundation** that both compute and graphics build on. Anything that
is *not* specific to the kernel/compute execution model and *not* specific to the
graphics pipeline lives here: the codegen/device/driver plumbing, the memory & buffer
model, value types, math intrinsics, and textures.

```
cajeta-gpu        (this spec — the foundation)
   ▲                       ▲
   │ depends on            │ depends on
cajeta-xpu             cajeta-gfx
(compute:              (graphics:
 science/ML/stats)      pipeline + engine)
```

**Why a separate spec:** the value types (`Vector`/`Matrix`/`Quaternion`), math
intrinsics, the SPIR-V/cubin/hsaco emit machinery, the device/driver layer, the
memory/buffer model, and textures are needed *identically* by compute kernels and by
graphics shaders. Factoring them out keeps the compute spec (`cajeta-xpu`) and the
graphics spec (`cajeta-gfx`) from forking the substrate. *(Today this code lives in the
xpu tree because it was built for compute first; this spec is the seam along which it
becomes shared.)*

## Code-refactor strategy (gpu ⟂ xpu) — read before moving files

A survey of the tree (2026-06-02) found the physical split is **smaller and riskier
than it looks**, and should be staged, not big-banged:

- **Already foundation, no move needed.** The cleanest foundation — the value types
  `CajetaVector` / `CajetaConstantType` / `VectorOps` — already live in
  `src/cajeta/type/`, *not* under `src/cajeta/xpu/`. They are core language types
  (host-usable; the host JIT tests need no GPU). Forthcoming `Matrix`/`Quaternion`
  land there too. So "gpu value types" need **no physical move**.
- **Mixed prelude.** `runtime/src/cajeta/xpu/core/` holds both foundation
  (`Buffer`, `Texture2D`, `Sampler`, `AddressSpace`, `Capabilities`, `Stream`,
  `Event`, `Fence`) and compute (`Thread`, `Wave`, `Workgroup`, `Barrier`,
  `KernelArg`, `KernelError`). Splitting it changes fully-qualified package names
  (`cajeta.xpu.core.*` → `cajeta.gpu.core.*`).
- **Mixed ABI.** The `__cajeta_xpu_*` runtime symbols are split the same way —
  `__cajeta_xpu_{buffer,texture}_*` are foundation, `__cajeta_xpu_{launch,barrier,
  thread}_*` are compute. Renaming them touches the runtime, the codegen that emits
  the calls, and tests — a wide, hard-to-reverse ABI change.

**Decision: defer the physical package/ABI rename; do it as a dedicated, test-gated
effort sequenced with the *first* `cajeta-gfx` work** (the second consumer that
validates the seam). Until then: (1) the spec-level split is authoritative; (2)
foundation *features* advance in `src/cajeta/type/` where no rename is needed (the
value types — see Stage A3/B1); (3) a half-renamed ABI is worse than none, so the
rename happens all-at-once when it happens. This sequencing avoids speculative
factoring before graphics exists, exactly as a shared base should be extracted.

Checkbox legend: `[x]` landed+tested · `[~]` partial (sub-bullets) · `[ ]` not started.
Backends: **NV** NVPTX→cubin · **AMD** AMDGPU→hsaco · **VK** SPIR-V · **CPU** LLJIT.
**Working agreement:** one increment at a time, tests + docs + commit checkpoint;
never a miscompile; commit only when asked; **no attribution trailer**; stage files
explicitly. Companion docs: `cajeta-xpu-matrix.md`, `cajeta-xpu.md`, `cajeta-amd.md`,
`cajeta-vulkan.md`, `cajeta-cpu.md`.

---

## Part A — Foundation already built ✅

### Stage A0 — Codegen pipeline (triple · assembler · binary) ✅
- [x] Per-backend device triple + `*Backend` (`nvptx64-nvidia-cuda`, `amdgcn-amd-amdhsa`, `spirv64-unknown-vulkan1.3-compute`, CPU)
- [x] Lowered text + object emit (`addPassesToEmitFile`); NV `ptxas`→cubin, AMD `ld.lld`→hsaco, VK direct SPIR-V binary, CPU LLJIT
- [x] `decorateKernel`/entry-point seam (CC + `nvvm.annotations` / `amdgpu_kernel` / `OpEntryPoint`)
- [x] In-process module registration `__cajeta_xpu_register_module(name, bytes, len)` via `llvm.global_ctors`, name-keyed (byte format is the only per-backend difference)
- [x] LLVM 23 migration (API + semantic fixes)

### Stage A1 — Device & driver layer ✅
- [x] Driver acquisition via dlopen — `libcuda.so.1`, `libamdhip64.so` (+ pinned `libhsa`), `libvulkan.so.1` (entry points via `vkGetInstanceProcAddr`)
- [x] Backend availability detection + selection order `CUDA → HIP → Vulkan → CPU`
- [x] VK device pick: first physical device with a compute queue (RADV reaches Strix Halo)
- [x] Module load per backend (`cuModuleLoadData` / `hipModuleLoadData` / `vkCreateShaderModule`→pipeline)

### Stage A2 — Memory & buffer model ✅
- [x] `Buffer<T>` RAII (ctor-allocate / `~Buffer` free); device alloc/upload/download
- [x] Address-space model (`AddressSpace.h`) — Global/Shared/Constant/Private/Generic mapped per backend; alloca-AS seam (0/5/7)
- [x] VK buffer model = descriptor-set SSBOs (BDA has no IR path); scalars as single-element SSBOs

### Stage A3 — `Vector<T,N>` value type ✅ (S6 wrap-up open)
- [x] **S1** non-type template substrate — `CajetaConstantType : CajetaType`; grammar integer arm in `typeArgument`
- [x] **S2** `Numeric`/`Floating`/`Integral` marker bounds (rejects `bool`)
- [x] **S3** `CajetaVector` type + `fromContext` interception; `getLlvmType()` → `FixedVectorType::get(elem, N)`; by-value POD marshalling
- [x] **S4** host codegen — construct, `.xyzw`/`.rgba` + `v[i]` read/assign, element-wise arith + scalar broadcast, `dot`/`length`/`normalize` (shared `vecops` helpers)
- [x] **S5** device codegen — `DeviceLowerer` mirror reusing `vecops`; verified on CPU, VK, AMD
- [~] **S6** consolidation & diagnostics:
  - [x] distinct diagnostics — `CAJETA_ERROR_VECTOR_COMPONENT` (lane bounds), `_CONSTRUCT` (ctor arity), `_ELEMENT_TYPE` (non-bool numeric element), `_LENGTH` (positive N), `_METHOD` (incl. `length`/`normalize` on integral element). *(verified 2026-06-02 — already complete in the tree)*
  - [x] `Buffer<Vector<float32,4>>` end-to-end — `XpuVectorDeviceTests.bufferOfVectorRunsOnCpu` (16-byte element stride + whole-vector `out[i] = <4 x float>` store; CPU JIT). *(2026-06-02 — codegen already handled it; the gap was test coverage)*
  - [ ] matrix/doc updates
  - [ ] RGBA bridge — retype `Texture2D.sample` → `Vector<float32,4>` at each `sampleTexture` seam (drop the lane-0 extract)

### Stage A4 — Texture / Sampler types ✅
- [x] `Texture2D` + `Sampler` types; `tex.sample(sampler, u, v)` — 2-D, float32 texel, normalized coords, nearest/bilinear, clamp/wrap, LOD 0
- [x] `LoweringTarget::sampleTexture` seam — CPU (C bilinear), VK (`samplelevel`→`OpImageSampleExplicitLod`), AMD (`__ockl_image_sample_2D`); on-device
- [x] NV `llvm.nvvm.tex.unified.2d`→PTX `tex.2d` (emit-only); requires LLVM 23 (VK); AMD `ockl.bc` hybrid-linked only for sampling kernels

### Stage A5 — Multi-arch binary bundling 🟡
- [x] AMD fatbin — `--xpu-arch=gfx1100,gfx1151` → `clang-offload-bundler`; `hipModuleLoadData` selects device arch; on-device
- [ ] NVIDIA fatbin — `ptxas` per `sm_XX` → cubins → `fatbinary` (`ptxas`/`fatbinary` absent on this box)

---

## Part B — Foundation still to build (forward)

The substrate value types, math, and texture/memory completeness that the compute and
graphics specs both depend on. Finishing Part B is the **definition of done** for the
foundation.

### Stage B1 — Linear-algebra value types
- [ ] `Matrix<T,R,C>` — lowers to `<R*C x T>` (or array-of-vectors); reuses the non-type-param substrate from A3/S1
- [ ] Matrix construction, element/row/column access, `m[i][j]`; `matmul`, transpose, identity, elementwise ops
- [ ] `Matrix × Vector` / `Matrix × Matrix`; determinant/inverse for 2/3/4-square (transform sizes)
- [ ] `Quaternion` type + slerp/normalize/rotate
- [ ] Swizzles `.xyz`/`.xy`/`.xxyy` (multi-component reads — deferred from Vector v1)
- [ ] Vector comparisons (`==`/`<` → `<N x i1>` mask) + `any`/`all`/`select`
- [ ] `cross`, `reflect`, `refract`, `distance`, `clamp`, `lerp`, `min`/`max` (HLSL/GLSL intrinsic set)

### Stage B2 — Device math intrinsics
- [~] **Increment 1 — native-lowering subset, in kernels (2026-06-02).** `Math.{sqrt,floor,ceil,trunc,round,abs,min,max,fma}` lower in the device `DeviceLowerer` (`KernelLowering.cpp::lowerMathCall`) to LLVM intrinsics that select natively on **all four backends** — no device math-library link. Operates in the argument's FP type (f32-native; not the host's forced f64). Transcendentals give a clean `XPU-N01` until increment 2. Tests: `XpuMathDeviceTests` (emit + CPU + clean-reject + Vulkan/RADV + AMD/gfx1151 on-device, 5/5 green).
- [ ] **Increment 2 — transcendentals** `sin/cos/tan/exp/log/pow/rsqrt` — per-backend device-lib realization (NV `libdevice`/`nvvm`, AMD `ocml` — same hybrid-link shape as Item 8's `ockl.bc`, VK `OpExt GLSL.std.450`, CPU libm). *(host path for these already exists in `MethodCallExpression.cpp`)*
- [ ] Vectorized forms (elementwise over `<N x T>`); fast/approx variants + fast-math flags
- [ ] `float16`/`bfloat16` element types (`<N x half>`/`<N x bfloat>`) — ML/graphics dtypes

### Stage B3 — Texture & sampler completeness
- [ ] RGBA / multi-channel formats (depends on the A3/S6 RGBA bridge)
- [ ] Integer & normalized-int formats; 1-D / 3-D / cube / array textures
- [ ] Mipmaps + explicit/implicit LOD; anisotropy; depth/compare samplers
- [ ] Writable images (`imageStore`) — compute-generated render targets (also the gfx bridge)
- [ ] Multiple samplers per texture; separate sampler objects

### Stage B4 — Memory & data ergonomics
- [ ] Pinned/host-mapped memory; unified memory where available
- [ ] Sub-buffer / strided views (`Buffer.slice`, non-contiguous gather)
- [ ] Async copies / transfer queues (the copy primitive; the *compute*-overlap `Stream` semantics live in `cajeta-xpu`)
- [ ] Bindless / multi-buffer descriptor sets (VK) beyond per-arg binding

### Stage B5 — NVIDIA codegen/device verification
- [ ] **NV runner = the x86-64 Windows + NVIDIA box, via WSL2** (Ubuntu + CUDA-on-WSL), reusing the **`linux-x64`** LLVM-23 artifact from C0 + the CUDA toolkit (`ptxas`/`fatbinary`). Registered as a second self-hosted **Linux** runner. This is the only NVIDIA hardware (the Strix Halo dev box is AMD-only); WSL2 avoids gating NV bring-up on the native-Windows/mingw toolchain (which is release-only — see `plans/c0/`). *(Verify CUDA-on-WSL exposes libcuda + ptxas on first setup.)*
- [ ] Unblocks the **7 currently-skipped NV exec tests** + verifies the NV emit-only foundation on-device (textures A4, math B2); complete NV fatbin (A5)
- [ ] Promote the NVIDIA column of `cajeta-xpu-matrix.md` from "emit-only" to "on-device measured"

---

## Part C — Cutting-edge SPIR-V feature completeness (via a downstream LLVM)

**Goal (owner directive): expose *every* cutting-edge GPU call SPIR-V/Vulkan can
make — and for anything Vulkan exposes that LLVM can't yet lower, add it.** Cajeta
emits SPIR-V through LLVM's **in-tree** SPIR-V backend, so the ceiling on "what GPU
calls are possible" is set by that backend, not by Cajeta. Closing the gap is partly
Cajeta-side (surface the call) and partly **LLVM-side** (teach the backend to lower
an opcode it doesn't). LLVM-side changes live in `llvm/lib/Target/SPIRV/` — **never
in the Cajeta repo** — so they need a distribution strategy (C0) and an honest audit
of the gap (C2).

### Stage C0 — LLVM distribution: downstream fork + prebuilt artifact (**adopted**)
The Cajeta repo depends on an LLVM that has our backend patches; it does not contain
them. Today we already build **stock upstream `main`** (`cpp/llvm-project`, 23-git,
no local commits) — so the "get a custom LLVM to CI" problem already exists for the
graphics path; feature patches just turn "stock main build" into "patched main build."
- [ ] Fork `llvm/llvm-project` on GitHub → rename to **`cajeta-llvm`** (fork relationship + upstream sync survive the rename); carry backend patches on a tracking branch (`cajeta-spirv`) over a **pinned** upstream base commit (`203c0668d`). Runbook + workflows in `plans/c0/`.
- [ ] A CI job **in the fork** builds LLVM **once** and publishes a **prebuilt artifact** (GitHub Release tarball / GHCR container image / build cache)
- [ ] Cajeta CI + local dev **consume** that artifact via `LLVM_DIR` — Cajeta CI never builds LLVM and never vendors LLVM source (same pattern Rust/Swift/Zig use to ship custom LLVM)
- [ ] **Host-artifact matrix mirrors `release.yml`** (`x86_64-linux-gnu`, `aarch64-linux-gnu`, `aarch64-apple-darwin`, `x86_64-w64-mingw32`) — one toolchain per *host*; the `X86;NVPTX;AMDGPU;SPIRV` *targets* are all in each. **`release.yml` today pulls distro LLVM (apt/brew/MSYS2 ≤22)** — C0 must replace that with the LLVM-23 artifact per leg, or 23-only features (graphics SPIR-V, ray query) break in release builds. Build **`linux-x64` first** (unblocks all Part C dev + AMD *and* NVIDIA on-device testing — NV via WSL2 on the Windows box, see B5); native Windows/mingw leg is **release-only, deferred**. Runner topology + matrix detail in `plans/c0/README.md`.
- [ ] **Upstream-first**: anything upstream will accept (ray query, cooperative-vector, …) goes up as a PR; the fork branch carries only not-yet-landed / in-review patches, minimizing rebase debt
- [ ] Rebase cadence: re-pin to a newer upstream base periodically; drop patches that landed upstream; document the base commit + applied-patch list in the fork README

### Stage C1 — The two-flavor reality (read before auditing)
LLVM's SPIR-V backend has **two dialects**, and a feature wired in one is not
automatically reachable from the other:
- **OpenCL "Kernel"** — `spirv64-unknown-unknown`, `OpCapability Kernel`, Physical addressing. Driven by SYCL/oneAPI; **most cutting-edge compute lowerings land here first** (enabled with `--spirv-ext=+SPV_…`).
- **Vulkan "Shader"** — `spirv-unknown-vulkanN-stage`, `OpCapability Shader`, Logical GLSL450. **This is what Cajeta emits** (we target Vulkan, not an OpenCL runtime).

So a feature with a CodeGen test under `spirv64-unknown-unknown` is *lowerable* but may **not** be reachable from our Vulkan flavor — and "teaching the backend" is often **porting an existing OpenCL-flavor lowering to the Shader flavor** (smaller than writing it from scratch). Hence the three-tier audit below.

### Stage C2 — Feature audit (verified against LLVM 23-git `Target/SPIRV/`, 2026-06-02)
Tiering rule: *lowerable* = IR intrinsics/builtins **and** a `test/CodeGen/SPIRV` test. Symbolic-table-only (an enum name with no opcode/intrinsic) = **stub, not lowerable**.

**Tier 1 — already reachable from our Vulkan flavor → probe + surface in Cajeta (no LLVM patch):**
- [ ] **Integer dot product** (`int_spv_{s,u,f}dot`, `SPV_KHR_integer_dot_product`) — DP4a int8/int4 — HLSL-oriented intrinsics, Vulkan-ready. (Quantized inference.)
- [ ] **Float atomics** (`SPV_EXT_shader_atomic_float_add` / `_min_max` via `atomicrmw fadd/fmin/fmax`, incl. f16/f32/f64) — scientific reductions, ML. *(confirm Shader-flavor reach)*
- [ ] **Shader clock** (`SPV_KHR_shader_clock`) — in-kernel profiling/timing
- [ ] **Subgroup rotate** (`SPV_KHR_subgroup_rotate`), **uniform group instructions**, **bit instructions**, **maximal reconvergence** (`SPV_KHR_maximal_reconvergence`) — confirm + expose beyond today's wave ops
- [ ] **Physical storage buffer / buffer device address** (`SPV_KHR_physical_storage_buffer`, addressing model + `physical-layout` test present) — raw pointers in shaders → flexible/pointer-rich data structures. *(verify, then surface)*

**Tier 2 — lowerable but wired OpenCL-flavor only → port to the Shader flavor (medium: capability/addressing wiring, no new opcodes):**
- [ ] **Cooperative matrix** (`SPV_KHR_cooperative_matrix`, `OpTypeCooperativeMatrixKHR`) — **tensor / matrix-core MMA; the single biggest ML lever (Prism / SPELA matmul).** Tested under `spirv64 --spirv-ext`; needs Shader-capability reach.
- [ ] **bfloat16 arithmetic** (`SPV_KHR_bfloat16`) — ML dtype; spirv64-tested → port to Shader flavor
- [ ] **fp16 vector atomics** (`SPV_NV_shader_atomic_fp16_vector`)

**Tier 3 — absent in BOTH flavors → full lowering (the real backend work, fork-carried until upstreamed):**
- [~] **Ray query + acceleration structures** (`SPV_KHR_ray_query`) — **in progress in the `cajeta-spirv` fork branch, TDD per increment.** Was only NV enum stubs. Findings: the zero-param opaque types use the backend's **default `getNonParameterizedType` lowering** (no custom helper needed, simpler than coop-matrix); ray query is **`[EnvVulkan]`-only** (rejected under the OpenCL `spirv64` triple) — so it's inherently the Vulkan/Shader flavor Cajeta emits. Tests live in `llvm/test/CodeGen/SPIRV/extensions/SPV_KHR_ray_query/`.
  - [x] **Increment 1 — `OpTypeRayQueryKHR` opaque type** (2026-06-03): `target("spirv.RayQueryKHR")` → `OpTypeRayQueryKHR` under `spirv-unknown-vulkan1.3-compute`, gated by `RayQueryKHR` capability + `SPV_KHR_ray_query` extension (clean fatal error without it). 5 backend edits — `SPIRVInstrInfo.td` (Op<4472>), `SPIRVBuiltins.td` (BuiltinType), `SPIRVSymbolicOperands.td` (RayQueryKHR cap<4472>), `SPIRVInstructionSelector.cpp` (isTypeInst), `SPIRVModuleAnalysis.cpp` (requirement+error). Test `ray_query_type.ll` green (text emission; spirv-val deferred to inc 2's real kernel). Incremental `llc` rebuild ≈ 14 s.
  - [x] **Increment 2a — `OpTypeAccelerationStructureKHR` opaque type** (2026-06-03): `target("spirv.AccelerationStructureKHR")` → `OpTypeAccelerationStructureKHR` (opcode 5341 — **tablegen accepts the duplicate Op<5341> alongside the existing NV def**, so the KHR name prints directly), same RayQueryKHR cap + extension gating. 4 edits (InstrInfo/Builtins/InstructionSelector/ModuleAnalysis). Test `acceleration_structure_type.ll` green. **Both opaque types now lower under the Vulkan flavor.**
  - [x] **Increment 2b — the operations** (2026-06-03, via `llvm.spv` intrinsics): `OpRayQueryInitializeKHR`/`ProceedKHR`/`GetIntersectionTypeKHR` all emit under the Vulkan flavor. Test `ray_query_ops.ll` green (FileCheck). **Key finding (cost a dead end first):** the `__spirv_*` builtin-call path is gated `if (!STI.isShader())` (`SPIRVPrepareFunctions.cpp:473`) — OpenCL/Kernel-only — while `SPV_KHR_ray_query` is `[EnvVulkan]`-only → mutually exclusive, so builtins can **never** lower ray query (verified: the calls stayed `LinkageAttributes Import`). Ops must (and now do) use **`llvm.spv.ray.query.*` intrinsics + GlobalISel selection** — the texture-style path that runs for every flavor. Implementation: 3 intrinsics (`IntrinsicsSPIRV.td`), 3 opcodes (`SPIRVInstrInfo.td`), and selection (`SPIRVInstructionSelector.cpp`) — `selectOpWithSrcs` for the valued Proceed/GetType, a small `selectRayQueryInitialize` for the void 8-operand init. Emitted shape verified: `OpVariable Function` rq + 8-operand init + valued proceed/get. (Unmangled intrinsic calls work — LLVM infers the overload from arg types.)
  - [x] **Increment 2c — spirv-val-clean kernel** (2026-06-03): a `GLCompute` entry with a **descriptor-bound** acceleration structure running the spatial-index pattern (init → `OpLoopMerge` proceed loop → inspect candidate type → read committed type). Test `ray_query_kernel.ll` passes both FileCheck and `spirv-val --target-env vulkan1.3`. **Key finding: zero new backend code needed.** The AS handle comes from the standard `llvm.spv.resource.handlefrombinding` intrinsic, and `OpTypeAccelerationStructureKHR` falls into the *generic non-image branch* of `loadHandleBeforePosition` (`SPIRVInstructionSelector.cpp`), which already emits exactly what's required: an `OpVariable UniformConstant` decorated `DescriptorSet 0`/`Binding 0`, then an `OpLoad` of the AS handle feeding `OpRayQueryInitializeKHR`. So Cajeta binds the AS exactly as it binds textures/buffers — no special path. (The by-value-param emission tests from 1/2a/2b are kept as minimal lowering checks; they intentionally pull in `Linkage` and are deliberately *not* run under `spirv-val` — 2c is the validity proof.) The type-level `SPIRVModuleAnalysis` requirements (cap `RayQueryKHR` + ext `SPV_KHR_ray_query`) were sufficient; no op-level requirements needed.
  - **Increment 3 — Cajeta surface + host AS build** (split into 3 sub-increments; the language/device surface is verifiable now, the host BVH build needs an RT-capable device):
    - [x] **3a — device-lowering surface (emit + spirv-val, no device needed)** (2026-06-03): new runtime types `cajeta.xpu.core.AccelerationStructure` (kernel-arg handle, bound like Texture2D) and `cajeta.xpu.core.RayQuery` (device-only kernel local; methods `initialize`/`proceed`/`committedType`/`candidateType`, origin/direction passed as scalar triples so no Vector<T,N> dep). Wired: `KernelArgTrait` (admit AS by canonical name; `isAccelStructType`/`isRayQueryType`), `KernelParam.isAccelStruct` + `collectParams`, `SpirvKernelLowering` (`vkAccelStructType()`/`vkRayQueryType()`, AS bound via the *existing* `handlefrombinding` path — proven in 2c; 4 ray-query seam overrides), `LoweringTarget` (4 new seam methods, default→XPU-N02 on non-Vulkan), and `DeviceLowerer` (`RayQuery` local → `alloca target("spirv.RayQueryKHR")`; method-call interception → `llvm.spv.ray.query.*`; `makeVec3`/`resolveAccelArg` helpers). Test `XpuVulkanEmitTests.lowersRayQueryToSpirv` green: IR has handlefrombinding + the 3 intrinsics; SPIR-V text has `OpCapability RayQueryKHR`/`OpTypeAccelerationStructureKHR`/`OpRayQueryInitializeKHR`/etc.; **binary passes `spirv-val --target-env vulkan1.3`**. No regressions (all Vulkan/NVPTX/AMD/CPU/Wave emit suites green). Two surface gotchas fixed: Cajeta's boolean primitive is `boolean` (not `bool`), and a zero-field class has a null LLVM type (RayQuery needed a placeholder field). **Build wiring (important):** the in-process SPIR-V emitter needs the extension enabled — added `enableSpirvExtensions()` in `SpirvBackend.cpp` that sets the `spirv-ext` cl::opt to `+SPV_KHR_ray_query` (the SPIRVSubtarget header isn't shipped in the artifact, so we drive the registered option). And cajeta's local build was repointed to the fork (`LLVM_DIR=cajeta-llvm/build-cajeta/lib/cmake/llvm`) — verified safe because `cajeta-llvm` HEAD~5 == the `llvm-project` checkout cajeta previously used (`203c0668d`), i.e. the fork is exactly that base + the 5 additive ray-query/CI commits.
    - [x] **3b — host BVH build + descriptor binding (EXEC-verified on real RT GPU)** (2026-06-03): the runtime Vulkan path (`cajeta_runtime.c`, which is the *actual* device path — the C++ `VulkanDriver.cpp` is compiler/test-only) gains the full BVH path. **Device bring-up** now probes for the RT triad (`VK_KHR_acceleration_structure` + `VK_KHR_ray_query` + `VK_KHR_deferred_host_operations` + `VK_KHR_buffer_device_address`) plus the matching feature bits via `vkEnumerateDeviceExtensionProperties`/`vkGetPhysicalDeviceFeatures2`, and **conditionally** enables them (chained `VkPhysicalDevice{RayQuery,AccelerationStructure,BufferDeviceAddress}Features` in `VkDeviceCreateInfo.pNext`) — a non-RT GPU is created exactly as before, `g_xpu_vk.rayQuery` stays 0, and the AS natives no-op, so the plain compute/texture path is untouched. **AS build** (`cajeta_xpu_vk_accel_build_aabbs`): device-address buffers (`VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT`) for AABB input / AS store / scratch; AABB geometry (`VkAabbPositionsKHR` is byte-identical to cajeta's 6-float box, so the `float32[]` uploads straight in); `vkGetAccelerationStructureBuildSizesKHR` → `vkCreateAccelerationStructureKHR` → `vkCmdBuildAccelerationStructuresKHR` (BOTTOM_LEVEL, PREFER_FAST_TRACE); AS+store kept in a table, AABB+scratch freed. **Descriptor bind**: new `CAJ_VKB_ACCEL` kind → `VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR` in the layout/pool/writes (write chains `VkWriteDescriptorSetAccelerationStructureKHR` via pNext); new `CAJETA_KP_ACCEL` param kind so `cajeta_xpu_launch_vulkan` routes the AS arg (and `KernelParamInfo::AccelStruct=4` + `collectKernelParamInfo` emit it). **Native host entries** `__cajeta_xpu_accel_build_aabbs`/`__cajeta_xpu_accel_free` (the `AccelerationStructure.cajeta` `@Native` forwards; floats at array-header offset 8, Vulkan-only — other backends return 0/no-op). C++ `VulkanDriver::rayQueryAvailable()` is a self-contained test gate (enumerates ext+features2, no logical device). Exec test `XpuVulkanDispatchDeviceTests.rayQuerySpatialIndexOnDevice`: a BLAS over one AABB `[0,1]³`, a kernel that casts a degenerate near-zero ray per query point and counts AABB candidates (RTNN pattern) — inside-box point → ≥1 candidate, outside → 0 — **ran green on a real AMD Radeon 8060S (RADV STRIX_HALO)** (777). Full XPU suite 185 passed / 7 skipped (CUDA/HIP — no device) / 0 failed; no regressions.
    - [x] **3c — Prism `SpatialIndex` (P1.0, EXEC-verified on real RT GPU)** (2026-06-03): the library primitive consuming 3a/3b lands as Prism's first real code — `cajeta-prism/src/prism/spatial/SpatialIndex.cajeta` (new `cajeta-prism` sibling repo + README). A `SpatialIndex` builds a bottom-level AS (BVH) over per-point AABBs (each datum wrapped in a half-extent-`r` box) and exposes the fixed-radius verb `countWithin(qx,qy,qz,out,n)`; the degenerate near-zero-ray / AABB-as-index / candidate-walk (RTNN pattern) is hidden inside an internal `@Kernel` — the user writes `idx.countWithin(...)`, never a ray. Exec test `PrismSpatialIndexDeviceTests.fixedRadiusCountOnDevice` (cajeta repo; reads the authoritative SpatialIndex source from the sibling repo, compiles it with a driver via the multi-source JIT, gated on `rayQueryAvailable()` + source presence) **ran green on the AMD Radeon 8060S (RADV STRIX_HALO)** (neighbour counts 1/0/1/1 → 777). Remaining P1 (knn/radius/contains/range verbs, custom-predicate exact-L2, refit/rebuild policy, compute fallback, precision safety, RT→tensor handoff) tracked in `cajeta-prism-plan.md` P1.1–P1.6.
  - [ ] Upstream-first PR once the lowering is complete; carry on `cajeta-spirv` until it lands.
  - [ ] Upstream-first PR once the lowering is complete; carry on `cajeta-spirv` until it lands.
- [ ] **fp8 / E4M3 / E5M2** float types — 0 backend files (only bfloat16 present). ML low-precision training/inference.
- [ ] **Cooperative vector** (`SPV_NV_cooperative_vector`) — 0 files — small-matrix / neural-shading MMA (newest)
- [ ] **Tensor addressing** (`SPV_NV_tensor_addressing` / `TensorNV`) — 0 files
- [ ] **Quad control** (`SPV_KHR_quad_control`), **replicated composites** (`SPV_EXT_replicated_composites`) — 0 files — minor

### Stage C3 — Sequencing (prioritized for compute · science · ML)
- [ ] **1. C0 infra first** — the fork + prebuilt-artifact pipeline; everything else rides it
- [ ] **2. Tier-1 probe sweep** — per-feature emit probes (the `GfxSpirvEmitProbe` pattern) to confirm Vulkan-flavor reach, then surface the confirmed calls in Cajeta
- [ ] **3. Ray query + acceleration structures** (Tier 3) — headline foundation gap; smaller than full RT; upstream-first
- [ ] **4. Cooperative matrix → Shader flavor** (Tier 2) — tensor-core matmul; biggest ML lever
- [ ] **5. bf16 (port) + fp8 (add)** — ML precision
- [ ] **6. Cooperative vector + tensor addressing** (Tier 3) — newest neural primitives
- [ ] **7. Minor**: quad control, replicated composites; confirm subgroup/bit/reconvergence

### Placement: what is `cajeta-gpu` vs `cajeta-gfx`
By the shared-foundation rule (*if both xpu and gfx need it → gpu*):
- **`cajeta-gpu` (here):** ray query + acceleration structures, cooperative matrix, float atomics, integer dot, bf16/fp8, cooperative vector, tensor addressing, physical storage buffer — **all compute-callable from an ordinary kernel**, needed by both science/ML (`cajeta-xpu`) and inline-RT graphics (`cajeta-gfx`).
- **`cajeta-gfx` (NOT here):** the **full RT pipeline** (raygen / closest-hit / miss / any-hit / callable execution models, ray-payload & hit-attribute storage classes, `OpTraceRayKHR`, shader binding table) and mesh/task shaders — rendering-specific shading model. Cross-referenced in `cajeta-gfx-plan.md`.

**Why ray query is foundation, not graphics — prior art (compute uses of RT, not rendering):** Monte-Carlo radiation/particle transport (reactor & medical dosimetry); fixed-radius / k-NN search (RTNN, PPoPP 2022) → MD neighbor lists, point clouds, clustering; computational-geometry predicates (point-in-mesh, visibility, range queries); wave/field propagation (NVIDIA Sionna RT for 5G/6G, room acoustics, seismic tomography); CT reconstruction (line-integral projection); robotics motion planning; astrophysical radiative transfer. Scientific users want **`rayQuery` inside a compute kernel** over a BVH — *not* the rendering RT pipeline — which is exactly why the ray-query lowering belongs in the shared foundation. *(Domain list recalled from training; citations to be firmed up before this justifies the backend spend.)*

---

## Platforms — Apple / macOS (Metal)

**The crux: macOS has no native Vulkan/SPIR-V — Apple is Metal-only.** Apple deprecated
OpenGL/OpenCL and never shipped Vulkan; the GPU stack is **Metal** (API) + **MSL/AIR**
(shader language / IR, AIR being Apple's closed LLVM-bitcode dialect). Our SPIR-V-centric
design therefore needs a bridge to reach Apple GPUs. There are exactly two, and they form
a natural two-tier strategy. **Note:** this is independent of C0 — SPIR-V codegen is
host-agnostic, so the `aarch64-apple-darwin` *toolchain* artifact (deferred in C0) is a
build-out task; the items below are about *device execution* on Apple hardware.

### Tier 1 — MoltenVK (Vulkan→Metal). Works with ~no Cajeta backend changes.
MoltenVK is a Khronos (Apache-2.0) Vulkan implementation layered on Metal. The fit is exact:
our runtime **already loads Vulkan via `dlopen` with no link-time dependency**, so on macOS
it loads MoltenVK's `libvulkan.dylib` instead of a native ICD — the SPIR-V emission path is
unchanged. Baseline compute (`cajeta-xpu`) + graphics (`cajeta-gfx`) light up for ~the cost
of bundling MoltenVK + a dlopen name.
- The gap is **feature coverage** — MoltenVK is strong on core compute/graphics but weak/
  partial on **exactly the Part C features we care about most**:
  - **`VK_KHR_ray_query`** — only recently/partially mapped to Metal raytracing, M3+ only,
    experimental → the Prism RT-as-compute story is shaky here.
  - **`VK_KHR_cooperative_matrix`** — effectively **not** exposed (Metal's `simdgroup_matrix`
    MMA isn't surfaced through this Vulkan extension). The single biggest gap.
  - **`VK_KHR_buffer_device_address`** — partial, and our AS build relies on it.

### Tier 2 — a native Metal backend. For what MoltenVK can't do.
A new `metal` XPU backend alongside `cpu`/`spirv`/`cuda`/`hip`. The pragmatic path **reuses
our SPIR-V emission**: SPIR-V → **SPIRV-Cross** → **MSL** → Metal runtime compile
(`newLibraryWithSource` / precompiled `.metallib`), with a new `LoweringTarget` + a Metal
driver via **`metal-cpp`** (Apple's official C++ headers). There is **no public LLVM→AIR/Metal
backend** (Apple's Metal compiler is closed), so transpiling from SPIR-V is the realistic
route — not a direct LLVM target. This unlocks the Apple hardware that matters:
- **Metal raytracing** (M3/M4 hardware RT) → reliable ray query → Prism `SpatialIndex` on Apple.
- **`simdgroup_matrix`** → cooperative-matrix / MMA → **SPELA forward training + Prism tensor
  path on Apple Silicon's GPU**.

### Per-spec
| Spec | Tier 1 (MoltenVK, now) | Tier 2 (native Metal, later) |
|---|---|---|
| **cajeta-gpu** | value types, math, memory, textures, basic compute ✓ | ray query (Metal RT), cooperative matrix (`simdgroup_matrix`) |
| **cajeta-xpu** | a Vulkan-via-MoltenVK device, reusing the SPIR-V backend ✓ | a first-class `metal` backend (full perf + features) |
| **cajeta-gfx** | MoltenVK is mature for graphics (how most macOS games ship) ✓ | mesh shaders, native Metal RT pipeline, MetalFX upscaling |

### Roadmap impact (why Tier 2 matters for us specifically)
The two pillars of the ML/RT thesis — **Prism's RT-as-compute spatial index** and **SPELA
cooperative-matrix training** — are exactly the two Tier-1 gaps (ray query + coop matrix).
So MoltenVK makes Apple a *baseline* compute/graphics target but **not** a first-class home
for the ML/RT work; cooperative matrix in particular is **Tier-2-only**.

### Sequencing (mirrors the NVIDIA/AMD approach: Vulkan first, native later)
- [ ] **MV1 — MoltenVK bring-up.** Bundle/`dlopen` MoltenVK on macOS; run SAXPY / textures /
  Prism compute-fallback on Apple Silicon (Metal-via-Vulkan). ~zero backend work. The "it
  works on a Mac today" milestone. *(First concrete increment.)*
- [ ] **MV2 — MoltenVK capability probe** (same pattern as the Vulkan ray-query SPIR-V probe,
  `test/gpu/GpuRayQueryProbeTests.cpp`): pin down *exactly* what `VK_KHR_ray_query` /
  `cooperative_matrix` / `buffer_device_address` MoltenVK exposes on M3/M4, so Tier 2's scope
  is measured, not guessed.
- [ ] **MT1 — native Metal backend, cooperative matrix first** (the ML lever, Tier-2-only):
  `LoweringTarget` + Metal driver (`metal-cpp`), SPIRV-Cross→MSL, `simdgroup_matrix`.
- [ ] **MT2 — Metal raytracing** for ray query (improves on MoltenVK's partial support).

### Unknowns to verify (de-risk before committing Tier-2 spend)
- MoltenVK's **exact** ray-query + coop-matrix + BDA support on M3/M4 (→ probe MV2).
- SPIRV-Cross fidelity for our compute SPIR-V (workgroup/subgroup ops, shared memory,
  `<N×T>` vectors, atomics) → MSL — any constructs that don't round-trip cleanly.
- Whether a precompiled-`.metallib` path is needed (vs runtime MSL compile) for startup cost.
- Licensing/packaging: MoltenVK (Apache-2.0) + `metal-cpp` (Apple) are bundleable; confirm.

---

## Definition of done for the foundation

- [ ] A3/S6 closed (or deferred with a tracked reason); A5 NV fatbin done or deferred
- [ ] Part B Stages B1–B5 landed, each test-gated on CPU + on-device (AMD/VK; NV once B5 lands hardware)
- [ ] The capability matrix is honest and current (every cell `native`/`abstraction`/`forks`/`not possible`; emit-only vs on-device distinguished)
- [ ] The value-type / math / texture / memory surface is documented as a **frozen dependency contract** that `cajeta-xpu` and `cajeta-gfx` target
- [ ] **Part C is feature-completeness, largely parallel to the freeze** — B1–B5 define the *frozen contract*; Part C keeps extending the cutting-edge call surface afterward. The exceptions that *should* land early because downstream depends on them: **C0** (fork+artifact infra — gates every later backend patch) and the **ray-query** + **cooperative-matrix** items (science/ML blockers).

When checked, both dependent specs can build on a stable foundation.

---

*The foundation is the shared substrate; it does not by itself run a kernel or draw a
frame. The compute execution model is `cajeta-xpu`; the graphics pipeline is
`cajeta-gfx`. Both depend on this spec.*
