# Flagship — Differentiable Gaussian-Splat Rendering — Specification

> Status: draft for review (2026-06-23). This is the Layer-2 flagship, the integration proof
> (`dev.cajeta.nucleo.geometry`). The companion analysis is `python-stack-analysis.md` §4.6 (the
> flagship), §2.3 (the column==tensor-buffer invariant), and §4.2 (the graphics lineage), with
> `target-experience.md` §4 (the splat flagship in code). Its siblings are `records-spec.md`
> (the `Splat` schema), `nucleo-frame-spec.md` (the `Table<Splat>`), `nucleo-column-spec.md`
> (the SoA per-field buffers), `nucleo-autograd-spec.md` (the engine the render is
> differentiated through), and `transform-intrinsics-spec.md` (the `Grad` combinator the render
> is wrapped in).
>
> This is a spec, so it states requirements and use cases, the why and the what. Build decisions,
> the how, are deferred as `> **TBD (plan-time):**` markers and collected in §10, to be resolved
> when this spec is turned into a plan. It is outline-numbered for addressability.

## 1. Definition

### 1.1 Purpose
A splat scene is a `Table<Splat>` of millions of rows, and rendering it is differentiable.
Optimizing a scene is gradient descent over the table's columns. The flagship exists to be
the integration proof. It is the one artifact where the dataframe (the table), the tensor (its
columns), autodiff (the `Grad` combinator), and rendering (the rasterizer) land on a single
set of bytes, one columnar engine viewed through four APIs (analysis §4.6). It hits both
top-tier priorities (graphics + ML) at once, and it is the first Layer-2 milestone because it
exercises the whole stack beneath it.

The load-bearing claim this spec is built to demonstrate follows from the column==tensor-buffer
invariant (`nucleo-column-spec.md` §1.1, analysis §2.3). A splat scene's columns are
simultaneously Arrow columns (load / store / filter / slice / stream) and tensor buffers, so the
fused, differentiable render kernels run over them directly, with no marshalling.

### 1.2 Scope
- The `Splat` record and the `Table<Splat>` it transposes onto (one `Column<T>` per field).
- Load / store of a splat table from a splat asset.
- A differentiable rasterize/render of a `Table<Splat>` from a `Camera` to an
  `Image<float32>`. Gradients flow back into the table's columns via the `Grad` combinator
  (`transform-intrinsics-spec.md`), differentiated through the autograd engine
  (`nucleo-autograd-spec.md`).
- A photometric loss against a target image and the gradients it yields on the columns.
- Scene optimization by gradient descent over the columns. An optimizer steps the columns.
- The far-field LOD use, in which distant geometry collapses to splats (the user's stated
  graphics-engine use).
- The CPU-table / GPU-draw boundary. The columnar table is the CPU-side
  authoring/optimization/storage form, and at draw time SoA columns transform to packed GPU buffers.

### 1.3 Non-goals
- **A general rasterization framework.** This spec is the splat render path and never a full
  graphics pipeline. Mesh rendering, materials, and lighting are out.
- **Topology and acceleration structures in the table.** Mesh connectivity is a graph rather than
  a column, and a BVH over geometry lives in the geometry subsystem over tensor buffers rather
  than as table rows (analysis §4.5 / §4.6 boundary; the `nucleo-frame-spec.md` §9 index interface
  is for the dataframe index, and it is not this). They stay out of the `Table<Splat>`.
- **The autograd engine and the VJP rules themselves.** The render's backward composes
  registered VJP rules. This spec consumes the engine (`nucleo-autograd-spec.md`,
  `transform-intrinsics-spec.md`) and does not define the differentiation mechanism.
- **Scene reconstruction / training infrastructure** (multi-view datasets, densification /
  pruning schedules, SfM initialization) beyond the per-step loss-and-grad loop. The training
  loop is shown. A full reconstruction pipeline is roadmap.
- **Point-cloud ingest** as a built-in importer. It is on the roadmap (§9) and outside v1.

### 1.4 Relationship to existing constructs
- **Records** (`records-spec.md`): `Splat` is an ordinary record schema. The render reads typed
  columns (`scene.pos`, `scene.opacity`) by field access, and a typo is a compile error.
- **The frame** (`nucleo-frame-spec.md`): `Table<Splat>` is a normal núcleo table. It
  load/store/filter/slices like any other, and the splat path adds only `render`.
- **The column** (`nucleo-column-spec.md`): each `Splat` field is one SoA `Column<T>`. The
  non-null numeric columns are bit-identical to tensor buffers, which is what lets the render
  kernels and the optimizer operate over them with no conversion.
- **The transform intrinsics** (`transform-intrinsics-spec.md`): `@Grad` / `Grad(...)` wraps the
  render-and-loss function. The backward is composed from the VJP registry and fuses with the
  forward like any other differentiated function.
- **`cajeta.xpu`**: the rasterizer lowers to the device model (the same `@Kernel` / device-IR
  path `cajeta.xpu` already exposes). The column→GPU-buffer handoff (§7) sits at that boundary.
- **`cajeta.math` / fixed-size shapes**: `Vec3`, `Quat`, `Matrix<float32,4,4>` camera matrices,
  and `SH3` are value-typed fixed-shape aggregates (compile-time shape, `target-experience.md`
  §6c).

## 2. The Splat schema and the splat table

A splat scene's schema is a record. The table transposes it to one column per field (SoA).
```cajeta
record Splat { Vec3 pos; Vec3 scale; Quat rot; float32 opacity; SH3 color; }

Table<Splat> scene = SplatScene.load("garden.splat");
```

**Use cases**
- **2.1** As a developer, when I declare `record Splat { Vec3 pos; Vec3 scale; Quat rot;
  float32 opacity; SH3 color; }`, then `Table<Splat>` has one SoA column per field
  (`pos`, `scale`, `rot`, `opacity`, `color`), each a `Column<T>` (per
  `nucleo-column-spec.md` / `records-spec.md` §7), known at compile time.
- **2.2** As a developer, when a scene has millions of rows, then each field is a single
  contiguous columnar buffer (millions of entries) rather than an array of `Splat` instances.
  The AoS `Splat` is the one-row view, and the physical storage is columnar.
- **2.3** As a developer, when I access a typed column (`scene.opacity`), then it resolves to a
  `float32` column accessor, and a typo (`scene.opcity`) is a compile error rather than a
  runtime key lookup.
- **2.4** As a developer, when a splat field is a non-null numeric (or fixed-shape value)
  type, then its column is bit-identical to a tensor buffer (the column==tensor-buffer
  invariant), so the render kernels and the optimizer read/write it directly.

> **TBD (plan-time):** The exact column layout of the composite fields. Either `Vec3`/`Quat`/
> `SH3` each become one fixed-width column of the value-type (a `Column<Vec3>`), or they are
> flattened to per-scalar planar columns (`pos.x` / `pos.y` / `pos.z`). The latter is closest to
> the GPU planar handoff (§7) but couples to how records-with-value-type-fields transpose
> (`records-spec.md` §7).

## 3. Load and store a splat table

**Use cases**
- **3.1** As a developer, when I call `SplatScene.load(path)`, then I get a `Table<Splat>`
  whose columns are populated from the on-disk splat asset. It is ordinary table I/O, with no
  special decode path beyond the format reader.
- **3.2** As a developer, when I store a scene (`scene.save(path)`), then the table's columns
  are written back to the splat asset format, round-tripping the schema.
- **3.3** As a developer, when I load a scene, then it is a normal núcleo table. I can
  `filter` / `slice` / `count` / inspect its columns with the dataframe surface
  (`nucleo-frame-spec.md`) before or after rendering, because a splat column is a frame
  column.

> **TBD (plan-time):** The splat asset format(s) read/written in v1 (e.g. `.splat`, `.ply`-based
> Gaussian-splat, a núcleo-native columnar form) and whether load materializes directly into
> Arrow-conformant `Column<T>` (zero-copy where the on-disk encoding already matches the
> in-memory layout, cross-spec with the codec lib, mirroring `nucleo-column-spec.md` §10).

## 4. Differentiable render — gradients flow into the columns

The render is the heart of the flagship. It is a rasterize from a camera to an image that is
differentiable, so that wrapping it in `Grad` makes gradients flow back into the table's
columns. The forward `render` stays intact. `Grad` synthesizes the backward by composing the
registered VJP rules of the primitives the render is built from (`transform-intrinsics-spec.md`
§5, §7).
```cajeta
@Grad
float32 photometricLoss(Camera cam, Image<float32> target) {
    var rendered = scene.render(cam);     // GPU rasterize, fused, differentiable
    return (rendered - target).square().mean();
}
```

**Use cases**
- **4.1** As a developer, when I call `scene.render(cam)` for a `Table<Splat>` and a `Camera`,
  then I get an `Image<float32>`. The splats are projected, sorted, and alpha-composited into a
  raster, lowered to the GPU via `cajeta.xpu` and fused, as one set of kernels over the columns
  with no per-splat temporaries materialized.
- **4.2** As a developer, when I wrap a function that calls `render` in `@Grad`
  (`Grad(photometricLoss)`), then the render is differentiable. The backward is composed
  from the render primitives' VJP rules and emitted as ordinary IR that fuses with the forward
  (`transform-intrinsics-spec.md` §5.4).
- **4.3** As a developer, when the backward runs, then gradients flow into the table's
  columns. There is a cotangent for `pos`, `scale`, `rot`, `opacity`, and `color`, because the
  columns are the differentiable parameters, by the column==tensor-buffer invariant.
- **4.4** As a developer, when a render primitive lacks a registered VJP rule, then `Grad`
  fails loud at compile time, naming the primitive. It is never a silent zero/wrong gradient
  (`transform-intrinsics-spec.md` §5.3).
- **4.5** As a developer, when I render the same scene from two cameras, then the forward is a
  static compute graph over millions of splats fused and lowered once per shape. It is never an
  eager per-op tape, since the eager tape would die by per-op dispatch before it rendered. The
  compiled, fused path is load-bearing here (analysis §4.4).

> **TBD (plan-time):** [R1] The rasterizer scope and algorithm, meaning tile-based sorted
> alpha-compositing (the 3DGS reference), the depth-sort / culling strategy, the
> per-pixel/per-tile parallelization, and which parts are expressed in differentiable
> primitives vs. delivered as a Tier-B fused IR span with a hand-authored VJP
> (`transform-intrinsics-spec.md` §8). The render's differentiability stance (§4.2) holds
> regardless, but the algorithm choice determines the VJP-rule surface.

## 5. Photometric loss and column gradients

**Use cases**
- **5.1** As a developer, when I compute `(rendered - target).square().mean()` for a rendered
  `Image<float32>` and a target `Image<float32>`, then I get a scalar photometric loss. It is an
  ordinary fused tensor expression over the image buffers (`nucleo-expr-spec.md`).
- **5.2** As a developer, when I call the `Grad`-wrapped loss
  (`photometricLoss.withGrads(cam, target)`), then I get back value + grads, which are the loss
  value and the gradient w.r.t. the scene's columns, as a typed return bag (the companion
  shape, `transform-intrinsics-spec.md` §5.1, `records-spec.md`).
- **5.3** As a developer, when I read the returned grads, then they are explicit return
  values keyed to the columns. There is no global `.grad` accumulator, no `requires_grad` bool,
  and no `zero_grad` footgun. This is the deliberate correction (`target-experience.md` §2,
  `transform-intrinsics-spec.md` §5.2).
- **5.4** As a developer, when I want to hold parts of the scene fixed (e.g. freeze positions,
  optimize only color), then I can mark that region/parameter `@NoGrad` so no gradient flows
  into those columns (`transform-intrinsics-spec.md` §9.1).

> **TBD (plan-time):** [R2] The grad-companion's exact mapping to columns. Grads may come
> back as a parallel `Table<Splat>` of cotangents (one grad column per parameter column), a
> per-column grad bag, or a flat tensor the optimizer interprets. This is resolved jointly with the
> `Grad` companion shape (`transform-intrinsics-spec.md` F3) and the optimizer's column-step
> surface (§6).

## 6. Optimize a splat scene — gradient descent over columns

```cajeta
var opt = optim.Adam(scene.columns(), lr: 1e-2);

for (var view : trainingViews) {
    var loss = photometricLoss.withGrads(view.cam, view.image);
    opt.step(loss.grads);                 // gradient descent over splat COLUMNS
}
```

**Use cases**
- **6.1** As a developer, when I construct an optimizer over `scene.columns()`, then the
  optimizer's parameters are the table's columns. Stepping the optimizer updates the
  column buffers in place, since the columns are the trainable state (`nucleo-optim` over
  `nucleo-autograd`).
- **6.2** As a developer, when I call `opt.step(loss.grads)` in a per-view loop, then the
  column buffers are updated by gradient descent against the photometric loss, and the scene
  converges toward the target views.
- **6.3** As a developer, when the optimization runs, then no subsystem boundary is crossed.
  The table (dataframe), its columns (tensors), the `@Grad` transform (autodiff), and `render`
  (rendering) all operate on the same column bytes. This is the integration proof
  (`target-experience.md` §4).
- **6.4** As a developer, when the scene changes size during optimization (splats added/pruned),
  then that is a table mutation (rows appended / filtered), addressed by the frame surface.
  It is out of this spec's core loop but compatible with it. Densification is roadmap (§9).

> **TBD (plan-time):** [R3] The `scene.columns()` → optimizer surface. This covers which columns
> are trainable by default (all float columns vs. an explicit selection), how the optimizer
> addresses a heterogeneous parameter set (position vs. opacity vs. SH coefficients may want
> different learning rates / schedules), and how an in-place column update interacts with the
> immutable-Arrow-batch convention (`nucleo-column-spec.md`).

## 7. The CPU-table / GPU-draw boundary

The columnar table is the CPU-side authoring / optimization / storage form. At draw
time, the SoA columns transform to packed GPU vertex/index buffers. That is a mechanical map,
because SoA Arrow columns lay onto planar GPU buffers (analysis §4.6 boundary).

**Use cases**
- **7.1** As a developer, when a scene is being authored, optimized, or stored, then it lives
  as a CPU-side `Table<Splat>` (columnar, filterable, streamable). The table is the working
  form for everything except the final draw.
- **7.2** As a developer, when I draw the scene, then the SoA columns transform to packed GPU
  draw buffers (planar buffers per column). The handoff is mechanical because the SoA layout
  already matches the planar GPU layout, with no AoS repack.
- **7.3** As a núcleo author, when topology or acceleration structures are needed for the draw
  (mesh connectivity, a BVH), then they live outside the table, in the geometry subsystem
  over tensor buffers rather than as table rows (analysis §4.5 / §4.6 boundary). That keeps the
  table a pure attribute store.

> **TBD (plan-time):** [R4] The GPU draw-buffer handoff details, meaning the exact column→planar
> GPU buffer transition (alignment, residency, who owns the device buffer), whether it reuses
> the device-resident column representation (`nucleo-column-spec.md` §8) or a render-specific
> packing, and how the differentiable render kernels (which read columns directly) relate to the
> draw-buffer packing (do they share device buffers or is draw a separate, non-differentiable
> consumer?).

## 8. Far-field LOD — distant geometry collapses to splats

The user's stated graphics-engine use is that at distance, detailed geometry is represented as
splats.

**Use cases**
- **8.1** As a graphics-engine author, when geometry is far from the camera, then I substitute a
  splat representation (`Table<Splat>`) for the full-detail mesh. That is a level-of-detail
  collapse to splats for far-field geometry.
- **8.2** As a graphics-engine author, when I use splats as far-field LOD, then the same splat
  table type, render path, and (where I want it) differentiable optimization apply. The LOD
  use is the same artifact as the flagship and never a separate code path.

> **TBD (plan-time):** [R5] The LOD policy itself, meaning the distance/screen-coverage threshold
> for collapsing to splats, how a mesh is converted to a splat table (a bake step), and whether
> the conversion is differentiable (optimizing the splat LOD against the full-detail render).
> It is a roadmap dimension layered on the core render path (§4).

## 9. Roadmap — point-cloud ingest

**Use cases**
- **9.1** As a developer, when I have a point cloud, then I want to ingest it into a
  `Table<Splat>` (initialize a scene from points). This is on the roadmap and outside v1.

> **TBD (plan-time):** [R6] Point-cloud ingest formats and the points→`Splat` initialization
> (which fields are seeded from points vs. defaulted, e.g. position from points, scale/opacity/
> SH from a heuristic), and the asset formats supported (PLY point cloud, LAS/LAZ, núcleo-native).

## 10. Acceptance criteria (spec-level)
- A `Table<Splat>` for `record Splat { Vec3 pos; Vec3 scale; Quat rot; float32 opacity;
  SH3 color; }` is a normal núcleo table with one SoA column per field. A million-row scene is
  columnar rather than AoS, and a field typo is a compile error.
- A splat table loads from and stores to a splat asset, round-tripping the schema.
- `scene.render(cam)` produces an `Image<float32>` lowered to GPU and fused.
- The render is differentiable. `@Grad`/`Grad(...)` over a render-and-loss function yields a
  backward composed from registered VJP rules, gradients flow into the scene's columns, and a
  missing VJP rule is a hard, named compile error (no silent wrong gradient).
- A photometric loss vs. a target image yields value + explicit grads on the columns (no
  global `.grad` state).
- An optimizer over `scene.columns()` steps the column buffers by gradient descent and the
  scene converges, with no subsystem boundary crossed (one set of bytes: table + tensor +
  autodiff + render).
- The columnar table is the CPU-side form. At draw time SoA columns map mechanically onto planar
  GPU buffers, and topology and acceleration structures stay out of the table.
- Splats are usable as far-field LOD with the same table type and render path.

## 11. Open questions (resolve at plan time)
- **[R1]** Rasterizer scope and algorithm (tile-based sorted alpha-compositing, sort/cull,
  differentiable-primitive vs. Tier-B-IR span split) (§4).
- **[R2]** The grad-companion → column mapping (parallel cotangent table vs. per-column bag vs.
  flat tensor); jointly with `transform-intrinsics-spec.md` F3 and §6 (§5).
- **[R3]** The `scene.columns()` → optimizer surface (which columns are trainable, per-parameter
  schedules, in-place update vs. immutable-batch convention) (§6).
- **[R4]** The GPU draw-buffer handoff details (column→planar transition, device-buffer
  ownership, relationship to the differentiable render kernels' device buffers) (§7).
- **[R5]** The far-field LOD policy and mesh→splat bake (threshold, conversion, whether the bake
  is differentiable) (§8).
- **[R6]** Point-cloud ingest formats and the points→`Splat` initialization (§9).
- **Composite-field column layout**, `Vec3`/`Quat`/`SH3` as one value-type column each vs.
  flattened per-scalar planar columns, and how that couples to records-with-value-type-fields
  transposition and the GPU planar handoff (§2, §7).
