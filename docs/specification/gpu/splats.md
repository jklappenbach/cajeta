# Cajeta GPU — Gaussian Splats (`cajeta.xpu.splat`)

A **splat** is an anisotropic Gaussian kernel with attributes. It is a continuous,
differentiable primitive that is scattered (accumulated) into a target. 3D Gaussian
Splatting (Kerbl 2023) made it the state of the art for real-time radiance-field
rendering. The underlying operation, a scatter-accumulate of anisotropic kernels, is
far older and far broader than rendering (SPH, MPM particle↔grid transfer, RBF
meshfree methods, tomographic back-projection). For that reason splats belong in the
`cajeta-gpu` foundation rather than in graphics. They are a shared primitive with two
sibling consumers.

```
cajeta-gpu  (foundation: value types, KernelBuffer, Image, AccelerationStructure, atomics,
   ▲   ▲     sort/scan, ray query)  ──  cajeta.xpu.splat lives here, on core
   │   │
   │   └────────────────────────────┐
cajeta-xpu  (compute)            cajeta-gfx  (rendering)
  splatScatter: SPH / MPM /        canela mid/far tier: rasterize splats;
  tomography / RBF P2G             relightable material splats
```

`cajeta.xpu.splat` is a foundation library layered on `cajeta.xpu`. Its device
operations register through core's two seams (§1.3) and obey core's
native/portable/degrade discipline, exactly like the `AccelerationStructure` noun +
`RayQuery` verb. See [`CajetaGPU.md`](CajetaGPU.md) §4, which already names "3-D-Gaussian
and point-cloud methods" as the AABB/procedural ray-query consumer, and the virtual-
geometry consumer plan at `../../plans/gpu/gfx/canela-plan.md`.

> **Status: design spec, no code.** This fixes the data model, serialization format +
> codec (encode/decode only, with no file I/O, §3), render path, compute path, and
> lifecycle API. The implementation plan (TDD / Deliverables /
> Acceptance Criteria) is authored separately, later. Research backing every choice is
> in `../../plans/gpu/gfx/research/gfx/splats/` (filenames cited inline as `[author-year]`).

---

## 1. The model

### 1.1 What a splat *is* (the math)

One splat is a 3D anisotropic Gaussian `G(x) = exp(−½ (x−μ)ᵀ Σ⁻¹ (x−μ))` weighted by an
opacity α and carrying an appearance payload:

- **mean** `μ` is the center, a `Vector<float32,3>`.
- **covariance** `Σ` is stored factored as `Σ = R S Sᵀ Rᵀ` to stay positive-semidefinite
  and compact. It is a per-axis scale `Vector<float32,3>` (log-scale on disk) and a unit
  rotation `Quaternion<float32>`. Never store the 6 raw covariance entries, because the
  factored form is what stays valid under optimization and interpolation.
- **opacity** `α` is a `float32` (logit on disk, sigmoid-activated).
- **appearance** is pluggable (§2.2). It is view-dependent radiance (spherical harmonics),
  relightable material (PBR), flat color, or an arbitrary scientific payload.

Rendering projects `Σ` to a 2D screen-space covariance `Σ'` via the projection Jacobian
(the **EWA** resampling filter, [`zwicker-2001-ewa-volume-splatting`]) and alpha-composites
the resulting 2D Gaussians. Compute scatters `G(x)·payload` onto a grid. It is the same
kernel with two accumulation targets.

### 1.2 Why foundation, not gfx

The splat is a scatter of a kernel, and that decomposes entirely into operations
core already owns. Those are `atomicFloatRMW` (accumulate), sort + prefix-sum (`gpu-utils`),
`Vector`/`Matrix`/`Quaternion` math, `Image2D` storage RMW, and `AccelerationStructure`
ray query. There is no fixed-function "splat" silicon on any GPU, so the splat
operation is portable-by-construction. It is built from core compute verbs, with backends
contributing only tuning (a faster vendor sort, a wave-optimized blend). This is the
ideal core citizen. It is one portable algorithm, validated against the CPU oracle, with
faster native tiers slotted underneath.

The same `splatScatter` verb that rasterization uses for screen tiles is what `cajeta-xpu`
uses for particle→grid transfer (SPH/MPM), tomographic back-projection, and
RBF/meshfree deposition (§7). Siloing it in gfx would force every scientific consumer
to reimplement it, which is exactly the anti-pattern the foundation exists to prevent.

### 1.3 Two seams (matches `CajetaGPU.md` §1.2)

| Seam | Splat instance | Native tier | Portable tier |
|------|----------------|-------------|---------------|
| **Noun** | `SplatCloud` — build-description (SoA attribute buffers + metadata); built acceleration (depth order, tile bins, AABB BVH, octree LOD) follows the build, never converted | compressed/octree GPU residency; hardware-BVH for the ray path | uncompressed SoA in `KernelBuffer<T>`; software LBVH; CPU reference |
| **Verb** | `splatRasterize`, `splatScatter`, depth-`sort`, `rayQueryOverSplats` | vendor radix sort; wave-optimized tile blend; HW ray query | core compute kernels (sort/scan/atomics/EWA) — the floor |

The implementation tier is chosen once at build/dispatch and is override-able with
the core env family. The variable is `CAJETA_GPU_SPLAT_IMPL` (`native` | `portable`/`software`),
mirroring `CAJETA_GPU_AS_IMPL`. Correctness follows the bit-checked-against-reference rule.
The portable rasterizer (CPU oracle) and any native tier each validate against the
reference image/gradient and never against each other, because different blend orders need
not be bit-identical. That is the same rule as coop-matrix tiers.

---

## 2. Data structures (in-memory)

### 2.1 Per-splat attributes & on-device encoding

Attributes are stored Structure-of-Arrays (one `KernelBuffer<T>` per attribute). That layout
is required for the sort (sort an index permutation, gather attributes) and for per-attribute
compression.

| Attribute | Live type | Disk encoding (§3.2) | Notes |
|-----------|-----------|----------------------|-------|
| position μ | `Vector<float32,3>` | 16-bit per-axis, octree-cell-relative | quantization range from the cloud/node AABB |
| scale | `Vector<float32,3>` | 8–16-bit log-scale | log domain keeps it positive |
| rotation | `Quaternion<float32>` | "smallest-three" (drop largest, 3×10-bit + 2-bit index) | always re-normalized on load |
| opacity α | `float32` | 8-bit logit | sigmoid on use |
| appearance | §2.2 | per-model | the bulk of the bytes (SH degree-3 = 48 floats) |

Element types follow core, so they are float32 / float16 / bfloat16. fp8 is deferred, since
it is storage-only with no device arithmetic yet, per `CajetaGPU.md` §3.3.

### 2.2 Appearance models (the pluggable payload)

The Gaussian geometry is universal, and the payload is parameterized. One `SplatCloud`
carries exactly one appearance model, recorded in its header:

| Model | Payload | Use | Research |
|-------|---------|-----|----------|
| `Radiance(shDegree 0..3)` | SH coeffs (DC `Vector<float32,3>` + bands) | view-dependent capture / novel-view | [`kerbl-2023`] |
| `Material` (PBR) | albedo `Vector<float32,3>`, normal `Vector<float32,3>`, roughness `float32`, metalness `float32` | **relightable** splats; the canela mid/far tier (capture *material*, not lit color) | [`liang-2023-gs-ir`], [`gao-2023-relightable`], [`jiang-2023-gaussianshader`] |
| `Flat` | `Vector<float32,4>` RGBA | data-viz, debug, scientific scalar fields | — |
| `Payload<T>` | arbitrary `T` (scalar/vector) | the **xpu scatter** path — carries the quantity to deposit (SPH density, MPM momentum, CT attenuation) | §7 |

`Material` is the model `canela` requires. It is what makes a splat tier relightable
under dynamic lighting rather than baking the capture's lighting in.

### 2.3 The `SplatCloud` noun

```cajeta
// cajeta.xpu.splat
class SplatCloud<A extends SplatAppearance> {
    // --- build-description: SoA attribute buffers (heap = ref) ---
    KernelBuffer<Vector<float32,3>>  positions
    KernelBuffer<Vector<float32,3>>  scales        // log-scale
    KernelBuffer<Quaternion<float32>> rotations
    KernelBuffer<float32>        opacities     // logit
    A.Storage              appearance    // SH bands / material channels / payload

    AABB                   bounds
    SplatAppearanceKind    kind

    int64 count()                        // element count — never `size`/`length`

    // --- built acceleration (follows the build; opaque; rebuilt on edit) ---
    SplatOrder        order()       // depth/tile permutation for a given view
    AccelerationStructure bvh()     // AABB BVH over per-splat 3σ bounds (core noun)
    SplatLod          lod()         // octree LOD index (§2.4), if built
}
```

Construction is build-from-description (`CajetaGPU.md` §1.4). The compressed GPU
residency, the BVH, and the octree are built from the attribute buffers and never
transcoded from another built form. Edits invalidate and rebuild the affected
acceleration only.

### 2.4 LOD & spatial structure

For streaming and view-bounded selection, splats are organized into an octree with
per-node LOD ([`ren-2024-octree-gs`]) over anchor-structured Gaussians
([`lu-2023-scaffold-gs`]). Very large scenes use the chunked hierarchy of
[`kerbl-2024-hierarchical-3dgs-large`]. The octree node is also the streaming unit
(§3.3) and the count-budget unit ([`fang-2024-mini-splatting`]). A view selects the
coarsest LOD whose projected splat size stays sub-pixel, exactly paralleling canela's
distance bands.

```cajeta
SplatView v = cloud.lod().select(camera, screen, budget)   // active splat set for a frame
```

---

## 3. Serialization — the splat *codec* (`.cajsplat`)

> **Scope: `splat` defines the *format* and a *codec*, not file I/O.** The byte layout is
> intrinsic to the data structure, so it belongs here. The splat package only
> encodes/decodes a `SplatCloud` to and from an abstract byte stream (`KernelStream` /
> `KernelBuffer<uint8>`), and it never opens files, mmaps, walks paths, or manages residency.
> The actual file/stream/mmap I/O and streaming residency live in the asset / `cajeta.io`
> layer (canela's residency manager), which composes this codec with real I/O. This is the
> same split as any other stdlib codec. The codec knows bytes, and the io layer knows files.

```cajeta
// cajeta.xpu.splat.codec — pure encode/decode; no files
SplatCodec.encode(cloud, stream)                 // SplatCloud -> bytes
SplatCloud<A> c = SplatCodec.decode<A>(stream)   // bytes -> SplatCloud
SplatCodec.decodeNode(stream, nodeSpan)          // ranged decode of one octree node (§3.3)
```

The container is self-describing, and it imports/exports PLY, SPZ, SOG (§3.4) for ecosystem
interop.

### 3.1 Container layout

```
┌─ Header ───────────────────────────────────────────────┐
│ magic "CAJSPLAT", version, flags                        │
│ splatCount, appearanceKind, shDegree | materialChannels │
│ bounds (AABB), upAxis, unit scale                       │
│ attribute table: per-attribute { encoding, offset, len }│
│ compression scheme id + params (§3.2)                   │
│ lod: octree present? root offset, node count, depth     │
│ dynamic: §4.4 deformation block present? frame count    │
└────────────────────────────────────────────────────────┘
┌─ Attribute streams (SoA, per-attribute, page-aligned) ──┐
│ positions │ scales │ rotations │ opacities │ appearance │   (each optionally compressed)
└────────────────────────────────────────────────────────┘
┌─ LOD / octree index (node AABBs, child ptrs, splat spans)┐
┌─ Optional: deformation / 4D block (§4.4) ───────────────┐
```

SoA + page-aligned streams mean an attribute (or an octree node's span within it) is a
contiguous, ranged region, so the io layer can map/read exactly what it needs and hand
the bytes to `SplatCodec.decodeNode`. The codec defines the spans and does not read them.

### 3.2 Compression & quantization

Compression is per-attribute, with the scheme recorded in the header. Defaults draw on the
proven results:

- **Attribute quantization** uses octree-relative 16-bit positions, smallest-three quats,
  8-bit logit opacity, and lower-precision SH bands (DC kept higher).
- **Codebook / VQ** uses sensitivity-aware vector quantization + entropy coding
  ([`niedermayr-2024-compressed-3dgs`]) and learned masking + codebooks
  ([`lee-2023-compact-3d-gaussian`]).
- **Image-grid (SOG-style)** packs attributes into 2D textures for GPU-friendly,
  block-compressible residency.

Lossy schemes target a stated PSNR budget. The lossless SoA path is always available as
the reference/round-trip oracle.

### 3.3 Streaming layout (what the codec exposes vs what the io layer does)

The octree node is the residency unit. Nodes carry their attribute spans (offsets into
each stream), so the io layer can fetch a node's splats with one ranged read per
attribute and call `decodeNode`. The codec's job is purely to make spans cheap to
locate and decode (partial, ranged, page-aligned). The actual reads, eviction, and
residency policy live in the consumer, which is canela's distance-band/orientation manager,
and it keeps coarse LOD resident as the no-holes fallback. `splat` provides the format
that makes streaming possible, and it does not stream.

### 3.4 Interop

| Format | Dir | Notes |
|--------|-----|-------|
| **PLY** (3DGS canonical) | in/out | the de-facto research interchange; custom float properties |
| **SPZ** (Niantic, MIT) | in/out | compact, ecosystem-standard |
| **SOG** (PlayCanvas) | in/out | image-grid; engine interchange |
| `.cajsplat` | native | the streamable/LOD/4D superset |

---

## 4. Construction & optimization (lifecycle: *in*)

### 4.1 From explicit data
Wrap existing attribute buffers (a converted point cloud, a procedural field) directly
into a `SplatCloud`, and build acceleration on demand.

### 4.2 Fitting / training (differentiable)
The optimization loop that produces a cloud from images:
1. **Init** from an SfM/point-cloud seed or a uniform volume.
2. **Differentiable rasterize** (§5.4) renders, compares to target images, and backprops to
   all parameters (μ, scale, rot, α, appearance).
3. **Adam step**.
4. **Adaptive density control** clones/splits under-reconstructed Gaussians, prunes
   transparent/oversized ones, and bounds the count ([`fang-2024-mini-splatting`]).

```cajeta
SplatCloud<Radiance> fit = SplatTrainer
    .from(seedPoints)
    .targets(images, poses)
    .options(SplatFit{ shDegree: 3, iters: 30_000, densifyUntil: 15_000 })
    .run()
```

This is the heaviest GPU consumer. It exercises rasterize forward+backward, sort, scan,
and atomics together, and it is a flagship integration test for the compute stack.

### 4.3 From capture (the canela bridge)
`canela`'s raycast/G-buffer capture already emits oriented points with material
(position, normal, albedo, roughness), which is the exact init for a `Material` `SplatCloud`. So
the same capture pass feeds either the TSDF→mesh path or, via a short fit, the splat tier.
There is no second capture pipeline, which is the unifying insight in `canela-plan.md`.

### 4.4 Dynamic / 4D (optional, later)
Animated content is a canonical cloud + deformation field. The field is an MLP/grid warp
([`yang-2023-deformable-3d-gaussians`], [`wu-2023-4d-gaussian-splatting`]) or a sparse
control-point rig driving dense Gaussians via LBS ([`huang-2023-sc-gs`]), and the latter
is the closest to engine skeletal animation. The `.cajsplat` 4D block stores per-frame or
control-point data. This is deferred, because it is heavy and physics/animation stay on the
mesh backbone in canela's hybrid.

---

## 5. Rendering — the rasterizer (verb: `splatRasterize`)

### 5.1 Tile-based pipeline ([`kerbl-2023`])
1. **Cull + project.** Frustum cull, project μ to screen, compute 2D `Σ'` from 3D `Σ`
   via the projection Jacobian, and derive the 3σ screen extent.
2. **Tile bin.** Map each splat to the 16×16 tiles it overlaps, and build per-tile key arrays
   `(tileId, depth)` with prefix-sum compaction.
3. **Sort.** Radix-sort the keys (reuse `gpu-utils` sort) into per-tile depth order.
4. **Blend.** Per tile, per pixel, front-to-back alpha composite over the sorted splats.
   Compute `w = α·exp(−½ dᵀ Σ'⁻¹ d)`, accumulate `color·w·T`, update transmittance `T`, and
   early-out when `T < ε`.

### 5.2 Projection filter + anti-aliasing
The filter is EWA screen-space resampling ([`zwicker-2001-ewa-volume-splatting`]) plus
Mip-Splatting ([`yu-2023-mip-splatting`]), which is a 3D smoothing filter bounding the max
frequency + a 2D Mip (dilation) filter. AA is baked into the foundation rasterizer because
LOD/zoom alias badly without it, and the splat tier is multi-resolution. The surface-aligned
2DGS variant ([`huang-2024-2d-gaussian-splatting`]) is offered where geometric/normal
accuracy matters (relight, mesh extraction).

### 5.3 Appearance evaluation
- `Radiance` evaluates SH at the view direction during blend into RGB.
- `Material` writes a G-buffer (albedo/normal/roughness) and defers relighting to
  the engine's lighting pass. This is what makes the splat tier relightable
  ([`liang-2023-gs-ir`], [`gao-2023-relightable`], [`jiang-2023-gaussianshader`]).
- `Flat`/`Payload` are written direct.

### 5.4 Backward pass (differentiable)
The same tile structure runs in reverse to produce gradients w.r.t. every parameter. It is
the engine of §4.2 and of any inverse problem (§7). Forward and backward share the EWA
math and tile bins, and only the accumulation direction differs.

### 5.5 Render integration
`splatRasterize` is a render-graph pass in gfx (composited with mesh draws via a
shared depth buffer for correct occlusion), and a standalone dispatch in headless/compute
use. Depth sort is reusable across frames under small camera motion, so you can amortize
the dominant per-frame cost.

---

## 6. Ray-query path (verb: `rayQueryOverSplats`)

Per `CajetaGPU.md` §4.1, splats are the AABB/procedural ray-query consumer. Build a
core `AccelerationStructure` over per-splat 3σ AABBs, and the custom intersection evaluates the
Gaussian response along the ray.

The uses are picking/selection (editor), ray-traced shadows/reflections on splats
([`gao-2023-relightable`] traces for shadows), collision/NN queries for the xpu
consumers, and an alternative to rasterization for secondary rays. It is the same noun
(`AccelerationStructure`) and the same verb (`RayQuery`) the foundation already defines, and
splats just supply AABB geometry + an intersection program.

---

## 7. Compute use (`cajeta-xpu`) — splat as the general scatter primitive

`splatScatter` deposits `G(x)·payload` from each splat onto a target grid/buffer with
`atomicFloatRMW`. It is the same kernel-deposit math as §5.1 step 4 with a world-space
target in place of the screen-space one. The scientific mappings:

| Domain | Splat = | Target | Verb |
|--------|---------|--------|------|
| **SPH** (fluids/astro) | particle + smoothing kernel | field grid | `splatScatter` (P2G) |
| **MPM / PIC-FLIP** | material particle | background grid | `splatScatter` (P2G) + gather (G2P) |
| **Tomography** (CT/MRI) | filtered projection sample | volume `Image3D` | `splatScatter` (back-projection) |
| **RBF / meshfree** | anisotropic basis | solution grid | `splatScatter` |
| **Radio-astro gridding** | visibility × conv kernel | uv-grid | `splatScatter` |

These need only the `Payload<T>` appearance and the scatter verb, with no SH and no blend.
This is why the primitive is foundation. Rendering is one caller of `splatScatter`, and
science is several others. [`xie-2023-physgaussian`] (MPM on Gaussians) and [`jiang-2024-vr-gs`]
(XPBD + splats) are the worked examples that splat-as-simulation-substrate is real.

---

## 8. Editing & conversion

- **Transform / merge / segment** covers rigid transforms (apply to μ + rotate `Σ` via the
  quaternion), concatenating clouds, and semantic select/delete ([`chen-2023-gaussianeditor`]).
- **Mesh extraction** is `SplatCloud → mesh` via SuGaR ([`guedon-2023-sugar`]) or 2DGS
  ([`huang-2024-2d-gaussian-splatting`]). It is the bridge back to collision/physics geometry.
  canela keeps physics on meshes, and this is how a splat asset gets its collider.
- **Texture bake** flattens `Material` appearance to standard PBR textures for the mesh
  tiers.
- **SLAM/online build** is incremental construction from RGB-D ([`keetha-2023-splatam`]).

---

## 9. Lifecycle & memory

Splat resources are ordinary RAII values under the one allocation/borrow axis
(`CajetaGPU.md`). Heap buffers are refs, and lifetime is the scope-exit drop chain. Release
follows the drop idiom, `cloud = null` (or reassignment), and never `delete`/`free`.
Built acceleration (order/BVH/octree) drops with the cloud. Streamed octree nodes are
evicted by the consumer's residency policy with coarse LOD as the resident floor.

---

## 10. Status & how this drives the plan

This document is the design contract. The forward plan (TDD / Deliverables /
Acceptance Criteria, per the plan-authoring format) is written separately and should
section to match:

1. **Core noun + verbs** covers `SplatCloud`, `splatRasterize` (fwd), `splatScatter`, and depth
   sort (on `gpu-utils`), through the two seams with a CPU reference oracle and
   `CAJETA_GPU_SPLAT_IMPL` override. This is the minimum that makes splats render at all.
2. **Codec** covers `SplatCodec` encode/decode to a byte stream (`.cajsplat` SoA + header) +
   PLY/SPZ import, lossless first. There is no file I/O, because that is the asset/`cajeta.io` layer.
3. **AA + appearance** covers EWA + Mip-Splatting, then `Radiance`, then `Material`(+deferred relight).
4. **Differentiable backward + fit** covers the training loop and densify/prune.
5. **LOD + (codec side of) streaming** covers the octree, `lod().select(...)`, ranged `decodeNode`,
   and compression. Residency and actual reads are the consumer's and never splat's.
6. **Ray-query path** covers the AABB BVH over splats + intersection.
7. **xpu scatter** covers `Payload<T>` + the SPH/MPM/tomography mappings. It validates the
   foundation claim that this is no graphics feature.
8. **Conversion** covers SuGaR/2DGS mesh extraction and texture bake.
9. *(deferred)* dynamic/4D; editor/SLAM tooling.

Sequencing mirrors canela. The mesh tiers do not depend on this. Splats are the
optional photoreal/scientific tier, built when an asset class or a compute consumer earns
them.

---

## See also

- Foundation contract & the two seams — [`CajetaGPU.md`](CajetaGPU.md) (esp. §1.2–1.5, §4).
- Value types used here — [`ValueTypeCatalog.md`](ValueTypeCatalog.md),
  [`Quaternions.md`](../math/Quaternions.md), [`MaskSelect.md`](MaskSelect.md).
- Storage images / ray query the splat paths build on — [`WritableImages.md`](WritableImages.md),
  [`RayQuery.md`](rayquery/RayQuery.md).
- The virtual-geometry consumer — `../../plans/gpu/gfx/canela-plan.md`.
- Research library (PDFs) — `../../plans/gpu/gfx/research/gfx/splats/`.
