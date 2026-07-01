# Spec: Streaming continuous-LOD geometry & its scheduling (`xpu-gfx-streaming-geometry`)

## 1. Definition

### 1.1 Purpose

Define the **graphics workload** the XPU kernel orchestrator
([`xpu-kernel-scheduling`](xpu-kernel-scheduling-spec.md), the `GFX` class) must
serve for real-time rendering of massive scenes: a **streaming, distance-reactive,
continuous level-of-detail (LOD) geometry pipeline** that maximizes rasterization
throughput while **never** interrupting, stuttering, or popping.

The target experience: as the viewer moves, geometry detail tracks distance
**continuously** — no discrete LOD swaps the eye can catch, no hitches while data
loads, no cracks between neighbouring surfaces. This is both a rendering
architecture and, centrally, a **scheduling** problem: the frame-critical
raster/selection work has a hard deadline, while geometry must stream in the
background without ever stalling the frame.

### 1.2 Scope

- The **rendering pipeline** as a graph of GPU passes (kernels): LOD selection,
  cluster culling, rasterization (deferred-material), and post.
- A **cluster-hierarchy continuous-LOD** representation and its **screen-space
  error** selection metric.
- **Anti-artifact** mechanisms: geomorphing (no popping), crack-free cluster
  boundaries (no seams), and mixed-LOD residency fallback (no interruption).
- **On-demand geometry residency streaming**: a working set spanning **at least
  three detail bands** around the current view — the band at the current
  distance, one **finer** (closer), one **coarser** (further) — plus the ability
  to render a **mixed** cut drawing different clusters at different bands
  simultaneously.
- The **scheduling contract** with the orchestrator: which passes are
  frame-critical (deadline) vs background (best-effort, preemptible), and how
  they interleave so raster is maximized and streaming is stall-free.

### 1.3 Non-goals

- Not a full renderer or material system; this specifies the geometry-LOD +
  streaming + scheduling substrate that a renderer sits on.
- Not a specific vendor's virtualized-geometry product; the techniques here are
  the underlying, portable primitives (progressive/cluster LOD, view-dependent
  refinement, visibility buffering, out-of-core streaming).
- Not authoring/simplification tooling (the offline build of the cluster
  hierarchy is assumed; runtime consumes it).

### 1.4 Principles

- **Continuous, not discrete.** LOD is a *cut* through a cluster hierarchy chosen
  per-view by screen-space error, refined smoothly — never a swap between a small
  set of whole-mesh LODs. Adjacent clusters may sit at different levels; the cut
  moves a cluster at a time.
- **Reactive to distance, at cluster granularity.** Each cluster's level is a
  function of its projected screen-space error (roughly, size / distance). Detail
  appears exactly where the pixels are.
- **Rasterize as much as possible, shade once.** Decouple rasterization from
  material shading (visibility buffer / deferred material) so the pipeline can
  push enormous triangle counts cheaply; do expensive shading once per visible
  pixel. Micro-triangles may take a compute/software raster path.
- **Never stall the frame.** Streaming residency runs *behind* the frame; if the
  ideal detail is not yet resident, render the **coarser** resident band (mixed
  LOD) and refine when it arrives. Availability, never a blocking wait.
- **Hide every transition.** Popping is removed by geomorphing across the
  transition; seams are removed by crack-free boundaries; loading hitches are
  removed by prefetch + background streaming that yields to frame work.

## 2. The representation — cluster hierarchy + screen-space error

### 2.1 Requirement

Geometry is a hierarchy (DAG) of small **clusters** (a few hundred–thousand
triangles each) across detail levels; the runtime selects, per frame, the **cut**
of clusters whose screen-space error is under threshold.

### 2.2 Mechanism

- Offline: partition each mesh into clusters; build parent clusters that simplify
  groups of children, recording a **geometric error** per cluster and a
  conservative bounding volume. Boundaries between sibling clusters are built to
  simplify consistently so a mixed cut stays **crack-free**.
- Runtime: project each candidate cluster's error to **screen space** (error ×
  projection / distance); descend where it exceeds the pixel threshold, stop
  where it is under. The result is a view-dependent cut — fine clusters near the
  camera, coarse far away, refined per cluster (Hoppe view-dependent refinement;
  cluster-DAG systems).

### 2.3 Use cases

- A wall near the camera resolves to thousands of fine clusters; the same wall in
  the distance collapses to a handful of coarse ones — continuously as you walk.

## 3. Anti-artifact: no popping, no cracks, no interruption

### 3.1 Requirement

Transitions between cuts must be **imperceptible**.

### 3.2 Mechanism

- **No popping** — when a cluster changes level, **geomorph**: interpolate its
  vertices from the old to the new positions over a few frames instead of
  snapping (Hoppe geomorphs; progressive-buffers geomorph on stream-in).
- **No cracks** — crack-free cluster boundaries: neighbouring clusters in the cut
  agree on shared edges (locked/consistently-simplified boundaries), so a mixed
  cut never gaps.
- **No interruption** — if the target level for a cluster is not resident, use
  the nearest resident **coarser** ancestor and geomorph up once the finer
  cluster streams in (mixed LOD is the graceful-degradation state, not an error).

### 3.3 Use cases

- Turning a corner reveals new detail; it geomorphs in rather than popping, even
  though it streamed in one frame late.

## 4. Streaming residency — the ≥3-band working set

### 4.1 Requirement

Maintain a resident working set that always spans **at least three detail bands**
around the current view — current, one finer, one coarser — so the selector can
always find a resident cluster to draw, and can refine or coarsen a step without
waiting.

### 4.2 Mechanism

- Treat cluster data like paged virtual memory: a **residency manager** keeps hot
  clusters resident and evicts cold ones (LRU + predicted view).
- **Prefetch** the finer band *ahead* of need from camera velocity/trajectory, so
  detail is resident before the screen-space error crosses the threshold.
- Keep the **coarser** band resident as the guaranteed fallback (never evict the
  ancestor of a visible cluster).
- Stream units are compressed; a **decompress/transcode** kernel runs on stream-in
  (background), then upload builds residency.
- The manager exposes, per cluster, `RESIDENT | STREAMING | EVICTED` so §3's
  mixed-LOD fallback is a pure lookup, never a stall.

### 4.3 Use cases

- Fast forward motion: the prefetcher pulls the finer band along the path; the
  viewer never sees the load.

## 5. The pass graph (GPU kernels)

### 5.1 Requirement

Enumerate the per-frame GPU passes and their dependencies so the orchestrator can
schedule them (§6).

### 5.2 Mechanism — per-frame DAG

| Pass | Kind | Roofline | Deadline |
|---|---|---|---|
| Instance/cluster **frustum + occlusion cull** (Hi-Z, two-pass) | compute (scan/compaction + gather) | memory-bound | frame-critical |
| **LOD selection** (screen-space error → cut) | compute (per-cluster) | latency/mem-bound | frame-critical |
| **Rasterization** (hardware for large tris; compute/software for micro-tris) into a **visibility buffer** | raster + compute | geometry/raster-bound | frame-critical |
| **Geomorph** vertex evaluation (in-flight transitions) | compute (map) | memory-bound | frame-critical |
| **Deferred material / shading** (resolve visibility buffer) | compute | compute+mem | frame-critical |
| Post (blur/AO/tonemap — see the scheduling spec's gfx notes) | compute (convolution/map) | memory-bound | frame-critical |
| **Residency streaming**: decompress/transcode + upload + prefetch | compute + copy | memory/copy-bound | **background, preemptible** |

Culling and LOD selection are **scan/compaction/gather** primitives (§ the kernel
taxonomy) — exactly the foundational primitives currently missing from the
library (`scan`, `scatter`, `compaction`) and needed to build this.

### 5.3 Use cases

- The visibility buffer lets rasterization be cheap and massive; shading cost is
  bounded by screen pixels, not scene triangles.

## 6. Scheduling contract with the orchestrator

### 6.1 Requirement

The orchestrator must run the frame-critical passes within the frame deadline at
maximum raster throughput, while the background streaming makes continuous
progress **without ever pushing the frame over deadline**.

### 6.2 Mechanism — maps onto `xpu-kernel-scheduling`

- **Frame-critical passes** run at the `GFX` deadline priority; the orchestrator
  guarantees they complete before present (tier-3 preemptive priority).
- **Background residency streaming** (decompress, upload, prefetch) is
  best-effort and **preemptible**: it co-runs (async-compute) with frame passes to
  fill idle memory bandwidth (tier-1 complementarity — decompress is
  memory/copy-bound, raster is geometry-bound) but **yields immediately** when a
  frame pass needs the resource. This is precisely the gfx policy in the
  scheduling spec: *async-compute fills gaps; best-effort preempts before
  present*.
- **Copy/DMA** overlap: uploads use copy queues so they overlap compute/raster.
- **Deadline governor**: if the frame is at risk, the orchestrator throttles
  streaming and the LOD selector biases toward the resident coarser band (graceful
  degradation) rather than blocking — the visual result is momentarily softer, not
  a hitch.
- **Prefetch scheduling**: the residency prefetcher is a low-priority producer;
  the orchestrator schedules it in whatever compute/copy bandwidth the frame
  leaves, using the interference model so it never slows a frame-critical pass.

### 6.3 Use cases

- A heavy frame preempts streaming → detail holds at the coarser band for a frame
  or two → streaming catches up in lighter frames → detail sharpens, all without a
  stall or a pop.

## 7. Metrics

- Frame-deadline attainment (no missed frames under motion).
- **Zero** perceptible pops/cracks (geomorph coverage; crack-free invariant).
- Streaming stall count = 0 (mixed-LOD fallback rate is allowed and measured).
- Rasterized triangles/frame and raster utilization (maximize).
- Resident-set size vs the ≥3-band target; prefetch hit rate.

## 8. Dependencies / risks

1. Requires the missing foundational kernels — **scan, scatter/compaction,
   gather, histogram** — for GPU-driven culling and LOD selection (kernel-library
   gap analysis). These lead the kernel backlog.
2. Software/compute rasterization for micro-triangles needs the raster path in the
   XPU backend; hardware raster covers large triangles.
3. Preemption/async-compute granularity varies by backend — the scheduling spec's
   tiered degradation applies (async where available; cooperative yield otherwise).
4. Crack-free boundaries + geomorphing depend on the offline cluster build; the
   runtime enforces the invariants but cannot fix a bad hierarchy.

## 9. References

Foundational papers + markers in
[`research/gfx-virtual-geometry/papers/`](../../research/gfx-virtual-geometry/papers/):
view-dependent refinement of progressive meshes (continuous LOD + geomorphing),
progressive buffers (out-of-core streaming LOD + geomorph on stream-in),
cluster-hierarchy continuous-LOD systems, and the visibility buffer
(raster-maximizing deferred material). Sibling specs:
[`xpu-kernel-scheduling`](xpu-kernel-scheduling-spec.md) (the orchestrator this
workload targets), `xpu-device-profile`, `kernel-occupancy-autotune`.
