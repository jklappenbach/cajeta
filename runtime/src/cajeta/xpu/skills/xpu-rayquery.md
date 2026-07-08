---
id: xpu-rayquery
applies-to: [cajeta/gpu/AccelerationStructure, cajeta/gpu/RayQuery, cajeta/gpu/SoftwareRayQuery, cajeta/gpu/SwRayCursor, cajeta/gpu/AsImpl]
title: Inline ray query as a spatial index — host-built BVH, native vs software traversal
description: Build an AccelerationStructure on the host and walk it inside a kernel with RayQuery; choose native vs the portable SoftwareRayQuery/SwRayCursor tier, or just let Auto decide.
---

Use these classes when you need a hardware-accelerated **spatial index** (nearest-neighbour,
range/`countWithin`, ray casting) over a set of AABBs or triangles. The pattern: build an
`AccelerationStructure` (a BVH) on the **host**, pass it as a kernel argument, and inside the
kernel walk its intersection candidates with a `RayQuery`. For a point query, cast a degenerate
near-zero-length ray (`tMax` ≈ 0.001) centred on the query point and count/refine the candidates.

You almost never touch `SoftwareRayQuery`/`SwRayCursor` directly — they are the **portable
lowering** of `RayQuery` on non-Vulkan backends. You write one `RayQuery` walk; on a
ray-query-capable Vulkan device it lowers to `SPV_KHR_ray_query` hardware ops, everywhere else
it lowers to the `SoftwareRayQuery` stackless walk over a `KernelBuffer<float32>` BVH. **Same
source either way** — the tier is a build-time/heuristic choice, not a code fork.

## Members and roles

- `AccelerationStructure` (host handle) — the BVH. A small RAII handle (`deviceHandle` +
  `primitiveCount` + recorded `impl`) over a device resource, like `Texture2D`. **The entry
  point you construct.** Built from AABBs or triangles.
- `RayQuery` (device-only) — the kernel-local traversal cursor. **NOT a host handle**: a
  function-local object you declare on the kernel stack, `initialize` against a bound
  `AccelerationStructure`, then advance with `proceed()`. This is the surface you write against
  in both tiers.
- `AsImpl` (enum) — your **preference** for how the AS is built: `Auto` / `Software` / `Native`
  / `NativeNoFloor`. Passed to `AccelerationStructure.of`.
- `SoftwareRayQuery` (device-only, internal) — the `@Device` static helpers (`step`, `slabHit`)
  that a `RayQuery` op lowers to on a software backend. You do not call these.
- `SwRayCursor` (`@ValueType`, device-only, internal) — the by-value traversal state threaded
  through `SoftwareRayQuery.step`. The software-tier counterpart of the opaque
  `OpTypeRayQueryKHR`. You do not construct these.

## Collaboration / object graph

```
host:    AccelerationStructure(aabbs|verts)  ──build──▶  device BVH (handle + recorded impl)
                       │  passed by value as a kernel arg (borrowed for the launch)
                       ▼
kernel:  RayQuery rq;  rq.initialize(scene, ...)   while (rq.proceed()) { rq.candidate*()... }
                       │ lowers per backend, driven by the AS's recorded impl
        ┌──────────────┴───────────────┐
   native (Vulkan)                 software (everywhere else)
   OpRayQuery*KHR ops          SwRayCursor ⇄ SoftwareRayQuery.step(cursor, KernelBuffer<float32> bvh)
```

The verb follows the noun: the `impl` chosen at **build time** is recorded on the
`AccelerationStructure` (`implTag()`), and the `RayQuery` traversal lowers to match it — not to
whatever backend happens to be active. The launch asserts the compiled verb path matches the
noun's recorded impl.

## The cross-class call sequence

1. Host: `AccelerationStructure scene = heap AccelerationStructure(boxes, n);` (Auto), or
   `AccelerationStructure.of(boxes, n, AsImpl.Software)` to force a tier.
2. Host: `kernel.launch(s, grid:[...], block:[...])(scene, ...)` then `s.sync()`.
3. Kernel, per thread: declare `RayQuery rq;` → `rq.initialize(scene, rayFlags, cullMask,
   ox,oy,oz, tMin, dx,dy,dz, tMax)` → `while (rq.proceed()) { inspect rq.candidateType() /
   candidatePrimitiveIndex() / candidateDistance(); optionally confirmIntersection() /
   generateIntersection(t) }` → after the loop read `rq.committedType()` /
   `committedDistance()` / `committedPrimitiveIndex()`.
4. Host: device BVH frees automatically at `scene`'s scope exit.

## Ownership & lifecycle

- `AccelerationStructure` is **RAII over the drop chain**: the constructor acquires the device
  BVH; `~AccelerationStructure()` releases it at scope exit (null-guarded and idempotent — a
  moved-from `#scene` or a second drop is a no-op). `free` dispatches on the recorded `impl`.
- `of(...)` returns `#AccelerationStructure` (**owned** — you hold it; `#`-transfer to move).
- A launch **borrows** each `AccelerationStructure` argument (passed by value) until the next
  `GpuStream.sync()`. Do not free or let the handle drop before `sync()`.
- `RayQuery` and `SwRayCursor` **never cross the host boundary** — they are device-only,
  function-local, and have no host lifetime. Constructing/using a `RayQuery` on the host is
  unsupported. `SwRayCursor` is a value type: immutable inside a kernel, so `step` returns a new
  cursor each call (no field stores) — but you never see this; it is the lowering of `proceed`.

## Choosing a tier (Auto vs Software vs Native)

- **Default — `Auto`** (the no-`of` constructor): native fast path when the active device
  advertises native inline ray query (`Device.supports(Capability.RayQueryNative)`), the
  portable software BVH floor otherwise. Start here.
- **`AsImpl.Software`** — force the portable BVH even on a ray-query-capable GPU. Worth it when
  you know your pattern: hardware RT can **lose** to a software BVH at large query radius /
  extreme density.
- **`AsImpl.Native`** — prefer native; **falls back to software** if the device lacks it. Safe.
- **`AsImpl.NativeNoFloor`** — prefer native AND drop the software floor (trades the fallback
  for memory/build time). Only assert this when *every* consuming kernel is a supported native
  shape; a kernel that cannot traverse the native rep is diagnosed and its launch skipped.
- The `CAJETA_GPU_AS_IMPL=software|native` env var overrides the preference per process (env
  wins), mirroring `CAJETA_XPU_BACKEND`.

## What this does NOT do (avoid dead ends)

- **No top-level instancing, no instance transforms, no rebuild/refit** in v1 — a single
  bottom-level AS over AABB or triangle geometry, opaque.
- **Native triangle BLAS is software-only in v1** — triangle geometry builds/traverses on the
  CPU software path; the Vulkan triangle path is a follow-up.
- **`RayQuery` is Vulkan-only for the native tier** — `SPV_KHR_ray_query` is `[EnvVulkan]`. On
  other backends `RayQuery` lowers to the software walk; it is never a host call. The native
  candidate getters `candidateDistance`/`candidateBarycentric*` are **software-only in v1**
  (the native fork SPIR-V intrinsic is not yet carried).
- **No `mkdir`-style helpers, no committed hit for AABB-by-default**: AABB candidates are kind
  `1` and uncommitted; counting them is the range query. For a committed nearest hit on
  triangles, `confirmIntersection()` each accepted candidate (it shrinks `tMax`, so the last
  commit is nearest) and read `committedType()/committedDistance()` after the loop.
- **`candidateFrontFace()` on an unconfirmed candidate is unreliable on some drivers** (RADV) —
  `confirmIntersection()` then read `committedFrontFace()`. The software path is reliable for both.

## Worked example — range count with a degenerate ray (CPU software path)

Mirrors `test/xpu/CarameloSpatialIndexDeviceTests.cpp` (`kRqMinDriver`): build a BVH over 3 AABBs
and, per query point, count the boxes that contain it.

```cajeta
import cajeta.xpu.AccelerationStructure;
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.RayQuery;
import cajeta.xpu.GpuStream;
import cajeta.xpu.GpuThread;

public class RqMin {
    @Kernel
    public static void countHits(AccelerationStructure scene,
                                 KernelBuffer<float32> qx, KernelBuffer<float32> qy,
                                 KernelBuffer<float32> qz, KernelBuffer<uint32> out,
                                 uint32 n) {
        uint32 i = GpuThread.globalIdX();
        if (i < n) {
            RayQuery rq;                              // device-local; no host handle
            rq.initialize(scene, 0, 255,             // AS, rayFlags, cullMask
                          qx[i], qy[i], qz[i], 0.0f,  // origin, tMin
                          0.0f, 0.0f, 1.0f, 0.001f);  // direction, tMax (≈0: point query)
            uint32 c = 0;
            while (rq.proceed()) {
                if (rq.candidateType() == 1) { c = c + 1; }   // AABB candidate
            }
            out[i] = c;
        }
    }
    public static int32 run() {
        uint32 np = 3;
        float32[] boxes = heap float32[np * 6];       // 6 floats/box: minX,minY,minZ,maxX,maxY,maxZ
        // ... fill boxes ...
        AccelerationStructure scene = heap AccelerationStructure(boxes, np);  // Auto; RAII
        KernelBuffer<uint32> out = heap KernelBuffer<uint32>(4);
        // ... upload query buffers ...
        GpuStream s = GpuStream.current();
        countHits.launch(s, grid: [1], block: [64])(scene, qx, qy, qz, out, 4u);
        s.sync();                                     // scene borrowed until here
        // out now holds the per-point box count; scene's device BVH frees at scope exit
        return 0;
    }
}
```

To force the portable tier regardless of device, swap the build line for
`AccelerationStructure scene = AccelerationStructure.of(boxes, np, AsImpl.Software);` (add
`import cajeta.xpu.AsImpl;`).

Exact-distance refinement (RTNN): read `rq.candidatePrimitiveIndex()` inside the loop to recover
the data point and compute the true L2 distance — the AABB-overlap count is an L-infinity
over-approximation. See `cajeta-docs/gpu/RayQuery.md` for the frozen software-BVH buffer layout
and `cajeta/gpu/Capability.RayQueryNative` for the device probe behind `Auto`.
