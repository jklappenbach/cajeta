# Cajeta GFX — Canela: Virtual Geometry & Textures

**`cajeta.gfx.canela` is the virtualized geometry & texture layer** — the subsystem
that lets developers add high-poly meshes and full-resolution textures to an
application **without ever hand-authoring LODs or mipmaps**. It is a consumer of
`cajeta-gfx` (the graphics pipeline) and `cajeta-gpu` (value types, math, textures,
device/codegen/memory). It does **not** depend on `cajeta-xpu`; where it needs a
compute pass (capture, fusion, simplification, streaming) it uses the `cajeta-gpu`
device/dispatch primitives directly — same rule as the rest of gfx.

```
cajeta-gpu  (foundation)
    ▲
cajeta-gfx  (graphics pipeline, render graph)
    ▲
cajeta.gfx.canela  ← this spec  (virtual geometry + virtual textures)
```

**Status: research complete, design proposed, no code.** This spec is forward work.
It is gated on `cajeta-gfx` Part G1 (graphics pipeline) and G3.0 (render graph), and
on `cajeta-gpu`'s value-type/texture/math stages. The hardware-RT-pipeline parts
(Part G3.3) are *not* required — Canela's capture pass uses **ray query** (the
foundation-level inline primitive, `cajeta-gpu` Part C), not the full RT pipeline.

Checkbox legend: `[x]` landed+tested · `[~]` partial · `[ ]` not started.
**Working agreement:** one increment at a time, tests + docs + commit checkpoint;
golden-image + golden-mesh tests for every generator; never ship a broken LOD
silently (a missing/failed LOD falls back to the next coarser valid one, logged, not
swallowed); commit only when asked; **no attribution trailer**; stage files explicitly.

---

## Context — the design, and why

The goal is **zero-authoring virtual geometry**: drop in a 2M-triangle mesh and a 4K
texture set, declare how the asset will ever be *viewed*, and the engine produces,
streams, and selects all detail levels automatically. We are not cloning Nanite —
Nanite's cluster-DAG is itself descended from prior art (Cignoni's *Batched
Multi-Triangulation*, Yoon's *Quick-VDR* CHPM, Hoppe's *progressive / view-dependent
meshes*). We build on the same lineage and add two things Nanite does **not** exploit:
a **view-bounded capture constraint** and a **raycast-fuse remeshing path** that
generates geometry *and* its material mips together.

### The two pillars

1. **Mesh-domain backbone (proven).** A cluster-DAG built by QEM edge-collapse
   (Garland–Heckbert 1997) over clusters with appearance-preserving error bounds
   (Cohen 1998), structured as a multiresolution DAG (Cignoni 2005, *Batched
   Multi-Triangulation*) with view-dependent cluster selection (Yoon 2004, *Quick-VDR*;
   Hoppe 1996–97). This carries the **near-field** LODs where silhouette fidelity
   matters most, and is the fallback for everything the capture path can't yet handle
   (skinned/deforming meshes, thin shells, razor creases). It is derived **entirely
   from the independent academic lineage above** — all 20–28 years old — and is *not*
   reverse-engineered from Nanite (see the **Legal Disclaimer** below).

2. **Raycast capture-and-fuse path (the novel idea).** For **mid/far** LODs, the
   developer's idea — orbit a virtual camera around the asset, raycast the original
   high-poly mesh, and fuse the hits into reduced geometry per distance band — is
   **sound and academically grounded**. It is, precisely:
   - **Image/view-driven LOD** (Lindstrom–Turk, *Image-Driven Simplification*, 2000):
     drive detail by what the camera actually *sees* from a sphere of viewpoints, not
     by 3D geometric error. Their headline result is the exact intuition behind a
     view-bounded camera: **hidden/occluded portions simplify away aggressively** for
     free, because they never affect any captured view. Bounding the camera range and
     rotation makes "what's visible" a small, knowable set.
   - **Volumetric range-image fusion** (Curless–Levoy, 1996 — the TSDF method;
     KinectFusion is its real-time GPU form): the "how do I combine all the raycast
     datasets into one reduced geometry" question is *already solved*. Each camera
     pose's ray hits become a confidence-weighted signed-distance field in a voxel
     grid; overlapping captures are averaged (grazing-angle hits down-weighted);
     **space carving** distinguishes *unseen* from *empty* and fills occluded holes
     plausibly; **Marching Cubes** (Lorensen–Cline) extracts the mesh.

### Two corrections the research forces on the original idea

- **Capture material attributes, not shaded color.** Lindstrom–Turk fix the light to
  the viewer and explicitly flag baked-in view-dependent shading as a limitation. In a
  relit real-time engine that is fatal. The capture pass must store a **G-buffer per
  ray hit** — albedo, world/tangent-space normal, roughness/metalness, motion/ID — and
  reconstruct *material* mips, never lit pixels. Specular/anisotropic appearance is
  then re-derived at runtime from the captured normals + roughness.
- **TSDF fusion rounds sharp features** (Curless–Levoy note difficulty at sharp
  corners, thin surfaces, and a lower bound on reconstructable shell thickness). This
  is *desirable* for coarse LODs but unacceptable for the silhouette at close range —
  hence the hybrid: **near = cluster-DAG (QEM on the real mesh), mid/far =
  raycast-fuse.** The crossover distance is where TSDF rounding drops below a pixel.

### Where this beats Nanite (and where it doesn't)

- **Beats:** joint geometry+texture LOD from one pass (no separate mipmap/virtual-
  texture authoring); occluded-detail elimination is automatic and view-bounded;
  far-field LODs collapse naturally into **impostors / billboard clouds** (Décoret) or
  **splats** from the *same* captured samples; the capture pass is embarrassingly
  parallel and offline, so runtime cost is pure selection + streaming.
- **Doesn't (yet):** dynamic/skinned/deforming meshes (capture bakes a pose — these
  stay on the mesh-domain backbone); razor-thin and crease-critical assets near-field;
  bake-time cost (the developer-acknowledged tradeoff — capture+fuse+simplify is
  hours, like Curless–Levoy's million-poly fusions, parallelizable on GPU).

### The hybrid representation & the splat tier

Canela is a **hybrid by distance band** — one asset, one capture, detail (and even
*representation*) selected by how far the camera is:

| Band | Representation | Why |
|---|---|---|
| **Near** | Cluster-DAG mesh (QEM / Cohen / Cignoni) | Crisp silhouettes, correct relighting, hardware-rasterized; **also the physics / collision / animation representation** |
| **Mid** | Raycast-fuse mesh (TSDF + Marching Cubes) | Reduced triangles, auto material-mips, still hardware-rasterized, integrates with standard PBR |
| **Far** | Impostor / billboard cloud (Décoret) — *or* splats | Object covers few pixels; cheapest possible |

Splats are **not a fourth pillar** — they are an *alternate output* for the mid/far
bands, chosen per asset.

**The unifying insight (what makes this a design, not a bolt-together).** The capture
pass emits **oriented points with material** (position, normal, albedo, roughness/metal,
ID). That single dataset is simultaneously the input to *all three* output paths:
- → TSDF fuse → **triangle mesh**
- → plane-fit → **billboard cloud / impostor** (Décoret)
- → splat optimizer → **Gaussians** (Kerbl 2023; LOD via Octree-GS; surfel/QSplat lineage)

One capture, three possible representations, picked per asset/band. Adding the splat
path later costs an *encoder*, not a second pipeline.

**Why splats are fast enough *here specifically*.** Gaussian-splat cost is dominated by
(1) a per-frame depth **sort** of active splats, (2) **fill-rate/overdraw** scaling with
screen coverage × resolution, and (3) **bandwidth/VRAM**. A single captured scene
renders ~100–200 FPS at 1080p on a high-end GPU, but that budget evaporates in a full
game (4K, VR, many objects, transparency, post). Canela assigns splats only to **mid/far
static bands**, where every cost collapses: LOD keeps the *active* splat count tiny, far
objects cover few pixels (negligible overdraw), and far bands are the smallest streamed
payloads. The parts where splats are genuinely slow or unsolved — **near-field
full-screen detail, dynamics/4D, relighting, collision** — never touch the splat path,
because the mesh backbone owns them. This is the same "splats for the eye, meshes for
everything else" split PlayCanvas and World Labs converged on independently.

**Sequencing discipline — mesh-first, splat tier deferred.** The mesh-only hybrid
(near DAG + mid/far fused mesh + impostor far) is **probably sufficient for most assets**
and is simpler: one rasterizer, physics already mesh, standard PBR lighting, no
per-frame sort, no relighting approximations. So:
- **Build mesh-first.** Ship the entire distance range on hardware triangles + impostors
  before any splat code exists.
- **Keep the splat encoder as a deferred, optional tier** (`BakeConfig::Splat`), built
  only when a concrete asset class earns it.
- **Decision rule for *when* splats earn their place:** authored / hard-surface asset
  (sword, crate, character) → **mesh all the way down** (fused mesh wins on every axis;
  splats add only complexity). Scanned / organic / photoreal / volumetric asset
  (environments, foliage, fuzzy materials) → **mesh near** (for physics + silhouette) +
  **splats mid/far** (they encode view-dependent appearance and soft edges more compactly
  than a fused mesh + textures). Either way, the *same capture pass* feeds both.

### Legal Disclaimer

**Legal Disclaimer:** Though some of the goals of UE5's Nanite are shared by Canela,
Canela was designed from the ground up without reference or reverse engineering UE5
code. The works are entirely original or based on prior art.

Prior-art provenance — each mechanism derives from independent academic literature:

| Mechanism | Derived from |
|---|---|
| Cluster simplification / error metric | Garland–Heckbert 1997 (QEM); Cohen 1998 (appearance-preserving) |
| Multiresolution cluster DAG | Cignoni 2005 (Batched Multi-Triangulation); Yoon 2004 (Quick-VDR / CHPM); Hoppe 1996–97 |
| Cluster partitioning | Jakob 2023 (meshlet strategies) |
| GPU-driven cull + indirect draw | Haar–Aaltonen 2015 |
| View-bounded LOD selection | Lindstrom–Turk 2000 |
| Capture + fuse remeshing (Canela-original path) | Curless–Levoy 1996; KinectFusion 2011; Lorensen–Cline 1987 |
| Virtual texture streaming | van Waveren 2012/2013 (id Tech 5); Ka Chen 2015 |

### View-bounding inputs (the developer's control surface)

Per asset (or asset class), the developer declares the viewing envelope, and these
*entirely determine* how many LODs exist and how aggressively each simplifies:
`distanceRange [min,max]`, `distanceDivisions` (→ one LOD band each), `rotationExtents`
(major-axis arc the asset can present — often a narrow cone, sometimes full 360°),
`rotationIncrement` (capture stride), and `targetTexelDensity` / `idealRayResolution`
(rays-per-solid-angle → the captured/fused resolution). Defaults: full sphere, 8
distance bands, density derived from the asset's source texel size at `min` distance.

---

## 1. TDD

a. **Regression guards (must stay green throughout).**
   1. [ ] Existing `cajeta-gfx` graphics-pipeline + render-graph tests unaffected
      (Canela is additive; importing `cajeta.gfx.canela` must not perturb G1/G3).
   2. [ ] `cajeta-gpu` ray-query probe tests still green (capture depends on inline
      ray query; assert the `CAJETA_HAS_SPV_RAY_QUERY` capability is present, else the
      capture path is compiled out and the spec's mesh-domain backbone is the only
      path — fail *loudly*, never a swallowed skip).
   3. [ ] `cajeta` compiler binary rebuilt so any new `@Device`/kernel helpers register
      (per the "rebuild cajeta for the Tour" rule — kernels silently no-op otherwise).

b. **New red-first unit tests (foundation math & data structures).**
   1. [ ] QEM quadric accumulation + edge-collapse cost on a known tetra/cube → matches
      Garland–Heckbert closed-form error; collapsing a planar fan costs ~0.
   2. [ ] TSDF voxel update rule (eq. 3/4 of Curless–Levoy): two synthetic range scans
      of a plane fuse to a zero-crossing at the analytic midpoint; grazing-angle hit is
      down-weighted vs head-on hit.
   3. [ ] Marching Cubes on an analytic SDF sphere → genus-0, watertight, vertex count
      within tolerance; ambiguous-case table resolves without cracks.
   4. [ ] Space-carving classification (unseen / empty / near-surface) on a hollow
      cylinder → interior marked unseen, line-of-sight voxels carved empty.
   5. [ ] View-envelope → LOD-schedule derivation is pure & deterministic: given
      `distanceRange/divisions/rotationExtents/increment/texelDensity`, emits the exact
      set of (distance band, orientation bucket, capture resolution) capture jobs.

c. **New red-first integration tests (golden-mesh / golden-image).**
   1. [ ] **Capture determinism:** raycasting a fixed mesh from a fixed pose with a
      fixed ray grid yields a byte-stable hit set (position/normal/material G-buffer).
   2. [ ] **Fuse correctness:** capture→TSDF→MC on the Stanford bunny at a mid band
      reproduces silhouette within N pixels (image metric, à la Lindstrom–Turk RMS over
      a dodecahedron of test views — *different* viewpoints than capture, to avoid bias)
      and reduces triangle count by the band's target ratio.
   3. [ ] **Material-mip fidelity:** relighting the fused LOD under a moving light
      matches relighting the source within tolerance (proves we captured material, not
      lit color — the Lindstrom–Turk correction).
   4. [ ] **LOD selection + streaming:** a camera dollying through `distanceRange`
      selects the correct band per frame, streams the next band in before it's needed,
      and never presents a hole; rotating past `rotationIncrement` boundaries streams
      the adjacent orientation bucket; a forced cache miss falls back to the coarser
      valid LOD (logged), never a blank.
   5. [ ] **Backbone fallback:** a skinned/animated asset bypasses capture and uses the
      cluster-DAG path; correctness == the mesh-domain golden.

---

## 2. Deliverables

a. **`cajeta.gfx.canela` API surface (the developer-facing contract).**
   1. [ ] `VirtualMesh` / `VirtualTexture` asset handles — opaque, LOD-agnostic; the
      developer references one handle and the system selects detail.
   2. [ ] `ViewEnvelope { distanceRange, distanceDivisions, rotationExtents,
      rotationIncrement, targetTexelDensity }` — the declarative bound that drives bake.
   3. [ ] `BakeConfig` selecting backbone strategy per asset:
      `Auto | MeshDAG | RaycastFuse | Splat` (Auto = static→RaycastFuse for mid/far +
      MeshDAG near; dynamic/skinned→MeshDAG).
   4. [ ] Runtime selection/streaming handles wired into the `cajeta-gfx` render graph
      (G3.0) as a culling+LOD pass producing indirect draws.

b. **Mesh-domain backbone (the proven core).**
   1. [ ] Meshlet partitioning (cluster generation; evaluate strategies per
      Jakob 2023) + per-cluster bounds for culling.
   2. [ ] QEM edge-collapse simplifier (Garland–Heckbert) with appearance-preserving
      error terms (Cohen) producing a cluster **DAG** (Batched Multi-Triangulation /
      Quick-VDR CHPM structure), not a flat chain.
   3. [ ] Runtime view-dependent cluster selection + GPU-driven indirect draw
      (Haar–Aaltonen GPU-driven pipeline); seamless LOD across cluster boundaries.

c. **Raycast capture-and-fuse path (the novel pillar).**
   1. [ ] **Capture pass:** virtual camera orbits per the LOD schedule; inline ray
      query (`cajeta-gpu` Part C) against the source mesh; emit a **G-buffer per hit**
      (position, normal, albedo, roughness/metal, ID) — store as Layered Depth Images
      (Shade 1998) / multi-layer via depth-peeling (Everitt) so occluded-but-reachable
      layers are captured, not just the nearest hit.
   2. [ ] **Fusion pass:** confidence-weighted TSDF accumulation (Curless–Levoy eq.
      3/4; GPU form per KinectFusion) into a sparse/RLE voxel grid; grazing-angle
      down-weighting; **space carving** for unseen-vs-empty hole filling.
      *Alternative path to evaluate:* oriented-point **Screened Poisson** (Kazhdan–Hoppe
      2013) when hits are sparse oriented points rather than a dense grid.
   3. [ ] **Mesh extraction:** Marching Cubes (Lorensen–Cline) → per-band reduced mesh;
      hand off to the QEM simplifier for final triangle-budget trim and meshletization
      so capture-path and backbone-path LODs share one runtime representation.
   4. [ ] **Material-mip bake:** resample the captured G-buffer into the band's texture
      mips (seam-aware atlas, Purnomo–Cohen; or per-face Ptex-style, Burley–Lacewell,
      to dodge UV authoring entirely) — **material attributes only**, never lit color.
   5. [ ] **Far-field collapse:** below a triangle/area threshold, emit a **billboard
      cloud / impostor** (Décoret) from the same capture instead of geometry.

d. **Virtual texture streaming (zero-authored mips).**
   1. [ ] Sparse/partially-resident virtual texture with feedback-driven tile
      streaming (van Waveren software VT; adaptive allocation per Far Cry 4 / Ka Chen)
      over the material mips produced in (c.4) and (b).
   2. [ ] Toroidal/clipmap-style virtual addressing for the resident cache
      (Losasso–Hoppe geometry clipmaps; Taibo dynamic VT).

e. **Streaming & residency manager (geometry).**
   1. [ ] Distance-band + orientation-bucket residency: prefetch the next band/bucket
      ahead of camera motion; evict by LRU + view-envelope priority.
   2. [ ] Cache-miss fallback to coarser-valid LOD (logged), out-of-core load (Adaptive
      TetraPuzzles / Quick-VDR out-of-core lineage).

f. **Splat tier — DEFERRED, optional (build mesh-first; see "The hybrid representation
   & the splat tier" in Context).** Do not start until the mesh-only hybrid (b+c, near
   DAG + mid/far fused mesh + impostor far) ships the full distance range, AND a concrete
   **scanned / organic / photoreal** asset class earns it (decision rule: authored
   hard-surface → mesh all the way down; scanned/organic/volumetric → mesh near + splats
   mid/far). The capture pass (c.1) already emits the required oriented-point + material
   data, so this is an *encoder + render path*, not a second pipeline.
   1. [ ] **Splat encoder:** `BakeConfig::Splat` turns the *same* captured oriented
      points (surfel/QSplat lineage — Pfister, Rusinkiewicz–Levoy) into **Octree-GS**-
      style LOD Gaussians (Kerbl 2023) for mid/far bands. Gated, off by default.
   2. [ ] **Splat render path:** GPU radix depth-sort of active splats (amortize/skip
      re-sort under small camera motion), tile rasterization, integrated as a render-
      graph pass alongside the mesh draws; near-field + physics + dynamics stay on the
      mesh backbone (splats never own collision/animation/relighting).
   3. [ ] **Verify the fast-regime assumption:** measure that LOD keeps active-splat
      count + overdraw within budget at mid/far coverage; if a band can't hit frame
      budget, fall back to the fused-mesh/impostor representation for that band (logged).

g. **Bake tooling.**
   1. [ ] Offline bake step integrated with the content build (`build.sh` / G4.0 asset
      pipeline); deterministic, cacheable, resumable; reports per-asset LOD stats and
      the image-metric error vs source.

h. **Docs & cross-refs.**
   1. [ ] This spec kept in sync with committed state (avoid the "plan lags committed
      state" drift); link from `cajeta-gfx-plan.md` Part G3.1 (LOD) and G4 (assets).
   2. [ ] Bibliography (below) kept pointing at `research/canela/`.

---

## 3. Acceptance Criteria

a. [ ] A developer adds a high-poly mesh + textures with **no LODs and no mipmaps
   authored**, declares a `ViewEnvelope`, and the asset renders correctly across the
   full distance range with automatic, seamless LOD and no visible popping.
b. [ ] Mid/far LODs are generated by the raycast-fuse path; near LODs by the
   cluster-DAG backbone; the crossover is chosen so TSDF rounding stays sub-pixel.
c. [ ] Fused LODs **relight correctly** under dynamic lighting (material captured, not
   shaded color) — the material-mip fidelity test passes.
d. [ ] Occluded geometry outside the `ViewEnvelope` is provably absent from mid/far
   LODs (the view-bounded win), verified by triangle-count reduction beyond what a
   view-agnostic QEM pass achieves at equal image error.
e. [ ] Streaming presents no holes under camera dolly + rotation; forced cache misses
   degrade to coarser LODs (logged), never blanks.
f. [ ] Dynamic/skinned assets transparently use the backbone path with no API
   difference to the developer.
g. [ ] *(deferred tier)* For a scanned/photoreal asset class that earns it,
   `BakeConfig::Splat` produces a working mid/far splat LOD from the same capture and
   holds frame budget at mid/far coverage; authored hard-surface assets stay mesh-only.
   The mesh-only hybrid ships first and complete without this.
h. [ ] Bake is deterministic and resumable; the build reports each asset's LOD schedule
   and measured image-metric error vs source.
i. [ ] **Originality holds:** Canela is original work or based on published prior art;
   each module cites its prior-art source.

---

## Verification commands

```sh
# rebuild compiler so canela kernels/@Device helpers register (NOT cajeta_test)
ninja cajeta
# targeted tests (never the full suite without an explicit ask)
ctest -j "$(nproc)" -R 'canela|Canela'
```

---

## Dependency notes & sequencing

- **Gated on `cajeta-gfx` G1 (pipeline) + G3.0 (render graph).** Canela is a render-
  graph pass + an asset path; both must exist first.
- **Capture uses ray query, not the RT pipeline.** It rides `cajeta-gpu` Part C (inline
  ray query) — the smaller, shared, already-prioritized primitive — *not* the deferred
  full RT pipeline (G3.3). Watch the fork-LLVM ray-query capability (`CAJETA_HAS_SPV_
  RAY_QUERY`); if absent, the capture path compiles out and only the mesh-domain
  backbone is available — and that must be a loud, logged condition, not a silent skip.
- **Backbone before capture.** Land the cluster-DAG/QEM backbone first (it's the
  fallback and the shared runtime representation); the raycast-fuse path feeds *into*
  the same meshlet/DAG runtime, so it can't be validated until the backbone exists.
- **Mesh-first, splat tier last.** Order is: (1) mesh backbone → (2) raycast-fuse
  mid/far mesh + impostor far → the mesh-only hybrid must ship the full distance range
  before (3) the deferred, optional splat tier (Deliverable f), and the splat tier only
  begins once a concrete scanned/photoreal asset class earns it (authored hard-surface
  assets stay mesh-only). The splat tier is an encoder + render path over the *existing*
  capture, never a second pipeline.
- **Promote shared pieces to the foundation.** QEM simplification, Marching Cubes, and
  the sparse-voxel/TSDF grid are general-purpose; per the "promote to general-purpose"
  rule, land them in `cajeta-gpu`/stdlib where other consumers (gpu-utils collision,
  physics) can reuse them, not siloed in canela.

---

## Bibliography (PDFs in `research/canela/`)

**The two load-bearing papers (read in full for this design):**
- Lindstrom & Turk 2000, *Image-Driven Simplification* — view/image-driven LOD; the
  occluded-detail-is-free result that validates view-bounded capture.
  `lindstrom-turk-2000-image-driven-simplification.pdf`
- Curless & Levoy 1996, *A Volumetric Method for Building Complex Models from Range
  Images* — the TSDF fuse + space-carving + Marching-Cubes recipe for the fuse step.
  `curless-levoy-1996-volumetric-range-fusion.pdf`

**Existence-proof only — not an implementation source:**
- Karis 2021, *A Deep Dive into Nanite Virtualized Geometry* — read for orientation /
  proof the result is achievable; never as an implementation source or design to mirror.
  `karis-2021-nanite-virtualized-geometry.pdf`

**Mesh-domain backbone:** Garland–Heckbert 1997 (QEM),
Hoppe 1996/1997 (progressive / view-dependent meshes), Cohen 1998 (appearance-
preserving), Cignoni 2005 (batched multi-triangulation), Yoon 2004 (Quick-VDR / CHPM),
Cignoni 2004 (adaptive TetraPuzzles), Haar–Aaltonen 2015 (GPU-driven), Jakob 2023
(meshlet strategies), Luebke 2001 (simplification survey), Gu–Gortler–Hoppe 2002 +
Sander 2003 (geometry images).

**Raycast capture & fuse:** Newcombe 2011 (KinectFusion), Kazhdan 2006 + Kazhdan–Hoppe
2013 (Poisson / Screened Poisson), Lorensen–Cline 1987 (Marching Cubes), Turk–Levoy
1994 (zippered meshes), Shade 1998 (layered depth images), Everitt 2001 (depth
peeling), Décoret 2003 (billboard clouds), Pfister 2000 (surfels), Rusinkiewicz–Levoy
2000 (QSplat), Munkberg 2022 (nvdiffrec) + Laine 2020 (nvdiffrast) + Nicolet 2021
(large steps) for the inverse-rendering bake variant.

**Virtual texturing / mips:** Williams 1983 (mipmaps), Heckbert 1986 (texture survey /
EWA), McCormack 1999 (Feline anisotropic), Losasso–Hoppe 2004 + Asirvatham–Hoppe 2005
(geometry clipmaps), van Waveren 2012/2013 + id Tech 5 (software/HW virtual textures),
Ka Chen 2015 (adaptive VT / Far Cry 4), Mayer 2010 (VT thesis/survey), Taibo 2009
(dynamic VT), Purnomo–Cohen 2004 (seamless atlases), Burley–Lacewell 2008 (Ptex).

**Leapfrog / neural & splat:** Kerbl 2023 (3D Gaussian Splatting), Ren 2024 (Octree-GS),
Müller 2022 (Instant-NGP), Fridovich-Keil 2021 (Plenoxels), Takikawa 2021 (Neural
Geometric LOD), Wang 2021 (NeuS), Chen 2022 (MobileNeRF), Yariv 2023 (BakedSDF),
Mildenhall 2020 (NeRF), Levoy–Hanrahan 1996 (light fields) + Gortler 1996 (Lumigraph),
Tewari 2021 + Chen 2024 (surveys).

---

*Canela = "cinnamon": the thin layer you dust on top. It is virtual geometry over the
shared foundation — a render-graph pass plus an asset path, not a new engine. Build the
proven cluster-DAG backbone first; layer the view-bounded raycast-fuse path on top; keep
the splat/neural far-field one flag away.*
