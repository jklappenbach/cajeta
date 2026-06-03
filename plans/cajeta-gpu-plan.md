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
  - [ ] **Increment 2b — the operations** (turnkey map, ready to implement): the ops use the **generic `buildOpFromWrapper(MIRBuilder, Opcode, Call, TypeReg)`** helper (`SPIRVBuiltins.cpp:626`; `TypeReg=Register(0)` for void ops, `GR->getSPIRVTypeID(Call->ReturnType)` for valued). Touch list:
    - `SPIRVBuiltins.td`: `def RayQuery : BuiltinGroup;` (beside `CoopMatr`, line 66) + `DemangledNativeBuiltin` records — `__spirv_RayQueryInitializeKHR`(8,8,Op…Initialize), `__spirv_RayQueryProceedKHR`(1,1,…Proceed), `__spirv_RayQueryGetIntersectionTypeKHR`(2,2,…GetType).
    - `SPIRVInstrInfo.td`: `OpRayQueryInitializeKHR` Op<4473> (void, 8 ID operands: rq,accel,flags,mask,origin,tmin,dir,tmax) · `OpRayQueryProceedKHR` Op<4477> (bool result, 1 operand) · `OpRayQueryGetIntersectionTypeKHR` Op<4479> (uint result, 2 operands).
    - `SPIRVBuiltins.cpp`: a small `generateRayQueryInst` (lookup opcode via `lookupNativeBuiltin`, void for Initialize else result type, call `buildOpFromWrapper`) + dispatch `case SPIRV::RayQuery:` (~line 3578).
    - **Fiddly part = the test**: a spirv-val-clean `GLCompute` kernel with correctly Itanium-mangled `__spirv_*` calls + an AS descriptor binding — the spatial-index pattern from the probe. (The op-emission text check is cheap; the valid-kernel + mangling is the iteration cost.)
  - [ ] **Increment 3 — Cajeta surface + host AS build**: emit the builtin calls from `DeviceLowerer`; host-side acceleration-structure build (incl. AABB geometry) in the gpu device layer; Prism `SpatialIndex` consumes it.
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
