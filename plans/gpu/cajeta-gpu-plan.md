# Cajeta GPU — foundation plan

Task tracker for the `cajeta.gpu.core` foundation. The **spec** is
[`cajeta-docs/gpu/CajetaGPU.md`](../../cajeta-docs/gpu/CajetaGPU.md); this plan is sectioned to
match it and tracks *what's left*, not the design rationale (that lives in the doc; history
lives in git). Compute and graphics have their own plans
([`xpu/cajeta-xpu-plan.md`](xpu/cajeta-xpu-plan.md), [`gfx/cajeta-gfx-plan.md`](gfx/cajeta-gfx-plan.md)).

**Scope rule (from the doc):** stdlib ships **core only**. Vendor-exclusive silicon
(`nvidia`/`amd`/`metal` libraries, cooperative vector, MFMA, MPS, CUDA/ROCm interop) is **out
of this plan** — external libraries on `olla.cajeta.dev`, each its own effort. The ray-tracing
*pipeline* (SBT, hit/miss shaders) is **gfx**.

Legend: `[x]` done (device-verified unless noted) · `[~]` partial · `[ ]` not started ·
`[⊘]` deferred by decision.

---

## 1. Architecture — the model to build (doc §1)

The verb seam exists; the rest of the model from doc §1 does not.

- [x] **Verb seam** (`LoweringTarget`) — the core-method → driver-op mechanism; ~45 seams,
  native-or-portable-default + bit-exact cross-check. This is the built, working half.
- [x] **Noun seam** (resource provider) — core datastructure → per-backend representation
  (build-from-description, not convert-between-builts). First-class as `CajetaNounProvider`
  (`runtime/native/cajeta_runtime.c`): a per-backend struct of build/free hooks (the runtime
  mirror of the verb seam), dogfooded on the seam-defining `AccelerationStructure` noun. Its
  built impl is now a **recorded property** of the noun (`CajetaAsImpl` in
  `cajeta_noun_impl.h`, stored as `AccelerationStructure.impl`), so the verb follows the noun,
  not the active backend: free dispatches on the recorded impl and the Vulkan launch asserts
  it matches the compiled verb path. The compile-time face is `LoweringTarget::accelImpl()`
  (the old `softwareRayQuery()` now derives from it — one source). Behavior-preserving
  device-verified CPU↔Vulkan. Buffer/Texture/Image keep their existing dispatch (one impl per
  backend — no tag is meaningful); their provider slots are reserved. **One backend, either
  impl** now works: see the capability override below.
- [~] **Capability heuristic + `Device.supports(...)` + override** — `Device.supports(Capability)`
  built (host query of the active device; `Capability.RayQueryNative`; `__cajeta_xpu_device_supports`;
  device-verified CPU false / Vulkan-RT true). **The explicit override + the execution mechanism
  are built (inc-4 brick #3):** an `AsImpl {Auto, Software, Native}` enum + `AccelerationStructure.of(...)`
  factory (default ctor = `Auto`) + a `CAJETA_GPU_AS_IMPL=software|native` env override (env wins),
  resolved once by `caj_resolve_as_impl`. Forced-software on a ray-query-capable GPU genuinely
  **runs**: the provider builds a software BVH into a storage buffer, the compiler emits a second
  `"<name>$sw"` kernel variant (the `SoftwareRayQuery` walk in plain SPIR-V via `SpirvSoftwareTarget`;
  needed a CFG-structurization pass in the SPIR-V emit pipeline + `shaderInt8`), and the launch
  selects the variant + buffer-binds per the recorded impl. Proven by a three-way equality
  (Vulkan-native == Vulkan-forced-software == CPU-software; `ToffeeSpatialIndexDeviceTests`).
  Still to do: the *automatic* density/extent heuristic (the deciding factor — query radius — is
  not known at AS build time, so the auto-policy stays "native if supported" for now);
  triangle-software-on-Vulkan; more capabilities.
- [~] **Impl-layer + SPIR-V degrade framework** — the "SDK for writing SDKs": a vendor authors
  verb + native lowering + a vendor-supplied SPIR-V fallback, using the same seam machinery
  core uses. **The internal face is now named + dogfooded (inc-4 brick #4):** one `ImplTier
  { Native, Portable }` concept (`LoweringTarget.h`) that both core degrade features answer
  through — the coop-matrix verb (`coopMatrixTier`) and the ray-query verb (`rayQueryTier`,
  derived from the AS noun's recorded `NounImpl`) — plus a generic `CAJETA_GPU_<FEATURE>_IMPL`
  override (`resolveImplTier`), proven on a **second** consumer beyond the AS noun:
  `CAJETA_GPU_COOPMATRIX_IMPL=software` forces the portable tile on a native-capable device,
  device-verified bit-exact (`ImplTierOverride.forcedSoftwareCoopMatrixOnDevice`). *Honest
  scope:* Native and Portable are different algorithms (not bit-identical for arithmetic
  features) — each is validated against the reference, not against the other. **Still seed**
  (the external half): the vendor-facing declaration syntax for verb + lowering + degrade,
  AOT/JIT impl-layer packaging, and the signing/sandbox model — recorded in
  [`../../cajeta-docs/gpu/VendorExtensionSDK.md`](../../cajeta-docs/gpu/VendorExtensionSDK.md)
  (crystallizes now that core has dogfooded the seams).
- [x] **`cajeta.xpu.core` → `cajeta.gpu.core` rename** — the foundation moved to the `gpu`
  namespace: directory `runtime/src/cajeta/{xpu→gpu}/core`, all package/import spellings,
  the compiler's hardcoded detection strings, the stdlib-embed path, tests, samples, docs,
  and the sibling `cajeta-toffee` repo (in lockstep). Wholesale (the whole `core` package was
  foundation); future compute (`Tensor`, orchestration) will be `cajeta.xpu.*` built on
  `gpu.core`. Verified: full XPU/ray-query/foundation regression green post-rename.

---

## 2. Backends (doc §2 — how core lowers; *not* vendor libraries)

- [x] **CPU** — reference path + bit-exact oracle; the floor.
- [x] **Vulkan** — device-verified (RADV / Strix Halo); richest; new capability lands here first.
- [~] **AMD** — device-verified (gfx1151), incl. `Image2D` storage images
  (`__ockl_image_store_2D`/`load_2D` over a surface object). Gap: mipmaps code-complete but
  ROCm-driver-blocked on this APU.
- [~] **NVIDIA** — emit-only; storage images (`Image2D` store/load) now emit (`sust.b.2d`/
  `suld.b.2d`, ◐); remaining advanced seams (texture dims > 2D, native coop-matrix, bindless
  arrays) not overridden; **nothing run on real NV silicon**. Needs B5 (§4) + the CUDA surface
  runtime + the missing seam overrides.
- [ ] **Metal** — no backend at all. MoltenVK (Vulkan→Metal, most of core ~free) → native Metal
  (for what MoltenVK can't reach). Gated on Mac hardware.

---

## 3. Core surface (doc §3 verbs + §4 nouns)

### 3.1 Verbs — status

- [x] **Execution** — coordinates, barriers, grid-stride, dynamic shared, kernel ABI.
- [x] **Memory** — `bufferElementPtr`, `MemoryKind` (Device/Pinned/Unified), `Buffer.slice`.
  bindless `Buffer<T>[]` is CPU + Vulkan + **AMD** (device-verified gfx1151 — the
  launch device-copies the `[count, h…]` handle array; the default pointer lowering
  flat-loads each handle). `[~]` NV open (default path inherits; CUDA runtime
  marshalling + emit pending B5).
- [x] **Value types & math** — `Vector`/`Matrix`/`Quaternion` (+ mask comparisons),
  transcendentals (AMD ocml), integer dot (DP4a), `Bits.*`, f32/f16/**bf16**.
- [x] **Textures** — 2D/3D/**1D/2DArray/Cube** + integer formats + mipmaps/LOD, device-verified
  on CPU + Vulkan + AMD. *(Drift fixed: the prior plan marked 1D/cube/array "open" — they ship.)*
- [x] **Capability primitives** — atomics (float/int/CAS), wave (shuffle/ballot/reduce/scan/
  rotate/laneId), quad, **cooperative matrix** (WMMA on AMD, native on Vulkan), shader clock.
- [~] **`Image2D` storage** (`storeImage`/`loadImage`) — Vulkan **+ AMD** (surface objects,
  device-verified on gfx1151) **+ CPU** (host float store, the oracle) device-verified; **NVIDIA
  emit-only** (`sust.b.2d`/`suld.b.2d`, ◐— device run needs the B5 CUDA surface runtime).
- [ ] **fp8 / E4M3 / E5M2** element type — blocked upstream (no LLVM backend type).

### 3.2 Nouns

- [x] `Buffer<T>` — alloc/upload/download, kinds, contiguous slice.
- [x] `Texture2D` / `Image2D` handles.
- [x] `AccelerationStructure` — AABB **and** triangle geometry on both the Vulkan native
  BLAS and the portable software BVH (CPU); no longer Vulkan-locked or AABB-only (§3.3).
  TLAS / instancing is the deferred next axis.

### 3.3 Ray query → genuinely core  ← **DONE (inc 1–3)**

**Design of record:** [`cajeta-docs/gpu/RayQuery.md`](../../cajeta-docs/gpu/RayQuery.md) —
the portable BVH noun + software traversal, the tier model (CoopMatrix `Native`/`Software`
shape), the layout contract, and the sequenced increments. The checklist below mirrors it.

Scope (doc §4): **inline ray query over BOTH triangle meshes and AABB/procedural BVHs** —
because scientific compute needs ray-triangle queries (mesh Monte-Carlo, SDF/curvature, ICP /
6-D pose, mesh-NN/fVDB) *and* the AABB path (3-D Gaussian, point clouds). Now genuinely core:
the full verb set (traverse, getters, confirm/generate, nearest-hit) runs on the portable
software BVH **and** native Vulkan, cross-checked CPU↔Vulkan, over both geometries.

- [x] **Vulkan hardware path** (the *acceleration*) — AABB BLAS build + `OpRayQuery`, device-
  verified on RADV; Toffee `SpatialIndex.countWithin` runs on it.
- [x] **Noun: portable software BVH** *(inc 1 — AABBs)* — median-split threaded BVH built
  into an all-`float32` block (`runtime/native/cajeta_bvh.c`); the software
  `AccelerationStructure` handle is that buffer. Triangles + LBVH/binned-SAH quality are
  follow-ups (inc 2 / 4). *Makes the AABB noun core.*
- [x] **Verb: portable software traversal** *(inc 1 — AABB slab)* — stackless threaded walk
  in cajeta (`SoftwareRayQuery.step`/`slabHit`, `SwRayCursor`); the call site lowers each
  `RayQuery` op to it on a software backend. **Möller-Trumbore** triangle leaves are inc 2.
- [x] **Noun-impl → verb-lowering coupling** — `LoweringTarget.softwareRayQuery()` per
  backend (CPU software + AS-as-buffer; Vulkan native `OpRayQuery`). Device-verified:
  `ToffeeSpatialIndexDeviceTests.*CpuSoftwareBvh` (777/888) match the Vulkan path.
- [x] **Triangle geometry** *(inc 2)* — software BVH triangle leaves (Möller-Trumbore,
  `SoftwareRayQuery.triangleHit` over a primData region) + `VK_GEOMETRY_TYPE_TRIANGLES_KHR`
  on the Vulkan build (`cajeta_xpu_vk_accel_build_triangles`, non-opaque). Device-verified:
  `triangleRayQueryOnCpuSoftwareBvh` == `triangleRayQueryOnDevice`; builder MT vs brute force.
- [x] **Verb getters + commit** *(inc 3)* — `candidate/committed Distance` (t), barycentrics,
  `frontFace`, `candidate/committedPrimitiveIndex`; **`confirm` / `generate` intersection** +
  committed nearest-hit. Software (cursor + tMax-shrink) and native (new `llvm.spv.ray.query.*`
  fork intrinsics + `OpRayQueryGetIntersection{T,Barycentrics,FrontFace}KHR` /
  `{Confirm,Generate}IntersectionKHR`), cross-checked CPU↔Vulkan. *Caveat: confirm before
  reading front-face / counting non-opaque triangle hits — unconfirmed candidate enumeration
  is non-deterministic on RADV.*
- [⊘] **TLAS / instancing** — deferred; the next core AS axis (ICP/pose will want instance
  transforms — a "soon," not a "never"). *Not* gfx.
- [⊘] **Ray-tracing pipeline** (raygen/closesthit/miss + SBT) — **gfx**, not core.

---

## 4. Infrastructure

- [x] **C0 — LLVM fork + prebuilt artifact** (`cajeta-spirv`; ray-query + coop-matrix ENABLED).
  `x86_64-linux-gnu` consumes the fork artifact + CI-consume wired.
- [~] **Upstream-first PRs** — 5 filed against `llvm/llvm-project` (ray-query, coop-matrix×2,
  merge-placement, global-array) + atomic-users fix; fork carries the rest (clock, subgroup-
  rotate, storage-image, quad) until they land. Live tracker: fork `UPSTREAM-PRS.md`.
- [ ] **B5 — NVIDIA runner** (x86-64 Windows + NVIDIA via WSL2 + CUDA-on-WSL) — the only NV
  hardware; unblocks the 5 skipped NV exec tests + the full NV on-device re-evaluation.
- [ ] **Remaining artifact legs** — `aarch64-linux`, macOS, Windows (distro LLVM ≤22 can't
  lower 23-only features); deferred until needed.

---

## 5. Explicitly out of scope (so it stops creeping back in)

- **Vendor libraries** — `nvidia`/`amd`/`metal` exclusive verbs (cooperative vector, TMA, MFMA,
  DPP, simdgroup-matrix, MPS, CUDA/ROCm interop). External, signed, on `olla.cajeta.dev`.
- **Ray-tracing pipeline** (SBT, hit/miss/raygen shaders) — gfx.
- **Triangle *rendering* surface** beyond intersection (material binding, shading) — gfx.
- **TLAS / instancing** — deferred (next core AS axis, §3.3).
- **Strided / non-contiguous buffer views** — call-site stride is the chosen idiom.
- **fp8** — blocked on upstream LLVM type support.

---

## 6. Definition of done

Core's contract is closed when **doc §3's verb set is ● / ○ on every shipped backend** and
**every core noun has a portable build** (no `[~]`/`[ ]` left unaccepted). Concretely:

1. ~~**Ray query is genuinely core**~~ ✅ **DONE (inc 1–3)** — software BVH + traversal over
   triangles + AABBs, full getters, confirm/generate, nearest-hit, cross-checked CPU↔Vulkan
   (§3.3). *Was the single biggest item.* (TLAS/instancing deferred.)
2. ~~**AMD `Image2D`** store/load~~ ✅ (surface objects, device-verified gfx1151) + ~~CPU
   `Image2D`~~ ✅ (host float store, the oracle) + ~~NVIDIA `Image2D` emit~~ ✅ (◐, `sust/suld.b.2d`);
   NVIDIA advanced seams + B5 on-device (incl. the NV storage-image *device* run + CUDA surface
   runtime); **Metal** backend.
3. **The model plumbing** — ~~noun seam~~ ✅ (§1), `Device.supports(...)` ✅ + ~~the impl
   override + execution mechanism~~ ✅ (`AsImpl`/`.of` + `CAJETA_GPU_AS_IMPL` + the `$sw`
   variant; the *automatic* density/extent heuristic still remaining), ~~the **internal**
   impl-layer/degrade seam~~ ✅ (named `ImplTier` + generic `CAJETA_GPU_<FEATURE>_IMPL` override,
   dogfooded across coop-matrix + ray-query) — the **external** vendor SPI of that framework
   still seed, the ~~`cajeta.xpu.core → cajeta.gpu.core` rename~~ ✅.
4. **fp8** when LLVM lands the type.

The foundation is then the **frozen dependency contract** `cajeta-xpu` and `cajeta-gfx` target.
