# Cajeta GPU — the device foundation

`cajeta-gpu` is the **shared device foundation**: value types & math, the memory/buffer
model, textures & images, acceleration structures, and the per-backend lowering of every
portable device capability. Compute (`cajeta-xpu`) and graphics (`cajeta-gfx`) are
**siblings** built on it — gfx does not depend on xpu; both target the contract here.

```
cajeta.xpu                     (foundation — the shared portable contract)
   ▲                              ▲
cajeta.xpu.xpu             cajeta.xpu.gfx
(compute primitives)       (rendering pipeline)
```

All three are **stdlib**, nested in one namespace the way `java.nio` holds its buffers while
`java.nio.{channels,file,charset}` build on them: `cajeta.xpu` holds the shared core (the
classes both facets use), and `cajeta.xpu.xpu` / `cajeta.xpu.gfx` are the compute and graphics
facets that import it. The nesting groups the GPU tier and aids discovery; the "builds on"
relationship is carried by imports, not by the dots (a child never depends on its parent
*by virtue of* nesting — `gpu.xpu` depends on `gpu` because it imports it).

The GPU↔GFX seam is concrete, not stylistic: **GPU owns inline ray query — over meshes *and*
bounding volumes; GFX owns the ray-tracing pipeline** (hit/miss shaders + SBT). (§4.)

> **One axis, one model.** Allocation + borrow follow a single rule — storage class is the
> axis (stack = copy / heap = ref). Device buffers, images, and acceleration structures are
> ordinary RAII values whose lifetime is the scope-exit drop chain.

---

## 1. The model

### 1.1 The GPU package tree — and what stays out of it

The stdlib GPU tier is three nested, **write-once-run-everywhere** packages:

- **`cajeta.xpu`** — the shared foundation: value types & math, the memory/buffer model,
  textures & images, `@Kernel`, inline `RayQuery`, waves/atomics. The classes *both* facets need.
- **`cajeta.xpu.xpu`** — the compute-*primitive* facet: GPU compute primitives that are
  compute-only (gfx does not use them) — `CooperativeMatrix`/`CoopStage` (tensor-core matmul)
  today, more as they land — that the numerical library lowers onto. Built on `cajeta.xpu`.
  (The `Tensor` and the numpy-equivalent numerical library do **not** live here — they are
  **`cajeta.math`**, a backend-agnostic, CPU-first stdlib package that *uses* `cajeta.xpu` /
  `cajeta.xpu.xpu` for acceleration. See `documents/cajeta-math/numpy-porting-spec.md`.)
- **`cajeta.xpu.gfx`** — the graphics facet: rasterization, the render graph, the ray-tracing
  pipeline + basic graphics algorithms — the **primitives an engine dev composes, not an
  engine** — built on `cajeta.xpu`.

**The boundary (deliberate):** stdlib carries *primitives that tightly wrap GPU capabilities
plus basic algorithms* — a framework-neutral starting point. It does **not** carry framework
opinions. The numpy-equivalent numerical library (the `Tensor` + core ops + `linalg`/`fft`/
`random`) is also stdlib, but as **`cajeta.math`** (backend-agnostic, CPU-first), not here —
it is the generic n-d array every framework wraps, so it is canonical-in-stdlib, while the
*opinions layered on it* are not. Opinionated ML frameworks (a `torch`/`keras` surface —
autograd-by-default, the `nn` module set, optimizers), the scipy/sklearn breadth
(`cajeta.sci`/`cajeta.learn`), and applications (a spatial-index engine like Toffee) are
**separate libraries** built on `cajeta.math` — keeping cajeta from taking a framework stance
in its own stdlib.

There are no `cajeta.xpu.nvidia` / `.amd` / `.metal` / `.vulkan` packages in stdlib either.
Vendor-exclusive silicon (NV cooperative vector / TMA, AMD MFMA, Metal simdgroup-matrix,
CUDA/ROCm/MPS interop) lives in **external vendor libraries** — distributed as signed
dependencies from `olla.cajeta.dev`, added explicitly. Importing one *is* the lock-in
declaration.

Write to `cajeta.xpu` (+ `.xpu`/`.gfx`) and you get more than you asked for: it already
selects the best available silicon path under the hood and falls back where a backend lacks
one. Want vendor-specific peak or exclusive features — import a vendor library and accept its
reach.

### 1.2 Two seams — verbs *and* nouns

A device capability is not just a call; it is a call **on a datastructure**. So core lowers
through **two** parallel seams:

- **Verb seam** (`LoweringTarget`, `src/cajeta/xpu/lowering/LoweringTarget.h`) — core method
  → the driver op/call available on the target backend.
- **Noun seam** (the resource provider) — core datastructure → the concrete representation
  the chosen driver path expects.

Every capability = `(verb seam) + (noun seam)`, each with native realizations and a portable
default. The noun seam is the harder, less obvious half (§4).

### 1.3 Native where the silicon has it, portable default everywhere — cross-checked

Each verb seam is **pure-virtual** (every backend must supply its own — e.g. the wave ops)
or **defaulted** (a portable form correct on every backend, overridden only by a backend
with a faster native op). The discipline:

> **Native where the silicon has it, portable default everywhere else.**

The correctness guarantee is the **bit-exact cross-check**: the portable default (verified on
AMD/CPU) and the native op (verified on Vulkan) must agree to the bit. That check only exists
because core is an abstraction with multiple realizations — it is *not* "Vulkan with a CPU
mode."

### 1.4 Nouns: build-from-description, not convert-between-builts

A core datastructure (an `AccelerationStructure`, a texture) is **not** a concrete blob you
transcode to a vendor format — a Vulkan BVH is an opaque driver artifact with nothing to
translate. The core noun is the **build description** (the inputs + a query interface); each
backend **builds its own** concrete representation from that description. The seam's job is
"build *your* representation from *my* description," never "convert my bytes to yours."

Two consequences:

- **The noun's chosen implementation determines the verb's lowering.** A software BVH needs
  software traversal; a Vulkan BVH needs `OpRayQuery`. The implementation is selected **once,
  when the resource is built**, and the verb follows the noun — they are coupled, not
  independently dispatched.
- **The built artifact is opaque and per-implementation.** The interface exposes the inputs
  and the query verbs; it hides the representation entirely.

### 1.5 Choosing an implementation — heuristic + impl layers

Where more than one implementation is available, the app selects by a **capability heuristic
with explicit override** (default: "use hardware if present"; override required because RT,
for one, *loses* at large radius / extreme density — the app, or the query parameters, must
be able to force the software path).

Which implementations exist at all is set by the **impl layers** present:

- **AOT binaries** — link the implementation layers you want; the app chooses among the
  compiled-in set at runtime.
- **JIT (`.cja`)** — whatever is on the classpath, with **`core` + `cpu` always built in** —
  so a `.cja` always has at least the software/CPU floor; vendor impls come from classpath deps.

A vendor library author writes a verb + its native lowering **+ a portable SPIR-V degrade**
using the same seam machinery core uses internally. So a vendor library is not hard-locked:
on absent silicon it can fall through SPIR-V to a competing GPU or to CPU. The degrade is a
*hook with a vendor-authored fallback* — SPIR-V is the emission vehicle, not free same-perf
portability (a vendor-exclusive op has no SPIR-V equivalent, so its fallback is the slower
portable algorithm the author supplies).

> **Core is just the in-tree "vendor library" that has a fallback for everything.** Same
> shape as an external one — verb + lowering + degrade — only complete and built-in.

The internal face of this is **named and built**: `LoweringTarget::ImplTier { Native, Portable }`
is the one degrade concept both core degrade features answer through — the coop-matrix verb
(`coopMatrixTier`) and the ray-query verb (`rayQueryTier`, derived from the AS noun's recorded
`NounImpl`). The explicit override is the `CAJETA_GPU_<FEATURE>_IMPL` family (`resolveImplTier`
in the compiler for compile-time features; `CAJETA_GPU_AS_IMPL` / `caj_resolve_as_impl` in the
runtime for the AS noun) — `CAJETA_GPU_COOPMATRIX_IMPL=software` forces the portable tile even on
a native-capable device. Because Native and Portable are *different realizations* (the hardware
MMA and the triple-loop tile accumulate in a different order), an arithmetic feature's two tiers
need not be bit-identical — each is validated against the reference, not against the other.

The third-party side of this — the SPI a driver vendor or interest group uses to ship an
extension library — is [`VendorExtensionSDK.md`](VendorExtensionSDK.md) (a seed; it crystallizes
now that core has dogfooded the seam machinery).

### 1.6 Degradation rule

Core always runs: with no GPU it lowers to CPU (CPU is the floor, not a package). A vendor
library **never silently emulates** — absent silicon yields a compile diagnostic / no-device,
*unless* the author provided a degrade. You opt a portable kernel into graceful fallback by
writing a core branch beside a guarded vendor branch:

```cajeta
if (Device.supports(Capability.X)) {
    // vendor-library fast path on its silicon
} else {
    // cajeta.xpu — portable; floors to CPU when no GPU is present
}
```

> **Status.** This is the design contract; it drives the plans. The portable surface ships
> as **`cajeta.xpu`** (renamed from `cajeta.xpu.core`); `Device.supports(...)` is built;
> the noun seam is now first-class (`CajetaNounProvider`, dogfooded on `AccelerationStructure`
> whose built impl is a recorded property the verb follows — §4.4); the **explicit override +
> execution mechanism** are built (an `AsImpl` enum + `AccelerationStructure.of(...)` + a
> `CAJETA_GPU_AS_IMPL` env override; forced-software runs on a ray-query GPU via a `$sw` kernel
> variant — one backend, either impl); the **internal** impl-layer/degrade seam is now named
> (`ImplTier`) and dogfooded across both degrade features (coop-matrix + ray-query) with a
> generic `CAJETA_GPU_<FEATURE>_IMPL` override (§1.5); the *automatic* density/extent heuristic
> and the **external** vendor-SDK that exposes the degrade seam are unbuilt (still a seed).

---

## 2. Backends (how core lowers)

"Backend" here means a target the compiler lowers **core** to — distinct from a vendor
*library* (§1.1), which exposes exclusive verbs. Core has four backends; Metal is planned.

| Backend | Pipeline | Status |
|---------|----------|--------|
| **CPU** | LLJIT (in-process) | ✅ reference path + bit-exact oracle; "device" is the host. The floor. |
| **Vulkan** | SPIR-V → driver | ✅ device-verified (RADV / Strix Halo); richest; new capability lands here first (we control the SPIR-V fork). Reaches Intel / Mali / Adreno / lavapipe — GPUs with no native backend. |
| **AMD** | AMDGPU → hsaco | ✅ device-verified (gfx1151), incl. storage images (surface objects). Gap: mipmaps driver-blocked. |
| **NVIDIA** | NVPTX → cubin → fatbin | ⚠️ emit-only; advanced seams not overridden; no on-device run (pending B5 WSL2+CUDA). |
| **Metal** | — | ❌ absent; planned MoltenVK → native. |

Runtime selection order: `CUDA → HIP → Vulkan → CPU`.

---

## 3. Core verbs

The complete verb set **is** the `LoweringTarget` seams — that header is authoritative; this
is its grouped contract. Legend: **●** native, device-verified · **○** portable default,
device-verified · **◐** emit-only (NVIDIA) · **◷** intended-core, fallback not yet written ·
**—** N/A · **✗** no backend.

### 3.1 Execution — coordinates, barriers, kernel shape

| Verb | CPU | VK | AMD | NV | Metal |
|------|:--:|:--:|:--:|:--:|:--:|
| `threadId` / `workgroupId` / `workgroupDim` / `globalId` / `gridSize` | ● | ● | ● | ◐ | ✗ |
| `workgroupBarrier` | ● | ● | ● | ◐ | ✗ |
| `createKernel` / `materializeParam` / `decorateKernel` | ● | ● | ● | ◐ | ✗ |
| dynamic `shared T[n]` | ● | ● | ● | ◐ | ✗ |

### 3.2 Memory & buffers

| Verb | CPU | VK | AMD | NV | Metal |
|------|:--:|:--:|:--:|:--:|:--:|
| `bufferElementPtr` / `bufferParamType` | ● | ● | ● | ◐ | ✗ |
| `bufferArrayElement` (bindless `KernelBuffer<T>[]`) | ● | ● | ● | — | ✗ |
| `KernelBuffer<T>` alloc/upload/download · `MemoryKind` (Device/Pinned/Unified) · `slice` | ● | ● | ● | ◐ | ✗ |

### 3.3 Value types & math ([`ValueTypeCatalog.md`](ValueTypeCatalog.md))

| Verb | CPU | VK | AMD | NV | Metal |
|------|:--:|:--:|:--:|:--:|:--:|
| `Vector<T,N>` / `Matrix<T,R,C>` / `Quaternion<T>` (+ comparison masks → `.all()`/`.any()`/`.select()`) | ● | ● | ● | ● | ✗ |
| `transcendental` (sin/…/pow/rsqrt; AMD → `__ocml_*`) | ● | ● | ● | ◐ | ✗ |
| `integerDot4x8` (DP4a) · `Bits.*` | ○/● | ● | ○/● | ◐ | ✗ |
| element types f32/f16/**bf16** (fp8 deferred — no LLVM type) | ● | ● | ● | ◐ | ✗ |

### 3.4 Textures & images ([`WritableImages.md`](WritableImages.md))

| Verb | CPU | VK | AMD | NV | Metal |
|------|:--:|:--:|:--:|:--:|:--:|
| `sampleTexture` / `fetchTexture` (2-D) | ● | ● | ● | ◐ | ✗ |
| `sample`/`fetch` **3D · 1D · 2DArray · Cube** | ● | ● | ● | — | ✗ |
| mipmaps / explicit LOD (`fetchLod`/`sampleLod`) | ● | ● | ◑ | — | ✗ |
| `storeImage` / `loadImage` (`Image2D` storage RMW) | ● | ● | ● | ◐ | ✗ |

**◑** AMD mipmaps: code complete + emit-verified, but `hipMallocMipmappedArray` is
unsupported on gfx1151/ROCm 7.2.2 — degrades gracefully, device test SKIPs.

### 3.5 Capability primitives

| Family | Verbs | CPU | VK | AMD | NV | Metal |
|--------|-------|:--:|:--:|:--:|:--:|:--:|
| Atomics ([`xpu/FloatAtomics.md`](../xpu/FloatAtomics.md)) | `atomicFloatRMW` / `atomicIntRMW` / `atomicCompareExchange` | ● | ● | ● | ◐ | ✗ |
| Wave ([`xpu/WaveReductions.md`](../xpu/WaveReductions.md)) | `waveWidth`/`LaneId`/`Shuffle`/`Ballot`/`ReduceSum`/`Reduce` | ● | ● | ● | ◐ | ✗ |
| | `waveScan` / `waveRotate` (portable default; VK native) | ●/○ | ● | ○ | ◐ | ✗ |
| Quad ([`xpu/QuadControl.md`](../xpu/QuadControl.md)) | `quadBroadcast`/`Swap`/`All`/`Any` | ○ | ● | ○ | ◐ | ✗ |
| Coop matrix | `coopMatrixLoad/Store/MulAdd/Splat` (the MMA operand — not a tensor) | ○ | ● | ● (WMMA) | ◐ | ✗ |
| Shader clock ([`xpu/ShaderClock.md`](../xpu/ShaderClock.md)) | `readClock` | ● | ● | ● | ◐ | ✗ |
| **Ray query** (§4) | `rayQueryInitialize/Proceed/…` | ◷ | ● | ◷ | ◷ | ✗ |

---

## 4. Core nouns — and ray query as the worked example

The datastructures core owns: `KernelBuffer<T>`, `Texture2D`/`Image2D`, and `AccelerationStructure`.
Each goes through the **noun seam** (§1.2): a core build-description with per-backend
representations. Ray query is the case that *defines* the seam, because its noun (the scene)
is the heavy part.

### 4.1 The real seam — inline query vs pipeline, not triangle vs AABB

Ray query belongs in `gpu.core` (both compute and gfx pull it from the foundation). But the
scope line is **not** triangle-vs-AABB — that was a wrong inference. Scientific compute needs
ray-**triangle** queries pervasively: mesh Monte-Carlo transport (Möller-Trumbore boundary
crossings), SDF & curvature from meshes, ICP / 6-D pose against mesh instances, BVH-over-mesh
NN training (fVDB). 3-D-Gaussian and point-cloud methods need the **AABB/procedural** path.
**Both geometry inputs are compute primitives — core needs both.**

The vendor APIs do split the two geometry *inputs* (and core supports both):

| Vendor | Triangle geometry input | AABB/procedural geometry input |
|--------|-------------------------|--------------------------------|
| Vulkan | `VK_GEOMETRY_TYPE_TRIANGLES_KHR` | `VK_GEOMETRY_TYPE_AABBS_KHR` + `generateIntersection` |
| NVIDIA OptiX | `OPTIX_BUILD_INPUT_TYPE_TRIANGLES` | `…_CUSTOM_PRIMITIVES` + intersection program |
| Metal | `…TriangleGeometryDescriptor` | `…BoundingBoxGeometryDescriptor` + intersection function |
| DXR | `…GEOMETRY_TYPE_TRIANGLES` | `…PROCEDURAL_PRIMITIVE_AABBS` |
| AMD | — through Vulkan/DXR — | inherits the Vulkan inputs |

What the vendors actually gate behind the *heavier* model is the **ray-tracing pipeline** —
raygen / closesthit / anyhit / miss shaders + a shader binding table — which **none** of those
compute uses need; they all use **inline ray query in a kernel**. *That* is the real seam:

> GPU foundation core = **inline ray query over BOTH triangle meshes and AABB/procedural
> BVHs**, with traversal, custom intersection, and the full getters (t, barycentrics,
> primitiveIndex, frontFace). GFX = the **ray-tracing pipeline** (SBT + hit/miss programs).

TLAS/instancing is an orthogonal advanced axis, deferred — though ICP / 6-D pose will want
instance transforms, so it's a "soon," not a "never."

### 4.2 The noun: `AccelerationStructure`

Build description = the geometry — **triangle (vertex + index buffers) and/or AABB
(`(min, max)` × N)** — plus build params. Today it has one representation: a Vulkan
`VK_KHR_acceleration_structure` BVH over AABBs only — so the noun is doubly narrow
(Vulkan-locked *and* AABB-only). To be core it needs a **portable software BVH** built
(LBVH / binned-SAH) over both geometry kinds into a plain `KernelBuffer<T>` (node array + primitive
refs), runnable on host or as a build kernel. Same description, two builds; the hardware BVH
is the *acceleration*, the software BVH is what makes it core.

### 4.3 The verb: `RayQuery`

A kernel-local cursor: `initialize → proceed* → getters`. Built today: `initialize / proceed /
committedType / candidateType / candidatePrimitiveIndex` — AABB-only, and unable to commit a
hit. To be core it needs the **`T` (distance)** + **barycentrics / frontFace** getters and
**`confirm` / `generate` intersection** — without those it can only *count* candidates, not
return a **nearest** hit (why Toffee's `countWithin` only counts). The portable lowering is a
**software traversal kernel** over the §4.2 BVH: a fixed/stackless stack, **Möller-Trumbore**
for triangle leaves, custom intersection for AABB leaves; the getters then compute directly.

### 4.4 Why this proves the model

Ray query exercises both seams and the impl-layer rule end to end:

- **Noun-impl drives verb-lowering** — a software BVH ⇒ software traversal; a Vulkan BVH ⇒
  `OpRayQuery`. Chosen **once at build time** by the capability heuristic (override-able: RT
  is not always the win), and the verb follows.
- **Build-from-description** — you never convert a Vulkan BVH to a software one; both build
  from the same geometry description (triangles and/or AABBs).
- **Impl layers** — `core`+`cpu` ship the software AS+traversal (the JIT floor); a hardware-RT
  impl can arrive as a linked layer / library; the app picks by capability.

---

## 5. Honest status & reconciled drift

- **CPU** — complete (reference), incl. storage images (`Image2D` — the in-process host float
  store, the oracle for the device storage-image path).
- **Vulkan** — complete, all native, device-verified.
- **AMD** — complete & device-verified, incl. storage images (`Image2D` via surface objects /
  `__ockl_image_store_2D`/`load_2D`); **except** mipmaps (driver-blocked, code done).
  Cooperative matrix native (WMMA).
- **NVIDIA** — emit-only and incomplete; nothing device-verified. Storage images
  (`Image2D`) now emit (`sust.b.2d`/`suld.b.2d`, ◐); device run + the CUDA surface
  runtime land with the B5 runner.
- **Metal** — absent (needs the backend, MoltenVK → native).
- **Vendor libraries** — none exist; out of stdlib by design (§1.1).

**Drift reconciled (the file was rewritten clean, not patched):**

- **1-D / cube / 2-D-array textures are done and device-verified** on CPU + Vulkan + AMD
  (`texture1d*` / `texture2dArray*` / `textureCube*` across `XpuVulkan/Hip/Cpu` tests) — the
  plan's "still open" is stale.
- **Ray query is *not* core yet** — its noun and verb are Vulkan-only *and* AABB-only; "core
  intent" without a software path (over **both** meshes and BVHs) is the badge-without-substance
  this model forbids. The fix is §4.2–§4.3.
- Earlier revisions invented vendor verb tables (TMA/MFMA/…) presented as spec — removed;
  vendor surface is external libraries, not foundation content.

---

## 6. Definition of done & how this drives the plans

Core's DoD: §3's verb set with every cell ● / ○ on every shipped backend, **and** every core
noun with a portable build (§1.4) — i.e. no ◷/◐/✗ left unaccepted. The plans should section to
match this document:

- **Core plan** — done: ray query genuinely core (software BVH + traversal over triangles
  *and* AABBs, full getters + confirm/generate + nearest-hit, native + software);
  `Device.supports(...)`; the `cajeta.xpu.core → cajeta.xpu` rename; the noun seam as a
  first-class SPI (`CajetaNounProvider`, dogfooded on `AccelerationStructure` with a recorded
  impl tag); the capability **override + execution mechanism** (`AsImpl`/`.of` + `CAJETA_GPU_AS_IMPL`
  + the `$sw` software-on-Vulkan kernel variant — one backend, either impl, cross-checked
  three ways); the **internal** impl-layer/degrade seam (named `ImplTier` + generic
  `CAJETA_GPU_<FEATURE>_IMPL` override, dogfooded across coop-matrix + ray-query); AMD
  `Image2D` storage images (surface objects, device-verified on gfx1151). Remaining:
  NVIDIA advanced seams + B5 on-device; Metal backend; the *automatic*
  density/extent heuristic; the **external** vendor-SDK that exposes the degrade seam (seed);
  fp8 (pending LLVM); NV storage-image *device* run (emit done ◐; needs the B5 CUDA surface runtime).
- **Vendor libraries** — out of this plan; each its own external effort on the degrade
  framework, sequenced by hardware (AMD now; NVIDIA on B5; Metal on Mac).
- **GFX** — the ray-tracing pipeline (SBT, hit/miss shaders); built on this foundation's
  inline query + BVH. (Triangle *geometry* is core, per §4; TLAS/instancing is the next core
  AS axis, not gfx.)

---

## See also

- Authoritative seam set — `src/cajeta/xpu/lowering/LoweringTarget.h`.
- Forward plan — `plans/gpu/cajeta-gpu-plan.md`.
- Deep per-cell ledger — [`xpu/CajetaXPU-Matrix.md`](../xpu/CajetaXPU-Matrix.md); cross-backend
  discipline — [`xpu/CajetaXPU-Variance.md`](../xpu/CajetaXPU-Variance.md).
- Compute — [`xpu/CajetaXPU.md`](../xpu/CajetaXPU.md); CPU — [`xpu/CajetaCPU.md`](../xpu/CajetaCPU.md).
  Graphics — [`gfx/CajetaGFX.md`](../cajeta-gfx/cajeta-gfx-spec.md).
- Per-feature — `ValueTypeCatalog`, `Quaternions`, `MatrixDeterminantInverse`, `MaskSelect`,
  `IntegerDotProduct`, `BitInstructions`, `WritableImages`,
  `xpu/{FloatAtomics,IntegerAtomics,WaveReductions,WavePrefixScan,SubgroupRotate,QuadControl,ShaderClock}`.
