# cajeta.gfx — Graphics Primitives Spec

**Status:** Draft for review · 2026-06-18 · supersedes the framing in
`plans/gpu/gfx/cajeta-gfx-plan.md` and `docs/gpu/gfx/CajetaGFX.md` (both drifted).

This is a requirements document, covering the what and the why. It does not enumerate
implementation tasks, because that is the job of the plan derived from it.

---

## 0. Why this spec exists (the drift)

The GPU/XPU foundation matured. `cajeta.xpu` now ships value types, the buffer/memory
model, textures + writable images, acceleration structures, inline ray query (mesh + AABB),
waves/atomics/barriers, cooperative-matrix tensor-core matmul, and `cajeta.math`
(`Tensor`/`Matrix`/`DType`). In the process the definition of `cajeta.xpu.gfx` drifted
between two documents:

- **`docs/gpu/CajetaGPU.md` (current, authoritative)** frames the graphics facet as
  *"rasterization, the render graph, the ray-tracing pipeline + basic graphics algorithms —
  the **primitives an engine dev composes, not an engine**."*
- **`docs/gpu/gfx/CajetaGFX.md` + `plans/gpu/gfx/cajeta-gfx-plan.md` (older)** frame gfx as
  *"the graphics/rendering layer **and the game engine built on top**"*, and then they enumerate
  a full opinionated engine with a render graph, forward/deferred + PBR, lighting/shadows,
  post-processing, ECS, scene graph, glTF/FBX import, particles, physics, audio, editor, and
  packaging (Parts G3–G6).

Those two framings are incompatible. This spec adopts the foundation doc's framing and
makes it precise. `cajeta.xpu.gfx` is the unopinionated graphics-primitive layer, meaning the
framework-neutral building blocks a game engine needs, and nothing above that line. The
opinionated engine (a chosen renderer, a GI system, ECS, physics, audio, an editor) lives in
separate libraries built on these primitives, and it never lives in stdlib gfx.

---

## 1. Definition & boundary

### 1.1 What gfx *is*

> `cajeta.xpu.gfx` carries primitives that tightly wrap the GPU's graphics capabilities,
> plus the small, framework-neutral algorithms that every engine re-derives. It is the
> graphics sibling of `cajeta.xpu.xpu` (compute) over the shared `cajeta.xpu` foundation.

```
cajeta.xpu                       (foundation — value types & math, buffers, textures,
   ▲            ▲                 acceleration structures, inline ray query, waves/atomics)
   │            │
cajeta.xpu.xpu  cajeta.xpu.gfx   ← this spec       (foundation libs on cajeta.xpu:
(compute prims) (graphics prims)                    cajeta.xpu.splat, cajeta.math)
                   ▲
                   │  separate libraries, NOT stdlib:
            Glorias (the PoC engine: scheduler, renderer, scene) · GI system
            · canela (virtual geometry) · physics · ECS · editor
```

The PoC engine is named `Glorias` (§6.1). The opinionated layer (frame scheduler,
renderer, scene model) lives in its own repo, built on these primitives.

The governing split is that shared primitives go to the foundation and graphics-only primitives go to gfx. A primitive used by both
siblings, compute and graphics, belongs in the foundation (`cajeta.xpu` or a foundation
library like `cajeta.xpu.splat`). gfx holds only what is graphics-only. This is
the rule that puts splats in `cajeta.xpu.splat`, since gfx rasterizes them and xpu scatters them for
SPH/MPM/tomography (`docs/gpu/splats.md`), the same way `AccelerationStructure`+`RayQuery`
already sit in the foundation. It also pulls the graphics math down into `cajeta.math`
(§4.1) and flags the geometry-processing toolbox (QEM/MC/TSDF) as foundation-leaning (§8.7).
gfx's genuine residents are the graphics-only primitives, namely the rasterization/RT pipeline,
BRDF lobes, the visibility-buffer software rasterizer, virtual texturing, camera/cull, and the
render-graph passes that compose foundation nouns.

Three rules carry over from the foundation contract (`CajetaGPU.md`) and bind every primitive
here:

1. **Write-once-run-everywhere.** Each primitive has a portable default correct on every
   backend, and a native fast path where the silicon has it, bit-exact cross-checked
   against the default. There are no `cajeta.gfx.vulkan` / `.nvidia` packages, because vendor-exclusive
   silicon lives in external vendor libraries.
2. **Two seams, verbs and nouns.** A graphics capability is a call on a datastructure. The
   verb lowers through `LoweringTarget`. The noun is built from a description and never
   transcoded from a vendor blob, since a swapchain, a graphics pipeline, and a BVH are opaque driver
   artifacts each backend builds from our description.
3. **RAII ownership.** Swapchains, pipelines, framebuffers, vertex/index buffers, page caches, and
   acceleration structures are ordinary owned values, and their lifetime is the scope-exit drop chain.

### 1.2 The IN / OUT line (the heart of this spec)

A primitive belongs in `cajeta.xpu.gfx` iff it satisfies all of:

- **Capability-wrapping or universally-rederived.** It either wraps a hardware graphics
  capability (a shader stage, a draw, a sampler) or is an algorithm every engine rebuilds
  regardless of art direction (BVH build, QEM simplify, a GGX lobe, mip generation).
- **Opinion-free.** It imposes no scene model, no material model, no frame structure, and no
  asset format. It is a tool, and it carries no policy.
- **Composable & value-semantic where possible.** It is a pure function or a POD value type that
  runs identically on CPU and GPU, or otherwise an owned resource with an explicit lifetime.

If a thing instead encodes how an engine is built, meaning which renderer, which GI algorithm, how
the scene is structured, or how assets are authored, it is OUT and belongs in a library on
top. §6 lists the OUT set explicitly.

---

## 2. Goals

- **G-1. Close the graphics-capability gap.** The foundation is compute-only today
  (`GLCompute` entry points, `vkCmdDispatch`; no graphics pipeline). Add the graphics
  execution model the foundation lacks, meaning shader stages → SPIR-V, interface-variable lowering,
  swapchain/present, render passes, pipeline state, and draws. This is the one genuinely new
  lowering surface, and everything else reuses the foundation.
- **G-2. Ship the framework-neutral algorithm toolbox.** This is the toolbox the SIGGRAPH/HPG literature has
  settled (BVH, software raster/visibility buffer, mesh simplification & meshletization,
  marching cubes, BRDF/BSDF lobes, mip/anisotropic filtering, virtual-texture page cache, SDF
  + sphere-trace, noise, splat rasterization, sampling/RNG), with each item as a portable primitive an
  engine composes.
- **G-3. Be the substrate the opinionated layers stand on.** `canela` (virtual geometry), a
  future renderer, a GI system, and a physics library must all be buildable entirely from
  gfx and foundation primitives, with no primitive siloed in a consumer. The canela plan
  already flags QEM, marching cubes, the sparse-voxel/TSDF grid, and the page cache for
  promotion here.
- **G-4. Keep stdlib opinion-free.** Draw and hold the IN/OUT line (§1.2, §6) so cajeta never
  ships a framework stance in its own stdlib.
- **G-5. One representation across paths.** A primitive emitted by one path is consumable by
  the others. For example, the capture/oriented-point set feeds mesh, impostor, and splat encoders, and
  the page cache serves both virtual geometry and virtual texture.

### Non-goals (v1)
- No chosen renderer, GI system, denoiser, or material/scene model (§6).
- No differentiable rendering or splat training. Splat and radiance-field rendering/encode are
  in, and training is a separate concern. The lean is that it is out for v1.
- No ECS, asset import, physics solver, audio, windowing/input event system, or editor (§6).

---

## 3. The current foundation gfx builds on (inventory, for grounding)

The primitive modules below are scoped against what already exists and what gfx adds:

| Foundation (`cajeta.xpu`, present today) | Relevance to gfx |
|---|---|
| `Vector<T,N>` (compiler builtin), `Matrix<T,R,C>` (`cajeta.math`) | transform math — **but no `Quaternion`/`Transform`/bounding volumes/`Frustum`/`Ray` yet** (gap, §4.1) |
| `KernelBuffer<T>`, `MemoryKind`, `KernelStream`, `Fence`/`Event` | vertex/index/uniform buffers, frame sync |
| `Texture1D/2D/3D/Cube`, `Texture2DArray`, `Sampler`, `TextureFormat` | sampling exists; **mip-gen, anisotropic, virtual-texture cache are gfx** |
| `Image2D` (writable storage image) | render-target / visibility-buffer write surface |
| `AccelerationStructure` (AABB + triangle), `RayQuery`, `SoftwareRayQuery`, `AsImpl`, `Capability` | **inline ray query is GPU-owned**; the **RT *pipeline* (raygen/hit/miss/SBT) is gfx-owned** (the GPU↔GFX seam) |
| `Wave`, `Quad`, `Barrier`, atomics, `Workgroup`, `KernelThread` | software rasterizer, BVH build, sort, reductions |
| `CooperativeMatrix`, `CoopStage` (`cajeta.xpu.xpu`) | compute-only; gfx does **not** use them |
| `cajeta.math`: `Tensor<T>`, `Matrix`, `DType`, fp16/bf16/fp8 element types | shared numeric substrate; FFT/Poisson-grade solvers are math-side |

It is confirmed that there is no graphics/rasterization/rendering code yet, with no vertex/fragment
stages, no swapchain, no render pass, and no draw. Part GP-1 below is all forward work.

---

## 4. Primitive modules (the gfx surface)

Each module states the capability, the use case (why), and its research grounding.
The papers are now local under `research/sigraph/papers` and `plans/gpu/gfx/research/{canela,gfx/splats}`.

### 4.1 Graphics math & spatial primitives  *(value types — these live in `cajeta.math`)*
- **Decision (resolved).** The small fixed-size math types are `cajeta.math`, beside the
  existing `Matrix<T,R,C>`. They are in neither `cajeta.xpu` nor gfx. `cajeta.math` is the
  backend-agnostic, CPU-first math package, and these are pure algebra and belong with `Matrix`.
  They are confirmed absent today, since only `Matrix`/`Tensor`/`DType` exist, so this is all forward work.
- **`cajeta.math` adds** `Quaternion`, `Transform` (TRS), affine / projection / view-matrix
  builders (`perspective`/`ortho`/`lookAt`), `Vector3/4` ergonomics over the builtin
  `Vector<T,N>`, the geometric value types `Aabb`, `Sphere`, `Plane`, `Ray`, `Frustum` and their
  intersection/containment tests, and color value types with sRGB↔linear.
- **gfx adds only the rendering-semantic wrappers** that compose those math types, namely `Camera`
  (view/projection policy) and frustum/AABB culling helpers. The raw algebra stays in math.
- **Why.** Every transform, cull, pick, and projection needs these. They are pure, branch-light, and
  CPU==GPU identical, which is exactly `cajeta.math`'s remit.

### 4.2 Graphics pipeline & presentation  *(the new capability — Part GP-1)*
- **Capability.** The graphics execution model the compute path does not have:
  - shader stages → SPIR-V via a per-stage target triple (`…-vulkan1.3-{vertex,pixel,…}`),
    one module/`.spv` per stage, with a `@Vertex`/`@Fragment`/… annotation surface parallel to
    `@Kernel`;
  - interface-variable lowering, with inputs/outputs as addrspace-7/8 globals carrying
    `Location`/`BuiltIn` decorations (vertex attrs, `gl_Position`, varyings, fragment outs), which is
    the genuinely new lowering surface;
  - uniform/storage buffers and push constants, reusing the descriptor model;
  - swapchain with surface/format/colorspace selection and present;
  - render passes / framebuffers (or dynamic rendering), and pipeline state (vertex input, input
    assembly, raster, blend, depth/stencil, viewport/scissor);
  - command recording for draws, frames-in-flight, and sync, with `vkCmdDraw`/`DrawIndexed` and
    vertex/index binding, plus geometry/tessellation and mesh + task shader stages, since the
    triple/attr knob generalizes.
- **Why.** Without this there is no rasterized frame at all, so it is the prerequisite for every
  consumer. The emitter feasibility is already proven, since `test/gfx/GfxSpirvEmitProbeTests.cpp`
  emits `spirv-val`-clean Vertex+Fragment via the in-tree backend with no glslang.
- **Grounding.** Vulkan 1.3 graphics and the mesh-shader model. The software-geometry pipeline
  (Kenzel 2018) informs the optional fully-programmable path.

### 4.3 Ray-tracing pipeline  *(gfx-owned half of the GPU↔GFX seam)*
- **Capability.** The raygen / closest-hit / any-hit / miss / intersection / callable execution
  models, which extend 4.2's per-stage triple, ray-payload and hit-attribute storage classes,
  `OpTraceRayKHR`, and the shader-binding-table runtime with TLAS/BLAS build/refit and instance
  transforms over the foundation `AccelerationStructure`.
- **Why.** Inline ray query in the GPU foundation covers shadows/AO/picks inside fragment/compute, and
  the pipeline covers full path tracing and the SBT programming model. The seam is explicit.
  GPU owns inline ray query, and GFX owns the RT pipeline.
- **Grounding.** Vulkan KHR ray-tracing-pipeline / DXR / OptiX. Clustered geometry and the
  partitioned TLAS (CLAS, PTLAS, RTX Mega Geometry) are their own module, 4.11, because inline ray
  query traces them too. There is an LLVM dependency. These execution models are Tier-3-absent in
  upstream LLVM, so they ride the downstream-fork pipeline, and this module is sequenced behind
  4.2 and behind inline ray query, exactly as the old plan noted.

### 4.4 Mesh & geometry-processing primitives
- **Capability.** A mesh container (vertex/index buffers and attribute layouts); meshlet/cluster
  partitioning; the cluster-DAG LOD structure (locked shared edges, per-group error
  bounds); QEM edge-collapse simplification (with appearance-preserving terms); marching
  cubes; a sparse-voxel / TSDF grid with the fuse/space-carve update; sparse-voxel octree
  (SVO); displaced-micro-mesh encode/decode.
- **Why.** These are the framework-neutral geometry ops every LOD/remeshing/reconstruction
  system rebuilds. The `canela` plan explicitly asks for QEM, marching cubes, and the
  sparse-voxel/TSDF grid to be promoted here so canela and physics/collision share one
  implementation.
- **Grounding.** Garland–Heckbert 1997 (QEM), Cohen 1998, Hoppe 1996/97, Cignoni 2005 (Batched
  Multi-Triangulation), Yoon 2004 (Quick-VDR), Jakob 2023 (meshlet strategies), Lorensen–Cline
  1987 (MC), Curless–Levoy 1996 (TSDF), Laine–Karras 2010 (ESVO), Maggiordomo 2023 (DMM),
  Mlakar 2024 (compressed meshlets), Pettett 2024 (multiresolution mesh engine).
- **`Geometry` trait (resolved, Q3, my recommendation).** Define a `Geometry` trait from
  the start, in the noun-seam shape `CajetaGPU.md` already uses, which is build-from-description plus
  query verbs (bounds, ray-intersect, rasterize/extract, LOD-select) with the representation hidden.
  Commit v1 to exactly one conforming backend, the triangle-cluster-DAG, which is the proven
  path canela needs. SVO, displaced-micro-mesh, and the splat cloud (`cajeta.xpu.splat`,
  already a foundation noun) conform to the same trait as each earns its place. Each is another
  implementor, and none is a second pipeline. The rationale is that the trait is nearly free to declare, stops
  consumers (canela, renderer, physics) from hard-coding triangle assumptions, and matches the
  foundation's "the noun's chosen implementation determines the verb's lowering" rule.
  Implementing all four backends in v1 is not warranted.
- **Placement note (shared → likely foundation, flag).** QEM / marching-cubes / TSDF-voxel-grid
  are used by compute (reconstruction), physics (collision), and graphics, and the
  canela plan already says "promote to `cajeta.xpu`/stdlib where other consumers can reuse."
  By the §1.3 principle below they lean foundation, with gfx holding only the
  graphics-semantic composition. They are marked in the foundation-vs-gfx pass (§8.7).

### 4.5 Acceleration structures & software rasterization
- **Capability.** Software BVH build (LBVH via Morton + radix sort, HLBVH, PLOC/H-PLOC) and
  traversal (stack/stackless, quantized/wide BVH8, ray-stream/packet) over the foundation AS
  noun, and a software rasterizer built on the foundation's 64-bit atomics, namely the visibility
  buffer (`atomicMin` on packed depth|primitive-id) for pixel-sized triangles and point/splat
  compositing.
- **Why.** Even with hardware RT, software BVH is needed for custom primitives, baking, AMD
  parity, and CPU fallback. The visibility buffer is the core primitive of virtualized-geometry
  and 2-billion-point rendering. Both are pure GPU-codegen showcases.
- **Grounding.** Pantaleoni 2010 (HLBVH), Schütz 2022 (2B points sw-raster), DOBB-BVH 2025,
  quantized-BVH/ray-stream 2025, Karras parallel-BVH lineage.

### 4.6 Texturing primitives (beyond foundation sampling)
- **Capability.** Mipmap generation; high-quality anisotropic / EWA filtering helpers;
  a generic `PageCache<TileKey,TileData>` with feedback-driven residency and LRU eviction;
  virtual-texture page-table indirection and sparse residency; toroidal/clipmap addressing.
- **Why.** Texture memory is a hard ceiling for large worlds. The page cache is the shared
  residency engine virtual geometry and virtual texture both stream through, one generic with
  two consumers. Sampling itself already exists in the foundation.
- **Grounding.** Williams 1983 (mipmaps), Heckbert 1986 / McCormack 1999 (EWA/Feline),
  van Waveren 2012/13 + idTech5 (software VT), Ka Chen 2015 (adaptive VT), Taibo 2009,
  Losasso–Hoppe 2004 (clipmaps), Hollemeersch 2010, Zhang 2021 (adaptive streaming).

### 4.7 Shading primitives  *(pure functions)*
- **Capability.** The BRDF/BSDF lobe library, meaning Cook–Torrance/GGX with height-correlated
  Smith, Schlick Fresnel, multiscatter energy compensation, VNDF importance sampling, Disney
  principled diffuse/sheen/clearcoat, and the BSDF transmission/SSS extension, each with
  `evaluate` and `sample`/`pdf` for MIS; color management (sRGB/linear, tonemap operators);
  spherical-harmonic and octahedral encode/decode.
- **Why.** Every shading path (raster, RT, GI, splat) needs the same correct lobes.
  Side-effect-free `(wo,wi,params)` functions are the most reused, lowest-level graphics module
  and a great LLVM math-codegen test (FMA/vectorization). This is a lobe library. A
  material system is opinion, so the material model is OUT.
- **Grounding.** Burley 2012 (Disney BRDF), Burley 2015 (Disney BSDF), Boksansky 2021 (crash
  course BRDF).

### 4.8 Sampling, RNG & Monte-Carlo primitives
- **Capability.** Deterministic per-pixel/per-lane RNG (PCG/xoshiro); low-discrepancy
  sequences (Sobol/Halton with scrambling); the reservoir value type (`{sample,wSum,M,W}`,
  WRS one-pass) and RIS/MIS helpers; quasi-Monte-Carlo importance-sampling utilities.
- **Why.** This is the shared substrate under path tracing, ReSTIR, DDGI, splat optimization, and
  procedural placement. The reservoir is a tiny POD value type ideal for SoA GPU buffers, and the
  primitives are policy-free even though specific reuse schemes (ReSTIR DI/GI) are OUT.
- **Grounding.** Bitterli 2020 (ReSTIR DI), Lin 2022 (GRIS), and Wyman 2023 (course) for the
  reservoir/RIS primitives. The systems built on them are OUT.

### 4.9 Radiance-field & point primitives  *(FOUNDATION — `cajeta.xpu.splat`, NOT gfx)*
- **Decision (resolved).** Splats are foundation. The design already exists, since
  `docs/gpu/splats.md` specifies `cajeta.xpu.splat`, a foundation library on `cajeta.xpu`.
  The rationale is that a splat is a scatter-accumulate of an anisotropic kernel. There is no
  fixed-function splat silicon, so it decomposes entirely into foundation verbs (atomics,
  sort/scan, `Vector`/`Quaternion` math, `Image2D` RMW, `AccelerationStructure` ray query).
  It has two sibling consumers, gfx with `splatRasterize` (tile-based EWA rasterization) and
  xpu with `splatScatter` (SPH/MPM particle→grid, tomographic back-projection, RBF). Siloing it
  in gfx would force every scientific consumer to reimplement it, which is the exact anti-pattern the
  foundation exists to prevent. It has the same shape as `AccelerationStructure`(noun)+`RayQuery`(verb).
- **What gfx contributes.** Only the render-graph integration. `splatRasterize` runs as a
  gfx render-graph pass, composited with mesh draws via a shared depth buffer for correct
  occlusion (`docs/gpu/splats.md` §5.5). The `SplatCloud` noun, the rasterizer/scatter verbs,
  the EWA/Mip-Splatting math, the `.cajsplat` codec, and the LOD octree all live in
  `cajeta.xpu.splat`. gfx imports them and does not define them.
- **Grounding.** Kerbl 2023 (3DGS), Zwicker 2001 (EWA), Yu 2023 (Mip-Splatting), Ren 2024
  (Octree-GS LOD), Pfister 2000 / Rusinkiewicz–Levoy 2000 (surfel/QSplat). The full bibliography is
  in `docs/gpu/splats.md` over `plans/gpu/gfx/research/gfx/splats/`.

### 4.10 Field & signal primitives
- **Capability.** A signed-distance-field grid and sphere-trace primitive; 3-D
  procedural noise (Perlin/Worley/simplex/value); octahedral environment encode. FFT,
  MAC-grid Poisson solve, and other heavy numeric solvers are `cajeta.math`, and they are used here
  without being duplicated.
- **Why.** SDF with sphere-trace is the shared geometry-query primitive under soft shadows, AO,
  GI fallback chains, and CSG, and noise underpins clouds, terrain, and detail. The effects built
  on them (a cloud system, a fluid solver, a Lumen-like GI) are OUT.
- **Grounding.** Laine–Karras 2010 (SVO/SDF lineage). The cloud/fluid/GI papers in
  `research/sigraph` are grounding for downstream consumers rather than for the primitives here.

### 4.11 Clustered geometry & partitioned top-level structures
- **Capability.** Two acceleration-structure forms beyond one BLAS per mesh and one TLAS over
  instances. A **cluster acceleration structure (CLAS)** is a small hierarchy over one triangle
  cluster, built in bulk from cluster descriptions, and a BLAS is assembled from a list of CLAS
  references rather than from raw triangles. A **cluster template** is a CLAS built once from a
  cluster's topology and instantiated many times with new vertex positions, which is how animated or
  displaced clusters are rebuilt cheaply. A **partitioned TLAS (PTLAS)** divides the top level into
  partitions of instances, so a frame rewrites only the instances and partitions that changed, plus
  a global partition for instances that span the scene. Builds take their arguments from device
  memory, so a GPU pass can decide each frame which clusters and instances to build without a host
  round trip. The per-cluster limits (triangles, vertices) are queried from the device, never
  assumed.
- **Why.** The cluster DAG of 4.4 and the streaming geometry of `xpu-gfx-streaming-geometry` change
  the geometry every frame: an LOD cut swaps clusters, and streaming adds and drops instances. Tracing
  rays against that geometry with whole-BLAS rebuilds costs a full build of every changed mesh, and
  a whole-TLAS rebuild costs every instance, however few moved. Clusters make the rebuild
  proportional to the clusters that changed, and partitions make the top level proportional to the
  instances that changed. Without both, ray-traced shadows, reflections and GI cannot follow a
  virtualized-geometry scene, and the rasterized and traced views of one frame disagree.
- **Ownership.** By the §8.7 rule (shared ⇒ foundation), building and refitting a CLAS, a cluster
  template, a cluster BLAS and a PTLAS belongs to `cajeta.xpu`, as new forms of the
  `AccelerationStructure` noun. Inline ray query traces them as it traces any BLAS or TLAS, so a
  compute consumer gets them too. gfx owns the composition: turning a cluster-DAG LOD cut into CLAS
  builds, assigning streamed instances to partitions, and keeping the traced cut equal to the
  rasterized one.
- **Portability.** The hardware path is NVIDIA's (RTX Mega Geometry: `VK_NV_cluster_acceleration_structure`,
  `VK_NV_partitioned_acceleration_structure`, and the OptiX 9 cluster API). Other vendors have no
  equivalent today. There the same noun builds one BLAS per cluster group, or falls back to the
  software BVH of 4.5. The capability query names which path a device took, and the traced result is
  the same either way; only the build cost differs.
- **Grounding.** NVIDIA RTX Mega Geometry (2025) and the two Vulkan extensions above; the OptiX 9
  programming guide's cluster acceleration structures; Karis 2021 (Nanite) for the cluster-DAG
  consumer.
- **Use cases.**
  1. A virtualized-geometry renderer traces shadow rays against the same LOD cut it rasterized this
     frame, rebuilding only the clusters the cut changed.
  2. A skinned or displaced mesh rebuilds its clusters each frame from one cluster template per
     cluster, with new vertex positions and no topology rebuild.
  3. An open-world scene streams instances in and out each frame and updates only the partitions
     they belong to, while the rest of the TLAS stays built.
  4. A compute consumer (a visibility or sensor simulation) traces inline ray queries against
     clustered geometry with no gfx dependency.
  5. The same scene runs on a device without the cluster extensions, through the per-group BLAS or
     software path, and the capability query reports that it did.

---

## 5. Use cases (who composes these, and how)

1. **`canela` (virtual geometry & textures)** is the reference consumer. It composes 4.4
   (meshlet/cluster-DAG, QEM, MC, TSDF), 4.5 (BVH + visibility buffer), 4.6 (page cache +
   virtual texture), 4.3/inline-ray-query (capture), 4.9 (splat far-field). Canela stays its
   own spec on top, and this spec is what makes its "promote shared pieces to the foundation"
   requirement real.
2. **A renderer** (separate lib) composes 4.2 (pipeline), 4.7 (BRDF), 4.1 (camera/cull) into
   a forward/deferred/visibility-buffer path of its choosing.
3. **A GI / path-tracer** (separate lib) composes 4.3 (RT pipeline) or inline ray query,
   4.8 (sampling/reservoirs), 4.7 (BRDF), 4.10 (SDF) into ReSTIR/DDGI/Lumen-class systems.
4. **A physics/collision lib** composes 4.5 (BVH broad/narrow phase), 4.4 (mesh), and 4.1
   (spatial volumes).
5. **Data-viz / 2-D UI** composes 4.2 (pipeline) and 4.6 (texturing) without any 3-D engine.

The acceptance bar for the primitive set is that each consumer above is expressible with no
primitive defined inside it. If a consumer needs to define a graphics primitive privately,
that primitive was missing from gfx.

---

## 6. Explicitly OUT (opinionated — separate libraries on top)

These were in the old gfx plan (Parts G3–G6) and are removed from stdlib gfx. They are
legitimate libraries, and they are never stdlib primitives:

- **A chosen renderer.** A forward/deferred path, a PBR material system, light/shadow
  techniques, and a post-processing stack (tonemap/bloom/TAA/SSAO). This lives in Glorias (§6.1).
- **Render/frame graph split (resolved, Q2).** gfx ships a minimal pass-dependency primitive
  that declares passes and their resource reads/writes, and derives the execution order and the barriers between
  them. The opinionated scheduler, covering transient-resource aliasing, memory-budget packing,
  async-compute overlap policy, and the whole frame structure, lives in Glorias and never in stdlib.
- **A GI system.** Lumen / DDGI / ReSTIR / Radiance Cascades / NRC, which compose 4.3/4.7/
  4.8/4.10.
- **Denoisers.** SVGF / ReBLUR / neural, which compose the foundation and 4.8.
- **Scene & content.** ECS, scene graph / transform hierarchy, glTF/FBX import, virtual
  filesystem, serialization, and particle systems.
- **Simulation.** Fluid/smoke/cloth/rigid-body solvers, which compose 4.10 and `cajeta.math`, and
  animation/skinning systems.
- **Platform.** Windowing, input, and audio. These are outside gfx and outside core stdlib. They are
  external, optional, capability-gated libraries over vendor-versioned OS APIs, which makes them a runtime
  HAL rather than an LLVM target. gfx only consumes the `Surface` they supply. The full resolution is in §9.
- **Tooling.** Editor, frame debugger, asset/content build, and packaging.
- **Vendor-exclusive.** NV cooperative-vector/TMA, AMD MFMA graphics paths, and Metal-native. These are
  external vendor libraries, never stdlib.

### 6.1 The PoC engine — `Glorias`

The proof-of-concept graphics engine is `Glorias`. It is the opinionated layer that proves the primitives
compose, and it lives in its own repository, built on `cajeta.xpu.gfx` and the foundation.

> **Why the name.** The name is a triple fit with the Cajeta dessert/optics theme. (1) A gloria is a real
> atmospheric-optics phenomenon, concentric coloured rings of light around a shadow, which makes it a
> graphics/light phenomenon. (2) Glorias are a traditional Mexican confection made from
> cajeta (goat-milk caramel) and pecans, so the name is taken in its exact plural form, an
> engine built from Cajeta, mirroring the candy built from cajeta. (3) It means glories, meaning
> visual splendour, which fits a renderer.
> It is a sibling to `canela`, cinnamon, the thin layer dusted on top. *If you would rather, the alternative is
> `Oblea`, the thin wafer cajeta is served on.*

Glorias owns everything in §6, meaning the opinionated frame scheduler (transient aliasing, budget
packing, async-compute policy), a concrete renderer (forward/deferred and a PBR material model),
a scene model, and the asset glue. None of those belong in stdlib. It is the first real
consumer that validates the gfx primitive set (§5's acceptance bar). *The repo is not created yet
("we'll create a new repo for it"). This spec just fixes the name and the boundary.*

---

## 7. Sequencing & dependencies (informative — the plan will detail)

1. **4.1 math/spatial** and **4.2 graphics pipeline** come first. Nothing renders without them, and 4.2's
   emitter feasibility is already proven.
2. **4.7 BRDF**, **4.8 sampling**, **4.5 BVH/sw-raster**, and **4.6 texturing/page-cache** are the
   pure-function and data-structure toolbox. They are parallelizable, and each is independently testable.
3. **4.4 geometry processing** promotes QEM/MC/TSDF/meshlet here, which unblocks canela.
4. **4.9 splats/points** and **4.10 SDF/noise** are composable additions.
5. **4.11 clustered geometry and the partitioned TLAS** come after 4.4's cluster DAG and the
   foundation's instance TLAS. They need neither 4.2 nor 4.3, since inline ray query traces them.
6. **4.3 RT pipeline** comes last and is forked-LLVM-gated, behind inline ray query.

One rule is cross-cutting. gfx never imports `cajeta.xpu.xpu`, so there is no cooperative-matrix/compute-execution
dependency, and where gfx wants a compute pass it uses `cajeta.xpu` device/dispatch directly.
The package rename (`cajeta-gfx` → `cajeta.xpu.gfx` nested stdlib) lands with the first
graphics work, since building the second foundation consumer is what validates the shared seam.

---

## 8. Question resolutions & remaining items

Resolved in review (2026-06-18):
1. **Math placement (§4.1). RESOLVED.** `Quaternion`/`Transform`/`Vector3-4`/projection
   builders/`Aabb`/`Sphere`/`Plane`/`Ray`/`Frustum`/color go to `cajeta.math`, beside `Matrix`.
   gfx keeps only `Camera` and cull helpers.
2. **Render graph (§6). RESOLVED.** The minimal pass-dependency and barrier-derivation primitive is
   IN gfx, and the opinionated scheduler is OUT, in `Glorias`.
3. **Geometry trait (§4.4). RESOLVED.** Define the `Geometry` trait now, and ship one backend
   (triangle-cluster-DAG) in v1. SVO / micro-mesh / splat conform later.
4. **Splats (§4.9). RESOLVED.** They go to the foundation `cajeta.xpu.splat` (`docs/gpu/splats.md`),
   including the optional differentiable train/fit path, which stays in the splat foundation lib and is
   deferred. gfx only integrates `splatRasterize` as a render-graph pass.
6. **2-D / UI (§5). RESOLVED, lean accepted.** 2-D/UI is a consumer of the pipeline,
   texturing, and math primitives. The code comparison is in Appendix A.

Resolved on review (cont.):
5. **Platform line. RESOLVED.** gfx owns swapchain and present, a real GPU object, and defines
   a thin opaque `Surface` interface it consumes. Window / input / audio are out of gfx
   and out of core stdlib, living as external optional capability-gated libraries, which are a runtime
   HAL over vendor-versioned OS APIs and never something LLVM abstracts. The full architecture and the
   single-library-vs-per-OS decision are in §9.
7. **Foundation-vs-gfx classification. RESOLVED.** The splat decision sets the rule
   "shared ⇒ foundation." Applied to the rest:

   | Module | Used by | Proposed home |
   |---|---|---|
   | 4.1 graphics math | everything | **`cajeta.math`** ✓ resolved |
   | 4.2 graphics pipeline + present | graphics only | **gfx** |
   | 4.3 RT pipeline | graphics only (GPU↔GFX seam) | **gfx** |
   | 4.4 mesh container, meshlet/cluster-DAG, `Geometry` trait | graphics-leaning | **gfx** |
   | 4.4 QEM / marching-cubes / TSDF-voxel-grid | compute + physics + graphics | **foundation** (`cajeta.xpu`) — *flag* |
   | 4.5 software BVH build/traverse | compute + graphics (shared w/ ray query) | **foundation** — *flag* |
   | 4.5 visibility-buffer software rasterizer | graphics only | **gfx** |
   | 4.11 CLAS / cluster template / cluster BLAS / PTLAS build | compute + graphics (shared w/ ray query) | **foundation** (`cajeta.xpu`, the `AccelerationStructure` noun) |
   | 4.11 cluster-DAG → CLAS, instance → partition composition | graphics only | **gfx** |
   | 4.6 `PageCache<K,V>` generic residency | geometry + texture streaming (shared) | **foundation** — *flag* |
   | 4.6 virtual-texture / mip-gen / anisotropic | graphics only | **gfx** |
   | 4.7 BRDF/BSDF lobes, color, SH, octahedral | graphics only | **gfx** |
   | 4.8 RNG / low-discrepancy / reservoir | compute + graphics (Monte-Carlo) | **foundation** — *flag* |
   | 4.9 splats | graphics + compute | **`cajeta.xpu.splat`** ✓ resolved |
   | 4.10 SDF grid + sphere-trace, noise | compute + physics + graphics | **foundation** — *flag* |

   **RESOLVED, accepted.** The five flag rows (4.4 QEM/MC/TSDF, 4.5 software BVH, 4.6
   `PageCache`, 4.8 RNG/low-discrepancy/reservoir, 4.10 SDF+sphere-trace+noise) move out of gfx
   into the foundation (`cajeta.xpu` / foundation libs), per the "shared ⇒ foundation" rule.
   gfx's true surface is therefore the graphics pipeline with present, the RT pipeline,
   mesh/`Geometry`/meshlets, the visibility-buffer software raster, virtual-texture with mip/anisotropic,
   BRDF/BSDF with color/SH/octahedral, camera/cull, and the minimal render-graph passes.
   Everything else is foundation it composes. The plan derived from this spec sequences the
   foundation promotions ahead of their gfx consumers.

---

## 9. Platform layer — window / input / audio

These are needed to ship a game, but they are neither graphics primitives nor GPU
capabilities, so they are neither in gfx nor in core stdlib. This section fixes what they are
and how they are packaged.

### 9.1 Why LLVM does not abstract them (codegen vs. runtime HAL)

LLVM abstracts codegen targets. It lowers one source to NVPTX / AMDGPU / SPIR-V / x86 /
ARM. GPU compute rides it because a kernel is code to be lowered. Window/input/audio are
OS services called through the C ABI, such as `CreateWindowExW`
(user32), `wl_compositor_create_surface` (libwayland), `[NSWindow initWithContentRect:]`
(AppKit), `IAudioClient` (WASAPI), and `AudioQueue` (CoreAudio), and there is no computation in them to lower. There is no "windowing IR." LLVM
compiles the call sites like any native code, but it has nothing to abstract. The abstraction
is a set of external symbols bound via FFI at link/runtime, which is a runtime HAL rather than a
compile target. This is the same line Cajeta already draws inside GPU. The kernel is
LLVM-lowered, and the driver that launches it (`runtime/native/`) is FFI to a vendor runtime.

| | Abstracted by | one source → | vendor versioning |
|---|---|---|---|
| GPU **compute** (kernels) | **LLVM** (codegen) | NVPTX / AMDGPU / SPIR-V | code is *generated* |
| GPU **present / WSI** | FFI runtime | `VK_KHR_{win32,wayland,xcb,metal}_surface` | per-OS Vulkan extensions |
| **window / input / audio** | FFI runtime HAL | Win32 / Wayland+X11 / Cocoa · WASAPI / PipeWire / CoreAudio | **fully** |

The consequence is that there will be separate per-platform backend code, each tracking a vendor's
versioning independently (Microsoft Win32/WASAPI/GameInput; Apple AppKit/CoreAudio with deprecation
churn; Linux Wayland/X11 with ALSA→PulseAudio→PipeWire and libinput; GPU-driver WSI). None of it is
compiled away. It is maintained as versioned FFI bindings, quarantined in these libraries so
the churn lives in one place instead of being smeared across every app/engine. The HAL job is what SDL3 /
GLFW / sokol already do in C. Cajeta either FFI-wraps one or writes native backends, and either way
it is FFI plus vendor versioning rather than an LLVM target.

### 9.2 The API is identical per platform; only the backend differs

There is one portable contract (`Window`/`Surface`/`Event`, `AudioStream`, `InputDevice`), and app code is
written once. Per-platform backends are selected at build/runtime, with the same
write-once-run-everywhere / opaque-noun discipline as `cajeta.xpu`. The `Surface` is opaque,
so `HWND` vs `wl_surface` vs `CAMetalLayer` is never inspected, and the gfx swapchain backend pairs it
with the matching Vulkan WSI extension. Genuinely divergent OS behaviour (macOS main-thread event
loop, Wayland self-positioning ban, DPI/HDR models) is covered by one portable model with rare
capability-gated escape hatches, under the same rule as "no `cajeta.xpu.nvidia` in stdlib."

### 9.3 Packaging — `ifx` facade (stdlib) + per-OS backends + the `ifx-backend` melt

The decision is a stdlib facade, external backends, and a required melt. It is neither one fat
cross-OS library nor a set of naked per-OS libraries the developer juggles.

1. **`ifx`, the contract facade, in STDLIB.** One cross-platform, pure-Cajeta, no-FFI API
   (`Window`/`Surface`/`Event`, `AudioStream`, `InputDevice`) for all three domains. Because it
   is stdlib it is built into the toolchain, never declared and never fetched, like
   `cajeta.lang`/`cajeta.io`, so every app compiles against it for free. The name `ifx` stands for
   "interface framework". It also carries the null/headless floor (§9.4 case 3).
2. **Per-OS backend distributables (external, optional).** These are `ifx.win32`, `ifx.wayland`,
   `ifx.x11`, `ifx.cocoa`, plus the audio backends `ifx.wasapi`/`.coreaudio`/`.pipewire` and the input backends. Each
   FFI-binds its OS, implements an `ifx` backend interface, and registers itself at load.
   This is dependency inversion, since the backend depends on `ifx` and never the reverse. Each is versioned
   on that vendor's churn cadence, independently, so an Apple deprecation bumps only `ifx.cocoa`.
3. **`ifx-backend`, a required `melt`.** A melt is Cajeta's BOM/version-catalog (`samples/buildtool/melt`), and this one
   is the one dependency line an app that wants real platform I/O adds. It is target-conditional.
   It resolves to `ifx.win32` for a Windows build, `ifx.wayland`+`ifx.x11` for Linux, and `ifx.cocoa` for
   macOS, plus the audio/input backends, so the developer writes one line and the build picks the
   right vendor libs. It is required only for interactive apps. A headless build adds nothing and
   rides the stdlib floor (§9.4 case 3).

```jsonc
// an interactive app's entire platform dependency surface:
"capabilities": ["window", "input", "audio"],
"dependencies": { "ifx-backend": "1.0.*" }   // ifx itself is stdlib — implicit
```

This beats a single fat library, which would re-release every platform when only macOS changed, the
vendor-churn problem. It also beats bare per-OS libraries, which lose write-once ergonomics and
risk API skew, since nothing owns the shared contract. The facade in stdlib plus the melt gives one-line
convenience without the monolith.

The three domains (`window`, `input`, `audio`) are kept separate because their use cases and vendor
APIs are independent. A server wants `audio` with no `window`, and gamepads come from a different OS
API (XInput / evdev / GameController) than the window's event queue. On desktop `window` and
`input` may share a backend binary, but the API surfaces stay distinct so headless/console/
server builds pull only what they need. All three are capability-gated.

```
ifx  (facade, STDLIB)  ◀──implements+registers── ifx.win32 / ifx.wayland / ifx.cocoa
   │ produces                                       (FFI, vendor-versioned, external/optional)
   ▼
 Surface  ──consumed by──▶  cajeta.xpu.gfx swapchain   (pairs w/ the right WSI ext)

 ifx-backend  =  REQUIRED melt; selects { window, input, audio } backends per build target
```

### 9.4 Binding model & the three cases

Backends bind through a registry, a probe, and a dispatcher, the same pattern as the GPU backend
dispatcher, where the bundle is built at build time, the runtime picks the best available, and there is a
defined floor when none is available. v1 static-links the bundled backends and dispatches at launch.
`dlopen` plugin backends are a later option.

1. **Per-target dependencies (build time).** A native binary is OS-specific, so "target N OSes"
   = N builds. The `ifx-backend` melt maps build-target → backend(s) (Windows→`ifx.win32`;
   Linux→`ifx.wayland`+`ifx.x11`; macOS→`ifx.cocoa`), and the developer writes one melt line. Direct
   dependence on a specific backend, such as X11-only embedded, is allowed, bypassing the melt.
2. **Hardware/OS detection (runtime).** The OS is already fixed by the build target, so the genuine
   runtime choice is within-OS (Wayland vs X11; PipeWire vs Pulse vs ALSA). Each backend
   exposes `probe() → viable?` and `priority`, and `ifx` binds the highest-priority viable bundled
   backend at launch (e.g. `WAYLAND_DISPLAY` set → Wayland else X11). The env override mirrors
   `CAJETA_XPU_BACKEND`, with `CAJETA_IFX_WINDOW=x11` and `CAJETA_IFX_AUDIO=alsa`. GPU-driver WSI
   selection is separate, and the Vulkan loader handles it at the gfx layer.
3. **No backend included (the floor).** `ifx` ships a built-in `null`/headless backend, at the lowest
   priority and always registered. It has no OS window, an offscreen `Surface` (gfx still renders to an
   image, which has real value for servers/CI/golden-image tests), empty input, and null audio. Importing
   `ifx` therefore always compiles and runs. The app never silently renders to nowhere. An app that opts in
   to headless gets `null` silently. An app that requests an interactive surface with only `null`
   available fails loudly at launch ("no `ifx.window` backend for this environment; add
   `ifx-backend`"), and the failure is logged and never swallowed. The build also warns if the melt resolves a
   target with no matching window backend. *Headless is a legitimate floor. A missing-but-needed
   backend is a loud error, and it never becomes a black screen.*

### 9.5 The `harness` backend — capture & replay for testing

The headless floor is split in two. `null` is the silent discard and the always-there fallback.
`harness` is a full `ifx` backend whose device is files and scripts instead of OS
hardware, turning the live engine into a deterministic test rig. It is selected deliberately
(`CAJETA_IFX_*=harness`, a `--harness` flag, or `ifx.useHarness(cfg)` in a test), and it implements
all three domains as capture/replay:

| Domain | real backend | `harness` |
|---|---|---|
| **window** | present to `HWND`/`wl_surface` | **record** each presented frame → PNG sequence / video / in-memory ring |
| **input** | OS event queue | **replay** a scripted, frame-locked event timeline (or **record** real input → script) |
| **audio** | WASAPI/CoreAudio/PipeWire | **capture** the output mix → WAV/PCM on disk; **replay** scripted PCM as mic input |

- **Ships as the optional `cajeta-ifx-harness` library** in its own repo. It has no FFI, since it writes via
  `cajeta.io`, and it registers into `ifx` like any backend. It is portable and runs anywhere (CI, the
  self-hosted runner, a headless server). The silent `null` floor stays in stdlib `ifx`, and
  `harness` is opt-in, as a dev-dependency for test builds.
- **Reuses the existing render path.** The harness `Surface` is just an offscreen render target,
  since gfx already writes `Image2D` and `download`s it, and `present()` reads back the framebuffer and
  writes frame *n*. The app is unchanged and runs the same write-once `ifx` code, so swapping the backend is the
  whole test setup. Glorias' own test suite runs on `harness`.
- **Operationalizes the golden-file discipline** the gfx/canela plans already mandate. It extends
  golden-image tests to the whole interactive loop (input → sim → render → audio), with golden
  frames, golden audio, and a captured input script.
- **Determinism stack.** A virtual clock with a fixed timestep instead of wall time, which pairs with the
  `clock` capability, plus seeded RNG (§4.8) and scripted input give reproducible runs. There is an honest
  limit. GPU float and driver differences mean captured pixels are not bit-identical across
  machines, so window/audio comparison uses a tolerance (a perceptual/RMS threshold, as the canela
  plan specifies with "image metric within N pixels"), and it is bit-exact only on the CPU reference path. This
  keeps it from becoming a false-green trap.
- **Verification API** pairs with `cajeta.testkit`, with `h.frame(n).matchesGolden(ref, tol)` and
  `h.audio().matchesGolden(ref, tol)`. On failure it writes the actual and diff artifacts to `build/`.

The floor therefore does triple duty. It degrades gracefully (`null`), renders offscreen for servers, and
captures and replays for deterministic CI testing (`harness`). That is the substrate that gives Glorias, and
any Cajeta game, automated visual, audio, and input regression tests with no display or sound card.

### 9.6 Optional codec providers (kept out of stdlib)

Video codecs are heavyweight, patent-encumbered (H.264/HEVC pools), and often hardware/vendor-
specific (NVENC/AMF/VideoToolbox/QSV). Those are the same reasons that keep vendor silicon and OS
backends out of stdlib. The same SPI pattern therefore applies one level down. Stdlib defines the
interface and ships only an unencumbered fallback, and real codecs are optional providers that
register in.

- **`ifx`/`harness` (stdlib) defines a `VideoSink` SPI** ("take frames, write a recording") and an
  audio equivalent, implementing only the royalty-free fallback. Window goes to a PNG/image sequence
  (or raw/Y4M), and audio goes to WAV/PCM, which is a header over samples with no codec.
- **Optional external codec libraries** implement the SPI and register themselves. They are
  `cajeta.media`/`cajeta.video` (libavcodec FFI) and the hardware variants `…nvenc`/`…amf`/
  `…videotoolbox`. Dependency inversion holds, so the codec lib depends on the `ifx` SPI and never the
  reverse. Include one and the registry has it. Omit it and only the PNG/WAV fallback exists.
- **Selection** is registry, probe, priority, and env override (`CAJETA_IFX_VIDEO=nvenc|ffmpeg|
  png-seq`), identical to the backend dispatcher. The graceful/loud rule (§9.4 case 3) applies. An app that wants some
  capture falls back to the frame sequence, and the fallback is logged. An app that demands a codec-only format with no encoder
  present gets a loud error, and never a silent no-record.
- **Verification needs no codec.** Golden tests assert on individual frames and audio, so
  the stdlib-only PNG-sequence and WAV fully cover the harness's testing job, and per-frame
  comparison is easier than per-video anyway. Compressed video is a pure enhancement for the
  watchable-artifact case (demos, gameplay capture, a CI failure clip), on top of a testing story
  that already works dependency-free.

This is the general rule the platform layer establishes. Stdlib owns the interface and a trivial
unencumbered default, and heavyweight, encumbered, or vendor implementations are optional external
libraries that register through the SPI. Codecs, OS backends, and vendor GPU silicon all follow
it.

---

### 9.7 Feature matrix, capability queries & the gap plan

This section comes from the 2025-2026 vendor-SDK research, with per-backend detail in each `cajeta-ifx-*` repo's
`documents/`. It is how `ifx` and the backends cover all needed features and handle the
gaps where vendors diverge.

The binding strategy differs by platform, which is a gap in itself. Not every vendor exposes a C ABI:

| Platform | window / input / audio binding |
|---|---|
| Windows | flat C + COM-style vtables — **direct FFI** (user32, GameInput, XAudio2/WASAPI) |
| Linux | C — **direct FFI** (libwayland, libxcb, libpipewire, libinput, libudev, ALSA) |
| Android | C NDK — direct FFI, **+ a JNI airlock** for soft-keyboard text (GameTextInput), the `RECORD_AUDIO` permission, and lifecycle dialogs |
| macOS / iOS | **AppKit/UIKit/GameController/AVFoundation are Obj-C-only** → a small **Obj-C/Obj-C++ shim** (`extern "C"`) compiled into the backend (or `objc_msgSend` runtime FFI). Core Audio/AudioToolbox/IOKit are C. |

Each Apple backend's plan therefore includes a `.m`/`.mm` shim, and each Android backend a JNI/Java companion.
`ifx`'s contract stays pure C-shaped, and the shim/JNI lives in the backend.

The portable floor is what every backend must provide. It is one surface (with drawable size and scale), a
present hook, lifecycle events (suspend/resume, surface-lost/surface-recreated), key/pointer
or touch input, gamepad enumerate with buttons/axes, audio output and capture (permission-gated),
and the capability query.

The optional features sit behind `ifx.supports(Feature)`, with the portable fallback when absent
(✓ supported · ~ partial · ✗ absent · n/a):

| Feature | Win | Linux | macOS | iOS | Android | Fallback when absent |
|---|---|---|---|---|---|---|
| multiple/resizable windows | ✓ | ✓ | ✓ | ✗ | ✗ | single fullscreen surface |
| programmatic window position | ✓ | ✗ Wayland | ✓ | n/a | n/a | no-op (compositor owns placement) |
| cursor warp | ✓ | ✗ Wayland | ✓ | n/a | n/a | **pointer-lock + relative motion** |
| keyboard / mouse | ✓ | ✓ | ✓ | opt | opt | optional capability; touch is the mobile floor |
| touch | opt | opt | ✗ | ✓ | ✓ | optional on desktop |
| gamepad rumble / FF | ✓ | ✓ evdev | ✓ | ✓ | ✓ Paddleboat | no-op |
| adaptive triggers / gyro / touchpad | ✓ GameInput | ~ SDL DB | ✓ GameController | ✓ | ~ | no-op |
| on-screen virtual gamepad | ✗ | ✗ | ✗ | ✓ GCVirtualController | engine-drawn | engine draws its own |
| audio capture (mic) | ✓ | ✓ | ✓ perm | ✓ perm | ✓ perm | permission-gated; denied → reported, no capture |
| **loopback / system-output capture** | ✓ WASAPI | ~ PipeWire/portal | ~ ScreenCaptureKit | ✗ | ✗ | unsupported → error, never silent |
| exclusive / low-latency audio | ✓ WASAPI excl/IAudioClient3 | ✓ PipeWire quantum | ✓ AudioUnit | ✓ RemoteIO | ~ AAudio MMAP (OEM-dep) | shared-mode default |
| HDR present | ✓ DXGI | ~ | ~ | ~ | ~ | SDR (a gfx-swapchain concern) |

The gap plan is how `ifx` handles divergence:
1. **Capability query is the mechanism.** `ifx.supports(Feature)` per active backend plus the portable
   floor. Apps program to the floor and feature-detect the rest, and they never assume.
2. **Lifecycle and surface-loss are MANDATORY contract events.** Android `APP_CMD_TERM_WINDOW` and iOS
   background force them, and desktop emits minimize/restore. The gfx swapchain must handle
   surface-recreated. This is non-optional, because mobile breaks without it.
3. **Mouse model is pointer-lock-first.** Wayland has no warp, so relative-pointer/lock is
   the primitive, and warp-to-XY is an optional capability.
4. **Window floor is "one fullscreen surface"** (mobile). Multi-window is an optional desktop cap.
5. **Permissions are first-class.** This covers mic capture (mobile/macOS) and input-device access (Linux
   seat/udev-ACL). `ifx` exposes request/state, and denied is a reported state rather than a crash.
6. **Audio route/interruption events are portable.** iOS `AVAudioSession` forces them, and others rarely
   fire, so games pause/resume and rebuild the graph uniformly.
7. **Min-OS floors** are set by the modern API chosen. They are Windows 10 1903 (GameInput); Linux =
   Wayland+PipeWire modern, X11+ALSA floor; macOS 11+; iOS 13+ (15+ recommended); Android API 24
   (Vulkan) / 26 (AAudio), GameActivity to API 19.

The net is that the contract is a floor plus capability flags. Each backend fills what its OS offers and
reports the rest unsupported. The per-OS SDK choice, the binding (C / Obj-C shim / JNI), and the
fallbacks live in each `cajeta-ifx-*` repo's spec and plan.

---

### 9.8 Interop mechanism — how `ifx` backends bind (C / Obj-C / JNI)

The thesis is that no backend requires Cajeta to understand Obj-C or Java, because all three targets are reachable
through a C ABI, which `@Native` already provides. `@Native("symbol")` binds a typed method to a C
symbol (e.g. `@Native("__cajeta_xxh3_alloc")`) and compiles through LLVM to a native object, so it
can both call any C-ABI symbol and export C-ABI symbols. Every platform's "hard" interop
reduces to that. The earlier "can Cajeta build/link these?" question is answered yes, via standard
clang/NDK steps. The open part is the build tool driving them, tracked per backend repo.

- **Windows / Linux use direct C FFI.** Win32, GameInput/XAudio2/WASAPI (COM-style vtables are
  C-callable), and libwayland/libxcb/libpipewire/libinput/evdev/ALSA are all C. Runtime backend
  selection uses `dlopen`/`dlsym`. Callbacks (WndProc, wl listeners) are C fn-ptrs Cajeta exports.
  There is no shim.

- **macOS / iOS use Obj-C via the C runtime plus a thin clang shim**, used together:
  1. **Direct `libobjc` FFI** for ordinary calls. `@Native`-bind `objc_getClass`,
     `sel_registerName`, and per-signature typed casts of `objc_msgSend`. It is not variadic, so
     on arm64 call `objc_msgSend` for everything, cast to the exact callee prototype, and on x86_64 route
     struct/`long double` returns to `objc_msgSend_stret`/`_fpret`. This is exactly how Rust `objc2`
     and Zig `zig-objc` work, with a codegen-time monomorphic `msgSend<Ret,Args>` and interned selectors.
     Memory is explicit (`objc_retain`/`objc_release`/`objc_autorelease` and `objc_autoreleasePoolPush/
     Pop`), since Cajeta is not ARC-aware. Delegates are built at runtime via `objc_allocateClassPair` and
     `class_addMethod`, where the IMP is a Cajeta-exported `(id self, SEL _cmd, …)` C fn.
  2. **A small clang `.m`/`.mm` shim** (`extern "C"`, `-fobjc-arc`, linked `-framework …`) for the
     genuinely painful parts. Those are blocks (GameController `valueChangedHandler` and AVFoundation handlers,
     where clang's `^{}` wraps a C fn-ptr and context), the app/run-loop bootstrap
     (`NSApplicationMain`/`UIApplicationMain` own thread 0), and rich delegates. Apple's toolchain
     is clang/LLVM, so the shim is a native build step.
  The split is typed-`objc_msgSend` FFI for ~90%, and the clang shim for blocks, bootstrap, and heavy
  delegates. An optional ergonomic is an `@ObjC` / `@Native(objc=…)` form the compiler lowers to
  selector-registration plus a correctly-typed `objc_msgSend` cast. That is sugar over the raw FFI floor
  and adds no new capability. It is the "extend `@Native` to Obj-C" idea, and it is feasible because
  the floor already works through `libobjc`.

- **Android uses JNI, which is a C ABI.** Cajeta compiles to a per-ABI `.so`. Java→native binds via
  `RegisterNatives` in `JNI_OnLoad(JavaVM*)`, a Cajeta-exported C symbol. This is preferred over name
  mangling, since it exports only `JNI_OnLoad`, hands ART raw fn-ptrs, and fails fast. Native→Java goes through
  the `JNIEnv*`, a C struct of ~230 function pointers (`FindClass`/`GetMethodID`/`Call*Method`),
  all `@Native`-callable. For threads, `JavaVM` is global, render/audio threads `GetEnv`/
  `AttachCurrentThread` with a TLS-destructor auto-detach, `JNIEnv` is thread-local, and local/global refs
  must be managed. The GameActivity / `android_native_app_glue` glue, compiled into the `.so`,
  already hands native code the `JavaVM`/`JNIEnv`/Activity `jobject`/`ANativeWindow`, so hand-written
  JNI is needed only for GameTextInput (soft keyboard), the `RECORD_AUDIO` permission, and
  lifecycle dialogs. A tiny Java/Kotlin companion (`GameActivity` subclass, `System.loadLibrary`,
  and manifest `lib_name`) is required. Packaging is Gradle/AGP (GameActivity via a Prefab AAR, AGP
  4.1+), or a manual `aapt2 → d8 → zip(lib/<abi>) → zipalign → apksigner` minimum.

Per-backend deep detail lives in each `cajeta-ifx-*` repo's spec/plan (Appendix B, Interop).

---

## 10. Research provenance

The digested indices are `research/sigraph/ResearchPlan.md` (the gfx research digest, covering ReSTIR, BVH,
virtualized geometry, Lumen/GI, virtual texturing, BRDF, splats/NeRF, fluids/clouds) and the
`plans/gpu/gfx/canela-plan.md` bibliography. The PDFs are gitignored, and 58 of 127 were fetched locally this pass,
all open-access/arXiv. The 69 not fetched are paywalled DOI/ACM/IEEE/Wiley/Springer
classics, fully summarized in the two digests above. The fetch log is `logs/fetch_gfx_pdfs.log`.

---

## Appendix A — 2-D/UI: consumer (my lean) vs. gfx primitive

The question (Q6) is whether stdlib gfx ships a `Canvas`/`Sprite`/`Text` API, or only the
pipeline, texturing, and math primitives a 2-D layer is built from. There are two worlds with the same on-screen
result. *The shader-I/O spelling is illustrative. What matters is where the opinions live.*

### World A — **2-D/UI is a consumer** (the lean): stdlib ships only primitives

```cajeta
// package gloria.ui  — NOT stdlib. Built on gfx + math primitives.
import cajeta.gfx.GraphicsPipeline;
import cajeta.gfx.RenderPass;
import cajeta.xpu.KernelBuffer;
import cajeta.gfx.Texture2D;
import cajeta.gfx.Sampler;
import cajeta.math.Matrix;            // ortho()/perspective() builders (§4.1)
import cajeta.math.Vector;

// Shader stages authored with the gfx per-stage annotations (§4.2).
@Vertex
static void spriteVS(Vector<float32,2> inPos, Vector<float32,2> inUv,
                     Matrix<float32,4,4> proj,            // push constant
                     out Vector<float32,2> vUv) {
    vUv = inUv;
    gfx.position = proj * stack Vector<float32,4>(inPos.x, inPos.y, 0.0, 1.0);
}

@Fragment
static void spriteFS(Vector<float32,2> vUv, Texture2D atlas, Sampler s,
                     out Vector<float32,4> color) {
    color = atlas.sample(s, vUv.x, vUv.y);
}

// The engine wraps the primitives into the API *its* users want — and in doing
// so makes every 2-D opinion explicitly, in engine code, not in stdlib:
public final class SpriteBatch {
    GraphicsPipeline pipe;
    KernelBuffer<float32> quads;
    Texture2D atlas;
    Sampler sampler;
    Matrix<float32,4,4> proj;

    public SpriteBatch(RenderPass pass, Texture2D atlas, uint32 w, uint32 h) {
        this.pipe    = heap GraphicsPipeline(pass, spriteVS, spriteFS);  // blend = engine's choice
        this.atlas   = atlas;
        this.sampler = stack Sampler(1, 0);                  // linear, clamp
        this.proj    = Matrix.ortho(0.0, w, h, 0.0, -1.0, 1.0);  // y-down, top-left origin: engine's UI convention
        this.quads   = heap KernelBuffer<float32>(4096);        // vertex layout + batching: engine's choice
    }
    public void draw(float32 x, float32 y, float32 w, float32 h, Rect uv) { /* append a quad */ }
    public void flush(RenderPass pass) { /* upload quads; pipe.draw(pass, quads, this.proj) */ }
}

// Caller (game / tool code):
SpriteBatch ui = heap SpriteBatch(pass, fontAtlas, 1920, 1080);
ui.draw(32.0, 32.0, 256.0, 64.0, glyphUv);
ui.flush(pass);
```

### World B — **2-D/UI is a gfx primitive**: stdlib ships `Canvas`/`Sprite`/`Text`

```cajeta
import cajeta.gfx.Canvas;     // now IN stdlib
import cajeta.gfx.Font;

Canvas c = heap Canvas(pass, 1920, 1080);        // stdlib fixes the coordinate convention
c.sprite(fontAtlas, 32.0, 32.0, 256.0, 64.0);    // stdlib fixes vertex layout + batching + blend
c.text(font, "Score: 1200", 32.0, 96.0, 24.0);   // stdlib fixes the entire text/font model
c.flush();
```

### What the contrast shows

World B is ~4 lines vs World A's ~40, and it is genuinely nicer to use. But every convenience in B
is an opinion baked into stdlib:

- the coordinate convention (origin top-left? y-down? DPI scaling?),
- the sprite vertex layout, the batching policy, and the default blend mode,
- and, the big one, a whole text/font model, covering bitmap vs SDF glyphs, shaping, Unicode/
  bidi, kerning, and rich text. That is a framework's worth of decisions, and 2-D toolkits disagree
  on all of them.

Those choices vary per game and per UI toolkit, so freezing them in stdlib forces one taste on
everyone and breaches the opinion-free line (§1.2). World A keeps stdlib carrying only the
opinion-free primitives (pipeline, texture sampling, `ortho`) and pushes `SpriteBatch`/`Canvas`/
`Text` into Glorias, or a standalone `cajeta2d` library, where an opinion is supposed to
live, stated in its own code. Hence the lean. 2-D/UI is a consumer, and it is never a gfx primitive.

The same logic is why a minimal render-graph (pass deps and barriers, which are opinion-free) is IN gfx
while the scheduler (aliasing/budget policy, which is opinionated) is in Glorias. The line is drawn at
opinion rather than at convenience.
