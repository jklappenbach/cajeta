# Cajeta GPU — Splats: implementation plan

**Design contract:** [`docs/gpu/splats.md`](../../docs/gpu/splats.md) is the spec — the
*what/why* (data model, two-seam model, codec contract, render/compute paths). This is
the *how*: the build, staged. Where the two disagree, the spec wins and this plan is
corrected.

`cajeta.gpu.splat` is a **foundation** primitive on `cajeta.gpu.core`, shared by gfx
(render) and xpu (scatter). It is **not on the critical path** of canela or the gfx
renderer — those ship on meshes; splats are the optional photoreal/scientific tier. Each
stage below is independently shippable; **Stage 1 is the minimum that renders a splat at
all**, and later stages are gated only by the stage before them, not by any consumer.

Checkbox legend: `[x]` landed+tested · `[~]` partial · `[ ]` not started. Conventions
follow the foundation: `count()` (never `size`/`length`), drop idiom (`x = null`, no
`delete`/`free`), one allocation/borrow axis, value-type comparison masks
(`.all()`/`.any()`/`.select()`), env override `CAJETA_GPU_SPLAT_IMPL`. Commit only when
asked; no attribution trailer; stage files explicitly.

---

## 1. TDD

a. **Regression guards (must stay green throughout).**
   1. [ ] Existing `cajeta-gpu` / `cajeta-xpu` / `cajeta-gfx` tests unaffected; importing
      `cajeta.gpu.splat` does not perturb core verbs/nouns.
   2. [ ] `gpu-utils` sort + prefix-sum tests still green (splat reuses them for the
      depth sort and tile binning).
   3. [ ] Rebuild the `cajeta` compiler binary (`ninja cajeta`) so new splat
      kernels / `@Device` helpers register — else they silently no-op ("no registered
      kernel").
   4. [ ] The **CPU reference path** is the bit-checked oracle for every device path
      (per `CajetaGPU.md` §1.3 — portable and native each validate against the
      reference, not against each other).

b. **New red-first unit tests (kernel math & data structures).**
   1. [ ] 3D→2D covariance projection (EWA Jacobian) matches the analytic `Σ'` on known
      cameras; factored `Σ = R S Sᵀ Rᵀ` round-trips through scale+quaternion.
   2. [ ] Single-splat rasterization: one Gaussian renders the analytic footprint and
      alpha; transmittance early-out triggers at the threshold.
   3. [ ] Tile binning + sort: `(tileId, depth)` keys yield the correct per-tile
      front-to-back order; overlap counts match.
   4. [ ] `splatScatter` deposit into a grid equals the analytic weighted sum (P2G);
      grazing/edge weights correct.
   5. [ ] Codec round-trip: `encode→decode` is **bit-identical** lossless; each lossy
      scheme stays within its stated PSNR budget.
   6. [ ] PLY + SPZ import produce a `SplatCloud` matching a reference cloud.
   7. [ ] Backward pass: finite-difference gradient check passes for μ, scale, rotation,
      α, and appearance.
   8. [ ] `lod().select(...)` is deterministic; `decodeNode(span)` equals the
      corresponding subset of a full decode.
   9. [ ] Ray-query over splats returns the correct nearest hit / Gaussian response vs
      the CPU software traversal.
   10. [ ] Mesh extraction yields a watertight, genus-correct mesh within tolerance.

c. **New red-first integration tests (golden-image / golden-cloud).**
   1. [ ] Rasterized scene vs a reference image within tolerance, on **CPU + Vulkan +
      AMD**, with the native tier and the `CAJETA_GPU_SPLAT_IMPL=software` tier each
      validated against the reference.
   2. [ ] Train-then-render: a fit from images reaches a target PSNR within a bounded
      splat count.
   3. [ ] Material splats relit under a moving light match the reference relight
      (proves material captured, not lit color).
   4. [ ] One **xpu** scientific consumer (SPH P2G *or* tomographic back-projection)
      runs end-to-end through `splatScatter` and matches its analytic/reference result.

---

## 2. Deliverables

a. **Stage 1 — core noun + verbs (the render-at-all minimum).**
   1. [ ] `SplatCloud<A>` noun — SoA `Buffer<T>` per attribute (positions, log-scales,
      quaternions, logit opacities, appearance), `bounds`, `kind`, `count()`.
   2. [ ] `splatRasterize` **forward** — project → tile-bin (prefix-sum compaction) →
      sort (`gpu-utils`) → front-to-back blend with early-out.
   3. [ ] `splatScatter` — world-space anisotropic-kernel accumulate via
      `atomicFloatRMW` (the verb shared with §2.g).
   4. [ ] Two-seam registration through core (`LoweringTarget` verb + noun provider);
      CPU reference; `CAJETA_GPU_SPLAT_IMPL` (`native`|`software`) override + bit-check.

b. **Stage 2 — codec (encode/decode only; NO file I/O).**
   1. [ ] `SplatCodec.encode/decode<A>` against a `Stream` / `Buffer<u8>`; `.cajsplat`
      header + SoA streams; **lossless first**.
   2. [ ] PLY + SPZ import/export (SOG later).
   3. [ ] `decodeNode(stream, span)` ranged decode hook (octree spans; §2.e). Actual
      file/mmap/streaming reads belong to the asset / `cajeta.io` layer, not here.

c. **Stage 3 — anti-aliasing + appearance.**
   1. [ ] EWA projection filter + **Mip-Splatting** (3D smoothing + 2D dilation) — AA is
      mandatory because the splat tier is multi-resolution.
   2. [ ] `Radiance(shDegree 0..3)` evaluation at the view direction.
   3. [ ] `Material` (PBR) → write a G-buffer (albedo/normal/roughness) + **deferred
      relight** hook (the canela-relightable model).
   4. [ ] `2DGS` surface-aligned variant for geometric/normal accuracy (optional in-stage).

d. **Stage 4 — differentiable backward + fit.**
   1. [ ] Backward pass over the tile structure — gradients for all parameters.
   2. [ ] `SplatTrainer`: init (seed/SfM) → rasterize → image loss → Adam → adaptive
      **densify/prune** with a bounded splat count.

e. **Stage 5 — LOD + (codec side of) streaming.**
   1. [ ] Octree / anchor-structured LOD build; `lod().select(camera, screen, budget)`
      → active splat set.
   2. [ ] Compression schemes keyed in the header — attribute quantization, VQ+entropy,
      image-grid (SOG-style); lossless remains the oracle.
   3. [ ] Ranged `decodeNode` wired to the octree spans. Residency policy + actual reads
      stay in the consumer (canela's manager); splat only makes streaming *possible*.

f. **Stage 6 — ray-query path.**
   1. [ ] Build a core `AccelerationStructure` over per-splat 3σ AABBs + a Gaussian-
      response intersection program (the AABB/procedural ray-query consumer per
      `CajetaGPU.md` §4.1).
   2. [ ] Picking/selection, collision/NN, and secondary-ray queries over splats.

g. **Stage 7 — xpu scatter consumer (proves it's a foundation primitive).**
   1. [ ] `Payload<T>` appearance; `splatScatter` to an `Image3D` / grid.
   2. [ ] One scientific mapping end-to-end — SPH particle→grid *or* tomographic
      back-projection — validating that `splatScatter` is shared substrate, not a
      graphics-only feature.

h. **Stage 8 — conversion.**
   1. [ ] Mesh extraction (`SplatCloud → mesh`) via SuGaR / 2DGS — the bridge to
      collision/physics geometry.
   2. [ ] `Material` → standard PBR texture bake.

i. **Stage 9 — deferred.**
   1. [ ] Dynamic / 4D — canonical cloud + deformation field or control-point rig;
      `.cajsplat` 4D block.
   2. [ ] Editor / SLAM tooling (GaussianEditor-style edits; incremental RGB-D build).

j. **Cross-cutting.**
   1. [ ] Lifecycle/memory — splat resources are RAII values; release via `x = null`;
      built acceleration (order/BVH/octree) drops with the cloud.
   2. [ ] Promote shared primitives — confirm sort lives in `gpu-utils`; keep the
      EWA/scatter math reusable by xpu, not siloed in the renderer.
   3. [ ] Docs — keep [`docs/gpu/splats.md`](../../docs/gpu/splats.md) in sync; link from
      `CajetaGPU.md`.

---

## 3. Acceptance Criteria

a. [ ] A `SplatCloud` renders via `splatRasterize` on CPU + Vulkan + AMD, each
   cross-checked against the CPU reference.
b. [ ] Codec round-trips lossless and imports PLY/SPZ; **no file I/O lives in `splat`**
   (delegated to the asset / `cajeta.io` layer).
c. [ ] EWA + Mip-Splatting eliminates LOD/zoom aliasing (golden-image).
d. [ ] `Material` splats relight correctly under dynamic lighting (deferred) — material,
   not lit color.
e. [ ] `SplatTrainer` fits a cloud from images to a target PSNR with a bounded count.
f. [ ] `lod().select` keeps active-splat count + overdraw in budget; `decodeNode(span)`
   equals the full-decode subset.
g. [ ] Ray-query over splats returns the correct nearest hit / response vs CPU.
h. [ ] At least one xpu scientific consumer (SPH or tomography) runs through
   `splatScatter` — the foundation-primitive proof.
i. [ ] Mesh extraction yields a watertight, collider-quality mesh.
j. [ ] `CAJETA_GPU_SPLAT_IMPL=software` forces the portable tier on a native-capable
   device; both tiers validate against the reference.

---

## Verification commands

```sh
ninja cajeta                                   # register splat kernels/@Device helpers
ctest -j "$(nproc)" -R 'splat|Splat'           # targeted; full suite only on explicit ask
```

---

## Dependency notes & sequencing

- **Gated on `cajeta-gpu` core** — value types (`Vector`/`Matrix`/`Quaternion`),
  `Buffer<T>`, atomics (`atomicFloatRMW`), `Image2D`/`Image3D`, `AccelerationStructure` +
  ray query (Stage 6), and `gpu-utils` sort/scan. No new LLVM/backend work — splatting is
  built from existing core compute verbs (no fixed-function splat silicon anywhere).
- **No consumer blocks on this.** canela and the gfx renderer ship on meshes; this plan
  proceeds in parallel and is adopted per asset/consumer when earned.
- **Stage order is the dependency order** — 1 (render) → 2 (codec) → 3 (AA/appearance) →
  4 (fit) → 5 (LOD/stream) → 6 (ray query) → 7 (xpu scatter) → 8 (conversion) → 9
  (deferred). 6 and 7 may proceed in parallel after 1 (both need only the noun + a verb).
