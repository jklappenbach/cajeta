# Ray query → core: the portable BVH noun + software traversal

**Status: software path (Inc 1–3) SHIPPED; native Vulkan RT-core path on-device-validated
(RADV + RTX 4090); NVIDIA OptiX RT-core verb (Inc 5, M0–M2) validated end-to-end on the
RTX 4090; Inc 4 auto-selection heuristic the remaining open item.**
This is the design for making inline ray query a genuine `cajeta.xpu` feature,
one that runs on every backend and not only Vulkan. The portable software BVH +
stackless traversal (AABBs, triangles, full candidate/committed getters + commit) is
built and cross-checked against the Vulkan native path (§8). What remains is the
automatic density/extent selection heuristic of Inc 4.

> **Native on-device (2026-06-16).** The native Vulkan path (`OpRayQuery` over a
> BLAS+TLAS, on the RT cores) is now validated on the RTX 4090 on Windows as well
> as RADV on Linux. AUTO resolves to native on any ray-query-capable Vulkan device.
> The `caj_native_rayquery_available` resolver and `Device.supports(RayQueryNative)`
> share one condition, so they never disagree. The impl is selectable per AS via
> `AccelerationStructure.of(.., AsImpl.Native | AsImpl.Software)` or the process-wide
> `CAJETA_GPU_AS_IMPL=native|software` override, and the chosen impl is recorded on
> the noun and readable via `AccelerationStructure.implTag()` (0 = software BVH,
> 1 = native BLAS, 2 = OptiX). The proof is `autoRecordsNativeImplOnDevice` (AUTO records
> native on the 4090) + `forcedNativeOfApiOnDevice`/`forcedSoftwareOfApiOnDevice` in
> `CarameloSpatialIndexDeviceTests`. AMD's HIP backend still uses the software BVH.

> **NVIDIA CUDA — OptiX RT-core tier (verb validated end-to-end, 2026-06-17).**
> NVIDIA's RT cores are reached via OptiX, which is a pipeline model rather than inline
> ray query, since there is no NVVM inline-RQ intrinsic. So the CUDA backend gets a third
> AS impl, `CAJ_AS_IMPL_OPTIX` (2). M0 (`OptiXRayQueryProbe`) proved OptiX RT-core results
> match the software oracle on the 4090. M1 landed the runtime AS provider
> (`src/cajeta/xpu/nvidia/OptixAccel.cpp` + the CUDA noun provider's OptiX arm). M2
> now lands the verb through the full compiler. A `RayQuery` `@Kernel` against an
> OptiX-impl AS is lowered (NvptxRegistration + `NvptxOptixRayQuery`) into a SEPARATE
> OptiX program-PTX module, because ptxas rejects the `_optix_*` asm, so it never goes
> through the cubin. It is dispatched via `optixLaunch` in place of `cuLaunchKernel`.
> Four canonical shapes run on the RT cores end-to-end, each matching the software
> oracle (777) on the 4090. They are AABB candidate-count (raygen + custom-prim
> intersection + anyhit-counts-and-ignores + miss; `aabbCountRayQueryOnOptixDevice`),
> triangle nearest-hit (built-in triangle, raygen + closesthit commits T/type/prim;
> `nearestHitRayQueryOnOptixDevice`), triangle candidate getters (built-in triangle,
> anyhit reads `optixGetRayTmax` + `optixGetTriangleBarycentrics` then ignores;
> `candidateGettersRayQueryOnOptixDevice`), and committed-triangle per-launch (a
> per-launch dynamic ray resolved from `initialize()` as constants or `buffer[i]` loads,
> + closesthit writing a hit-flag or front-face; `{triangleCount,frontFace}RayQueryOnOptixDevice`).
> A ray-query kernel outside these canonical shapes throws XPU-N04 at registration and
> keeps its software cubin, so there is never a silent miscompile. The AS-impl policy is
> that AUTO on CUDA stays on the software-BVH floor, and OptiX is opt-in via
> `CAJETA_GPU_AS_IMPL=optix`. The AUTO→OptiX flip is deferred, because the impl is
> resolved before the consumer kernel is known, so auto-routing an Unsupported shape onto
> an OptiX AS would fault. The `RayQueryRtCore` capability (`Device.supports`) reports the
> RT-core path per device (true iff CUDA + OptiX engine loaded). `RayQueryNative` stays
> false on CUDA since OptiX has no inline RQ. OptiX is a compile-time-only dependency
> (header-only SDK; the engine is the driver's `nvoptix.dll`). See
> `documents/gpu-rayquery-optix/`. It is the
headline item of the GPU foundation
(`plans/gpu/cajeta-gpu-plan.md §3.3`) and the
worked example of the model in [`CajetaGPU.md §1` / `§4`](../CajetaGPU.md). Read those
first for the why. This doc is the what and the how, including the BVH noun.

Ray query is the keystone because it is the first core feature that needs both
seams at once, a verb (traverse a ray) and a noun (the acceleration structure the
ray traverses). The verb alone is easy, since it is a handful of ops. The noun is the work,
and building it is what forces the noun seam into existence as a first-class part
of the foundation. Everything else in the model plumbing (`Device.supports`, the
capability heuristic, the impl-layer/degrade framework) follows the shape ray query
establishes here.

---

## 1. Where it started (the pre-work baseline)

> **Note:** this section is the starting baseline that motivated the work. Inc 1–3
> (§8) have since closed it. Software ray query now runs on every backend, and XPU-N02
> is no longer thrown for ray query. It survives only as the diagnostic for the native
> `OpRayQuery` path on a non-Vulkan backend, which the software default pre-empts.

- **Verb.** `RayQuery` (`runtime/.../core/RayQuery.cajeta`) is a device-only kernel-local
  cursor. v1 ops = `initialize` / `proceed` / `committedType` / `candidateType` /
  `candidatePrimitiveIndex`. It lowers only on Vulkan, via the
  `llvm.spv.ray.query.*` intrinsics → `OpRayQuery*KHR`. Every other backend's seam
  throws XPU-N02 ("SPV_KHR_ray_query is a Vulkan-only extension").
- **Noun.** `AccelerationStructure` (`runtime/.../core/AccelerationStructure.cajeta`) is
  a host handle over a Vulkan BLAS, AABB-only, opaque, with no TLAS / triangles /
  refit. It is built by `__cajeta_xpu_accel_build_aabbs`, which dispatches only to
  `cajeta_xpu_vk_accel_build_aabbs` (`CAJ_XPU_VULKAN`), and any other backend returns
  handle 0.
- **Consumer.** Caramelo `SpatialIndex.countWithin` / `radiusExact`
  (`CarameloSpatialIndexDeviceTests`) is the device-verified proof the path works, and it
  is gated `if (!VulkanDriver::rayQueryAvailable()) SKIP`.

So today ray query carries core intent with a Vulkan-only reality. That gap, a
core verb that silently does nothing off Vulkan, is the badge-without-substance the
model forbids. Closing it is this work.

---

## 2. The tier model — exactly the CoopMatrix shape

`CooperativeMatrix` already solved "a core verb with a native fast path and a portable
fallback that runs everywhere". It is a static per-`(backend, dtype, shape)` choice between

- **Native**, the hardware op (Vulkan `SPV_KHR_cooperative_matrix`, AMD WMMA), and
- **Software**, a portable emission (flat tile + triple-loop) the DeviceLowerer emits
  inline. Software is the default, so every backend runs coop-matrix instead of throwing.

Ray query takes the same trichotomy, with the noun added:

| | **Native** | **Software** (new — this work) |
|---|---|---|
| **Noun** (`AccelerationStructure`) | Vulkan BLAS (`VK_KHR_acceleration_structure`) | a portable **BVH packed in a `KernelBuffer<T>`** |
| **Verb** (`RayQuery` ops) | `OpRayQuery*KHR` | a portable **stackless traversal** the lowerer emits over the BVH buffer |
| **Default** | where the driver advertises ray query | **everywhere** (the floor — CPU, and any GPU without native RQ) |

Two changes from the CoopMatrix precedent:

1. **The choice is coupled across the seam.** With coop-matrix the verb tier is read
   off `(dtype, shape)`. With ray query the verb lowering is determined by which noun
   impl was built. A software BVH buffer ⇒ software traversal, and a Vulkan AS ⇒
   `OpRayQuery`. The noun is selected once at build time (by the capability heuristic,
   §6), and the verb follows. This is "the noun's chosen implementation determines the
   verb's lowering" from [`CajetaGPU.md §1.4`](../CajetaGPU.md).
2. **The default flips the failure mode.** Today the default seam throws. After this
   work the default seam emits the software traversal. A `RayQuery` in a kernel on
   CPU / AMD / NVIDIA then runs (over a software BVH) instead of failing. That is the same
   "Software default means it runs, not throws" rule CoopMatrix established.

---

## 3. The noun: a portable BVH packed in a `KernelBuffer<T>`

This is the substance. The deliverable is a build-from-description provider. It
consumes the geometry description (the AABBs and/or triangles a caller hands in) and
produces an opaque, device-resident BVH the traversal verb reads. Per the model, core
never transcodes a built Vulkan AS into a software BVH. Each impl builds from the
same description independently.

### 3.1 Layout (the contract between build and traversal)

The layout is flat, pointer-free, and GPU-friendly, in one `KernelBuffer<T>`, so it
uploads/binds like any other buffer, on any backend. There are two regions, both
`uint32`/`float32` words:

```
[ header | node[0..N) | primRef[0..P) | primData (aabbs and/or triangles) ]
```

- **header** holds the node count, prim count, root index, geometry-kind flags (has-AABBs,
  has-triangles), and offsets to each region.
- **node** is a compact BVH node, a child AABB (6×f32) + either child node indices
  (interior) or `[firstPrim, primCount]` into `primRef` (leaf). The layout is stackless
  (e.g. `escapeIndex`/`miss` links, or a left-child + skip pointer) so traversal needs
  no per-ray stack. That matters for register-starved device traversal and for the
  fixed-iteration walk the lowerer emits.
- **primRef** holds `uint32` indices remapping sorted leaf order → original primitive id,
  so `candidatePrimitiveIndex()` returns the caller's index, matching the Vulkan
  semantics the Caramelo exact-L2 test depends on.
- **primData** is the geometry itself, AABBs as 6×f32 and triangles as 3×(3×f32) (or an
  index buffer into a shared vertex buffer, TBD in inc 2).

The exact node encoding is the one real design decision inside the noun. It is fixed in
inc 1 and frozen, because both the builder and the separately-authored traversal
emit against it.

### 3.2 The builder

- **First builder: LBVH (radix / Morton).** It is linear, parallel-friendly, simple to make
  correct, and itself expressible as portable kernels, which dogfoods the foundation.
  The BVH for the software ray-query path is built by core kernels. The steps are compute
  centroid Morton codes → sort → build the radix tree → fit AABBs bottom-up.
- **Quality follow-up: binned-SAH.** It gives better-quality trees (fewer traversal steps) for
  static scenes, and the capability heuristic can pick it when build cost is amortized over
  many queries. It is the same layout with a different builder, and it slots in behind the
  same noun seam.
- **Build location.** The host-side C builder comes first (`__cajeta_xpu_bvh_build_*`, the
  `accel_build_aabbs` precedent), writing into a mapped `KernelBuffer<T>`, because it is the
  fastest path to a correct, cross-checkable noun. The on-device cajeta build kernel
  (the dogfooding ideal) is a follow-up once the layout and traversal are proven.
  Correctness comes first, and then the build moves onto the device.

### 3.3 Geometry: AABBs **and** triangles (both are core)

Both are in, deliberately, as the scope correction from the plan. Scientific compute needs
ray-triangle queries (mesh Monte-Carlo, SDF / curvature, ICP / 6-D pose,
mesh-NN / fVDB) and the AABB / procedural path (3-D Gaussian, point clouds). A core
BVH that only does AABBs would re-introduce the same half-feature gap one layer down.
AABBs land first, because they cross-check directly against today's Vulkan AABB path and the
existing Caramelo tests. Triangles follow in the same layout.

---

## 4. The verb: portable stackless traversal

When the noun is a software BVH buffer, the `RayQuery` seams lower to a portable
traversal instead of `OpRayQuery*KHR`, emitted the way Software coop-matrix emits its
triple-loop:

- **`rayQueryType`** is no longer "unsupported". It returns a concrete software cursor
  struct, holding the ray (origin/dir, tMin/tMax), the BVH buffer base pointer, the current
  traversal node, and the current/committed candidate record (prim index, t,
  barycentrics, kind). This is the alloca the kernel keeps on its stack.
- **`rayQueryInitialize`** stores the ray + BVH pointer into the cursor and seeds the
  traversal at the root.
- **`rayQueryProceed`** advances the stackless walk to the next candidate leaf. It
  descends, slab-tests child AABBs, and at a leaf runs the leaf intersection, which is
  Möller-Trumbore for triangle leaves and the slab / point-in-box test for AABB
  leaves, recording the candidate. It returns `true` while a candidate remains.
- **`rayQueryIntersectionType`** / **`...PrimitiveIndex`** read straight off the
  cursor's candidate/committed record (`primRef`-remapped to the caller's index).
- **New getters (inc 3):** `t` (distance), barycentrics, frontFace, and `confirm` /
  `generate` intersection. The AABB path can only count without these, since it cannot
  commit a nearest hit. These are the same getters the Vulkan path will need exposed,
  so they are added to the seam set once and lowered both ways.

The walk is a bounded-iteration loop, since the stackless layout makes the bound the tree
depth rather than an unbounded stack, so it lowers cleanly on every backend without dynamic
allocation.

---

## 5. The coupling: noun impl → verb lowering, chosen once

This is the single mechanism the noun seam introduces. At AccelerationStructure build time
the capability heuristic picks an impl (§6). That choice is recorded on the noun and is
what the `RayQuery` seam consults when lowering the verb:

```
build AS  ──heuristic──▶  impl = { Vulkan-AS | software-BVH }
                              │
RayQuery ops in a kernel  ◀───┘   verb lowering = impl's traversal
                                  (OpRayQuery*KHR  |  software walk)
```

For AOT binaries the user may pin the impl layers compiled in. For JIT (`.cja`)
the available impls are whatever is on the classpath, with software always built in
as the floor. Default ≠ law, so the heuristic is overridable. A software BVH can beat a
native AS at large query radius / extreme density, which is the exact RT-vs-grid tradeoff the
override exists for.

**M3 evolution — the multi-impl noun + launch-time selection (CUDA/OptiX).** The
"chosen once at build" coupling above is the single-impl degenerate case. On CUDA the
OptiX RT-core path needed a stronger model. Whether a kernel can traverse a given impl is
a property of the (kernel-shape, impl) pair, and that is known only at launch rather than
at build. So an `AccelerationStructure` is now a multi-impl noun. It always retains the
software BVH as a floor and may additionally carry the device-native rep (OptiX `OptixAs`
on CUDA). Impl selection moved to launch time, so the runtime picks, per consuming kernel,
the best impl that kernel can actually use. A registered OptiX program + the AS carrying the
OptiX rep goes to `optixLaunch`, and otherwise the kernel runs the software cubin over the
retained floor. No kernel ever receives a rep it cannot traverse. Vulkan rides the same
model as the degenerate case, with one rep per AS, and the launch picks the `$sw` vs native
variant by the recorded impl (§3, `CajetaXPU-Matrix.md` §3). See
`documents/gpu-rayquery-optix/rayquery-optix-m3-multiimpl-{spec,plan}.md`.

---

## 6. Selection heuristic + `Device.supports`

- `Device.supports(Capability.RayQueryNative)` asks whether the bound device advertises native
  inline ray query (VK_KHR_ray_query or equivalent). It is true on a ray-query Vulkan
  device and false on CUDA, because OptiX has no inline RQ and its RT cores are pipeline-only.
  It drives the default noun-impl choice and is user-overridable.
- `Device.supports(Capability.RayQueryRtCore)` asks whether the bound device advertises the
  pipeline-based RT-core path (NVIDIA OptiX). It is true iff the active backend is CUDA
  and the OptiX engine (`nvoptix.dll`) loaded. This is the heuristic input an app uses
  to choose the OptiX opt-in path on CUDA. The two capabilities are distinct because the
  CUDA RT-core path is reached through an `optixLaunch` pipeline rather than an inline op.
- The heuristic inputs are device support, geometry kind, primitive count, and expected query
  count / radius (build cost amortization). The choice is native when advertised and the workload
  suits it, and software otherwise. Software is always a valid answer and never a
  failure.
- **On CUDA, AUTO now prefers the OptiX RT-core tier (M3).** The build records the
  software floor as the primary, and the OptiX rep is built lazily on the first
  supported-shape launch, so an AS consumed only by software kernels never pays for OptiX.
  A supported-shape kernel then runs on the RT cores, while an Unsupported-shape kernel
  transparently falls back to the retained software floor. That is the launch-time selection in
  §5, with no fault and no opt-in. The earlier opt-in-only policy (AUTO-stays-software, deferred
  flip) is superseded, because launch-time selection eliminated the fault that made it unsafe.
  `CAJETA_GPU_AS_IMPL` still forces a tier, `=software` (floor only) or `=optix` (eager OptiX
  primary + retained floor). This is verified on the 4090, where all four canonical OptiX shapes
  run on RT cores under AUTO and match the software oracle.

This is the first concrete use of `Device.supports` + the capability heuristic from the
foundation plan §1. Ray query is what makes them real rather than speculative.

---

## 7. Cross-check: the correctness guarantee

The model's correctness rule is a bit/result-compatible cross-check between a portable
path and a native path. Ray query already has the harness:

- `CarameloSpatialIndexDeviceTests.fixedRadiusCountOnDevice` and `exactL2RefinementOnDevice`
  run `countWithin` / `radiusExact` on the Vulkan native path today.
- After inc 1 these same tests run on the CPU software path, with the same source and the same
  results (777 / 888), un-gated from `rayQueryAvailable()` for the software leg. A
  software/native agreement test (same scene, both impls, identical neighbour counts)
  is the acceptance criterion for each increment.

When the software AABB path matches the Vulkan AABB path on the existing Caramelo scenes,
the AABB half of "genuinely core" is proven rather than asserted. Triangles get an analogous
mesh cross-check.

---

## 8. Plan — sequenced increments

Legend: `[ ]` not started · `[~]` partial · `[x]` done.

**Inc 1 — Software AABB BVH + traversal (the seam-proving slice). ✅ DONE.**
- [x] Freeze the BVH buffer layout (§3.1). It is an all-`float32` block, threaded depth-first
      nodes + primRef (`runtime/native/cajeta_bvh.c`). Indices are stored as exact floats and
      read with `(uint32)`, so no reinterpret primitive is needed, with a <2^24 limit.
- [x] Host-side **median-split** builder over AABBs → blob (`cajeta_xpu_cpu_accel_build_aabbs`,
      wired into `__cajeta_xpu_accel_build_aabbs` CPU case). *LBVH is the quality
      follow-up; median-split is correct and sufficient for the cross-check.*
- [x] `AccelerationStructure` software impl. The CPU handle is the host blob pointer
      (the CPU buffer convention), and `_free` = `free()`.
- [x] Software cursor (`SwRayCursor` `@ValueType`) + the traversal in cajeta
      (`SoftwareRayQuery.step`/`slabHit`); call-site lowering builds the cursor
      (insertvalue), calls `step` with writeback, reads candidate fields.
      `initialize`/`proceed`/`committedType`/`candidateType`/`candidatePrimitiveIndex`.
- [x] Noun-impl → verb-lowering coupling (§5), per-backend, via `LoweringTarget.softwareRayQuery()`
      (CPU true → software walk + AS-as-buffer; Vulkan false → native `OpRayQuery`).
- [x] **Cross-check:** `CarameloSpatialIndexDeviceTests` `fixedRadius` (777) + `exactL2` (888)
      pass on the **CPU** backend, matching the Vulkan native path; plus a self-contained
      `minimalRayQueryOnCpuSoftwareBvh` and the `SoftwareBvhBuilderTests` noun unit tests.
- Incidental fixes this slice surfaced (pre-existing, off-path): `KernelBuffer.upload/downloadAsync`
  class-param-field idiom; `Quad` host @Native stubs (missing since the quad commit — it
  also un-broke the Vulkan native Caramelo test); boolean literals in the device lowerer.

**Inc 2 — Triangles. ✅ DONE.**
- [x] Triangle geometry in the layout + builder — a primData region (9 floats/tri) +
      a TRIANGLES geometry flag (`cajeta_xpu_cpu_accel_build_triangles`); a distinct-arity
      3-arg `AccelerationStructure(vertices, triCount, vertexStride)` ctor selects it.
- [x] Möller-Trumbore leaf test in the walk — `SoftwareRayQuery.step` branches on the
      geometry flag; `triangleHit` is the MT intersection; triangle candidates are
      `candidateType() == 0`.
- [x] `VK_GEOMETRY_TYPE_TRIANGLES_KHR` on the Vulkan build (`cajeta_xpu_vk_accel_build_triangles`,
      non-opaque so candidates enumerate in `proceed()`, matching the software model).
- [x] **Mesh cross-check:** `triangleRayQueryOnCpuSoftwareBvh` (software MT) and
      `triangleRayQueryOnDevice` (Vulkan hardware RT) agree (777); plus
      `SoftwareBvhBuilderTests.triangleMesh/triangleCloud` vs a brute-force MT oracle.

**Inc 3 — Full getters + commit.**
- [x] *(3a)* **Candidate getters — software.** `candidateDistance` / `candidateBarycentricU`
      / `candidateBarycentricV`. `step` inlines Möller-Trumbore to capture t/u/v into the
      cursor, and the getters read those fields. This enables nearest-hit-by-user-min (the RTNN
      pattern). Verified by `candidateGettersOnCpuSoftwareBvh` (t=5, u=v=0.25).
- [x] *(3b software)* **`confirm` / `generate` + committed getters — software.**
      `confirmIntersection` (triangle) / `generateIntersection(t)` (AABB) copy the current
      candidate into the cursor's committed slot and shrink `tMax` to the hit distance,
      so the remaining walk only finds closer hits. The committed hit ends up the nearest,
      independent of traversal order. `committedType`/`committedDistance`/`committedBarycentricU`
      /`V`/`committedPrimitiveIndex` read it. Verified by `nearestHitOnCpuSoftwareBvh`, where of two
      stacked triangles the nearest (prim 1, t=6) wins.
- [x] *(3b native)* **Native getters + confirm/generate**: added the
      `llvm.spv.ray.query.*` fork intrinsics + opcodes `OpRayQueryGetIntersection{T,
      Barycentrics,FrontFace}KHR` / `OpRayQuery{Confirm,Generate}IntersectionKHR` (cajeta-llvm,
      `cajeta-spirv`) + `SpirvTarget` seams. Cross-checked: `candidateGettersOnDevice`,
      `nearestHitOnDevice`, `triangleRayQueryOnDevice`, `frontFaceOnDevice` match the
      software path.
- [x] *(3b)* `frontFace` getter. Software is the MT det-sign (det>0 = front, the CCW convention),
      and native is `OpRayQueryGetIntersectionFrontFaceKHR`. Use the committed front-face (after
      confirm), because reading an unconfirmed candidate's front-face is non-deterministic on RADV.
      The same holds for counting unconfirmed non-opaque triangle candidates, so confirm first. The
      software path is reliable for candidate + committed.

**Inc 4 — Plumbing hardening (shared with the rest of the model).**
- [~] `Device.supports(RayQueryNative)` + the selection heuristic with override (§6).
      *Done: `Device.supports(Capability.RayQueryNative)` + `RayQueryRtCore`
      (`Capability.cajeta`, `Device.cajeta`) + the manual `CAJETA_GPU_AS_IMPL` impl
      override (`AsImpl.cajeta`, `resolveImplTier`) + the `AccelerationStructure.implTag()`
      accessor so a caller can confirm which impl a build chose. AUTO/native are
      on-device-validated on the RTX 4090 (Windows), and the resolver and capability share
      one un-gated condition. Open: the automatic density/extent selection heuristic,
      and the AUTO→OptiX flip on CUDA (deferred; see Inc 5 + §6).*
- [ ] On-device cajeta LBVH build kernel (move build off the host; dogfood the seam).
- [x] Promote the noun seam to the first-class SPI the VendorExtensionSDK seed needs. It is
      built as `CajetaNounProvider` (`runtime/native/cajeta_noun_impl.h`) and dogfooded on
      `AccelerationStructure`, where the verb follows the noun's recorded impl tag.

**Inc 5 — NVIDIA CUDA OptiX RT-core tier (the verb on a third silicon path). ✅ M0–M2 DONE.**
NVIDIA has no inline-RQ intrinsic, so the CUDA RT cores are reached through an OptiX
pipeline, as an IMPLEMENTATION of the same `cajeta.xpu` inline `RayQuery`
verb. The pipeline is internal and never user-authored (cf. §9). See
`documents/gpu-rayquery-optix/` for the spec/plan, with on-device proof on the RTX 4090.
- [x] **M0/M1.** OptiX RT-core ↔ software parity (`OptiXRayQueryProbe`) + the runtime AS
      provider (`OptixAccel.cpp`, `CAJ_AS_IMPL_OPTIX`=2, `optixRecordsImplOnNvptxDevice`).
- [x] **M2 verb codegen.** A `RayQuery` `@Kernel` against an OptiX-impl AS lowers
      (`NvptxOptixRayQuery` + `NvptxRegistration`) to a SEPARATE OptiX program-PTX module
      (ptxas rejects the `_optix_*` asm, so it bypasses the cubin) registered by kernel
      name + a shape tag, and the CUDA launch path dispatches `optixLaunch` instead of
      `cuLaunchKernel`. There are four canonical shapes, each matching the software oracle (777) on
      the 4090. They are AABB candidate-count (custom-prim intersection + anyhit-counts-ignores),
      triangle nearest-hit (built-in triangle + closesthit commits T/type/prim),
      triangle candidate getters (built-in triangle + anyhit reads `optixGetRayTmax` +
      `optixGetTriangleBarycentrics` then ignores), and committed-triangle per-launch
      (a per-launch dynamic ray, with each component resolved from `initialize()` as a constant
      or a `buffer[i]` load, + closesthit writing a hit-flag or front-face via
      `optixIsFrontFaceHit`; covers the triangle-count + front-face kernels). The tests are
      `XpuNvptxOptixEmitTests` (PTX emission, GPU-free) + `{aabbCount,nearestHit,
      candidateGetters,triangleCount,frontFace}RayQueryOnOptixDevice`. Any non-canonical
      shape → XPU-N04 at registration → keeps its software cubin (no silent miscompile).
- [x] **AS-impl policy + capability** — `Device.supports(Capability.RayQueryRtCore)`
      (true iff CUDA + OptiX engine); AUTO on CUDA stays software (`autoRecordsSoftware
      ImplOnNvptxDevice`), OptiX opt-in via `CAJETA_GPU_AS_IMPL=optix`. The AUTO→OptiX
      flip is deferred (the impl is resolved before the consumer kernel is known, so
      auto-routing an Unsupported shape would fault its software cubin on an OptiX handle).
- [ ] Broader OptiX shapes (non-const-ray getters, AABB generate-intersection, multi-ray
      committed getters) + then the AUTO→OptiX flip (or a per-compilation-unit
      "all-supported → flip" gate). *Front-face + per-launch dynamic rays now done (the
      committed-triangle shape); front-face winding matches cajeta's det>0 convention.*

**Quality follow-up (not gating "core"):**
- [ ] binned-SAH builder behind the same noun seam.

---

## 9. Out of scope (stays out)

- **TLAS / instancing** is deferred. It is the next core AS axis (ICP / pose will want it),
  and it belongs to core rather than gfx. (`plans §3.3`.)
- **Ray-tracing *pipeline*** (raygen / closesthit / miss + SBT) as a user-authored
  surface belongs to gfx rather than core. The GPU↔GFX seam is inline-ray-query (here) vs a
  user-facing RT-pipeline (there). The nuance is that the NVIDIA OptiX core impl (Inc 5) builds a
  raygen/anyhit/closesthit/intersection/miss pipeline INTERNALLY. That is a compiler
  lowering of the inline `RayQuery` verb, since no NVVM inline-RQ intrinsic exists, and it is
  no pipeline the user writes. It stays an implementation detail of core inline ray query.
- **Triangle *rendering*** beyond intersection (material binding, shading) belongs to gfx.
- **Vendor-exclusive RT** (NV OMM/DMM, cluster AS) belongs to external vendor libraries.

---

## 10. Rules

`cajeta.xpu` inline ray query runs on every backend. It is native `OpRayQuery*KHR`
over a Vulkan BLAS where the device advertises it, and a portable stackless BVH walk over a
`KernelBuffer<T>`-packed BVH (Möller-Trumbore for triangles, slab for AABBs) everywhere else.
That walk is the Software default, mirroring `CooperativeMatrix`. The noun impl is chosen once at
build time by the capability heuristic (overridable) and determines the verb lowering.
Software is always a valid answer and never a failure, so XPU-N02 stops being thrown for ray
query. Correctness is the software-vs-native cross-check on the existing Caramelo scenes.

## See also

- Foundation model — [`CajetaGPU.md §1` / `§4`](../CajetaGPU.md).
- Foundation plan — `../../plans/gpu/cajeta-gpu-plan.md §3.3`.
- The tier precedent — `CooperativeMatrix` (`runtime/.../core/CooperativeMatrix.cajeta`),
  `LoweringTarget.coopMatrixTier`.
- Vendor SPI that depends on this noun seam — [`VendorExtensionSDK.md`](../VendorExtensionSDK.md).
