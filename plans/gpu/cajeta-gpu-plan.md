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
- [ ] **Noun seam** (resource provider) — core datastructure → per-backend representation
  (build-from-description, not convert-between-builts). Does not exist as a first-class seam;
  ray query forces it (§3.3).
- [ ] **Capability heuristic + `Device.supports(...)`** — runtime impl selection with explicit
  override (default ≠ law: RT loses at large radius / extreme density).
- [ ] **Impl-layer + SPIR-V degrade framework** — the "SDK for writing SDKs": a vendor authors
  verb + native lowering + a vendor-supplied SPIR-V fallback, using the same seam machinery
  core uses. The unlock that makes vendor libraries portable instead of hard-locked. Shape
  recorded in [`../../cajeta-docs/gpu/VendorExtensionSDK.md`](../../cajeta-docs/gpu/VendorExtensionSDK.md)
  (seed; crystallizes after core dogfoods the seams).
- [ ] **`cajeta.xpu.core` → `cajeta.gpu.core` rename** (~120 spellings) — foundation classes
  move to the `gpu` namespace; `xpu` keeps compute-execution (kernels, `Tensor`). See the
  gpu⟂xpu code-move guidance before relocating files.

---

## 2. Backends (doc §2 — how core lowers; *not* vendor libraries)

- [x] **CPU** — reference path + bit-exact oracle; the floor.
- [x] **Vulkan** — device-verified (RADV / Strix Halo); richest; new capability lands here first.
- [~] **AMD** — device-verified (gfx1151). Gaps: `Image2D` store/load not wired; mipmaps
  code-complete but ROCm-driver-blocked on this APU.
- [~] **NVIDIA** — emit-only; advanced seams (texture dims > 2D, native coop-matrix, images,
  bindless arrays) not overridden; **nothing run on real NV silicon**. Needs B5 (§4) + the
  missing seam overrides.
- [ ] **Metal** — no backend at all. MoltenVK (Vulkan→Metal, most of core ~free) → native Metal
  (for what MoltenVK can't reach). Gated on Mac hardware.

---

## 3. Core surface (doc §3 verbs + §4 nouns)

### 3.1 Verbs — status

- [x] **Execution** — coordinates, barriers, grid-stride, dynamic shared, kernel ABI.
- [x] **Memory** — `bufferElementPtr`, `MemoryKind` (Device/Pinned/Unified), `Buffer.slice`.
  `[~]` bindless `Buffer<T>[]` is CPU + Vulkan only (AMD/NV open).
- [x] **Value types & math** — `Vector`/`Matrix`/`Quaternion` (+ mask comparisons),
  transcendentals (AMD ocml), integer dot (DP4a), `Bits.*`, f32/f16/**bf16**.
- [x] **Textures** — 2D/3D/**1D/2DArray/Cube** + integer formats + mipmaps/LOD, device-verified
  on CPU + Vulkan + AMD. *(Drift fixed: the prior plan marked 1D/cube/array "open" — they ship.)*
- [x] **Capability primitives** — atomics (float/int/CAS), wave (shuffle/ballot/reduce/scan/
  rotate/laneId), quad, **cooperative matrix** (WMMA on AMD, native on Vulkan), shader clock.
- [~] **`Image2D` storage** (`storeImage`/`loadImage`) — Vulkan only; AMD/CPU/NV open.
- [ ] **fp8 / E4M3 / E5M2** element type — blocked upstream (no LLVM backend type).

### 3.2 Nouns

- [x] `Buffer<T>` — alloc/upload/download, kinds, contiguous slice.
- [x] `Texture2D` / `Image2D` handles.
- [~] `AccelerationStructure` — exists but **Vulkan-locked and AABB-only** (see §3.3).

### 3.3 Ray query → genuinely core  ← **the headline open item**

**Design of record:** [`cajeta-docs/gpu/RayQuery.md`](../../cajeta-docs/gpu/RayQuery.md) —
the portable BVH noun + software traversal, the tier model (CoopMatrix `Native`/`Software`
shape), the layout contract, and the sequenced increments. The checklist below mirrors it.

Scope (doc §4): **inline ray query over BOTH triangle meshes and AABB/procedural BVHs** —
because scientific compute needs ray-triangle queries (mesh Monte-Carlo, SDF/curvature, ICP /
6-D pose, mesh-NN/fVDB) *and* the AABB path (3-D Gaussian, point clouds). Today it is Vulkan +
AABB only, so it is **not core** — "core intent" without a portable, both-geometry path is the
badge-without-substance the model forbids.

- [x] **Vulkan hardware path** (the *acceleration*) — AABB BLAS build + `OpRayQuery`, device-
  verified on RADV; Prism `SpatialIndex.countWithin` runs on it.
- [x] **Noun: portable software BVH** *(inc 1 — AABBs)* — median-split threaded BVH built
  into an all-`float32` block (`runtime/native/cajeta_bvh.c`); the software
  `AccelerationStructure` handle is that buffer. Triangles + LBVH/binned-SAH quality are
  follow-ups (inc 2 / 4). *Makes the AABB noun core.*
- [x] **Verb: portable software traversal** *(inc 1 — AABB slab)* — stackless threaded walk
  in cajeta (`SoftwareRayQuery.step`/`slabHit`, `SwRayCursor`); the call site lowers each
  `RayQuery` op to it on a software backend. **Möller-Trumbore** triangle leaves are inc 2.
- [x] **Noun-impl → verb-lowering coupling** — `LoweringTarget.softwareRayQuery()` per
  backend (CPU software + AS-as-buffer; Vulkan native `OpRayQuery`). Device-verified:
  `PrismSpatialIndexDeviceTests.*CpuSoftwareBvh` (777/888) match the Vulkan path.
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

1. **Ray query is genuinely core** — software BVH + traversal over triangles + AABBs, full
   getters, confirm/generate (§3.3). *The single biggest item.*
2. **AMD `Image2D`** store/load; NVIDIA advanced seams + B5 on-device; **Metal** backend.
3. **The model plumbing** — noun seam, `Device.supports(...)` + capability heuristic, the
   impl-layer/SPIR-V-degrade framework, the `cajeta.xpu.core → cajeta.gpu.core` rename.
4. **fp8** when LLVM lands the type.

The foundation is then the **frozen dependency contract** `cajeta-xpu` and `cajeta-gfx` target.
