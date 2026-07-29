---
id: xpu-acceleration-structure
applies-to: [cajeta/gpu/AccelerationStructure]
title: Building a GPU AccelerationStructure (BVH handle)
description: Construct an AABB or triangle BVH host-side, pick the impl, and pass it into a kernel for RayQuery.
---
# AccelerationStructure — host-side BVH handle

A small host-side handle (`deviceHandle` + `primitiveCount` + recorded `impl`) over a
device bounding-volume hierarchy. You **build it on the host** and pass it as a `@Kernel`
argument; the actual traversal happens **in-kernel** via `RayQuery` (see
`cajeta/gpu/RayQuery`) — there is no host-side query method here. Beyond ray tracing it
doubles as a hardware spatial index: build over AABBs, cast a near-zero-length ray, walk
candidates for nearest-neighbour / range queries. Member of package `cajeta.xpu`.

This is an **entry-point type** — you construct it directly.

## Pick a constructor by geometry, and a factory for explicit impl

Three host build paths; **arity selects the geometry**:

```cajeta
import cajeta.xpu.AccelerationStructure;
import cajeta.xpu.AsImpl;

// AABBs: 6 float32 per box (minX,minY,minZ, maxX,maxY,maxZ), row-major.
// impl chosen by the Auto capability heuristic (native if the device supports
// inline ray query, else the portable software BVH).
AccelerationStructure scene = heap AccelerationStructure(boxes, n);

// Triangle soup: vertex v of triangle t starts at vertices[(t*3+v)*stride],
// spanning x,y,z; stride = floats per vertex (3 = tightly packed).
// 3-arg arity selects the triangle path. v1: CPU software BVH only.
AccelerationStructure mesh = heap AccelerationStructure(verts, nTris, 3u);

// of(...): force the impl, overriding the Auto heuristic. AABB geometry ONLY.
AccelerationStructure soft = AccelerationStructure.of(boxes, n, AsImpl.Software);
```

`of` is a **static factory, not a constructor**, because a 3-arg AABB+impl ctor would
collide with the 3-arg triangle ctor `(float32[], uint32, uint32)` and silently
mis-resolve. So the explicit-impl override of the AABB build goes through `of`; there is
**no** triangle equivalent of `of`.

## Construction & ownership

- Constructor args (`aabbs`/`vertices`) are **borrowed**, not `#`-transferred — the build
  reads them and you keep owning the array.
- The two-arg/three-arg constructors require `heap` (RAII): the ctor acquires the device
  BVH; `~AccelerationStructure()` releases it.
- `of(...)` returns `#AccelerationStructure` — an **owned** handle transferred to you.
  Bind it (`AccelerationStructure soft = ...of(...)`) so the drop chain frees it at scope
  exit, exactly like the `heap` ctors.
- The `AsImpl` preference widens to its ordinal at the ABI boundary; `of` builds the
  representation and records the resolved impl in one shared resolver call, so the built
  rep and `implTag()` always agree.

## Lifecycle

Governed by the drop chain — `~AccelerationStructure()` frees the device BVH at scope
exit (the `KernelBuffer`/`Texture2D` RAII convention). It is **null-guarded and idempotent**:
a prior free, or a moved-from handle (`#scene`), makes the destructor a no-op — so moving
the handle into a callee does not double-free. A launch **borrows** each
`AccelerationStructure` argument until the next `GpuStream.sync()`; do not let it drop
before you sync.

## The methods that matter

- `uint32 count()` — primitive count.
- `int32 implTag()` — the single recorded impl as a **`CajetaAsImpl`** ordinal
  (`0` = portable software BVH, `1` = native BLAS). NOTE this is a *different* enum from
  the `AsImpl` *preference* you pass in (`Auto/Software/Native/NativeNoFloor`); do not
  compare `implTag()` against `AsImpl` ordinals.
- `int32 implSet()` — bitmask (`1 << impl`) of **all** representations carried. A native
  build may also retain the software floor; the launch-time selector picks the best impl
  in this set each consuming kernel can traverse, so an unsupported-shape kernel falls
  back to the software floor instead of faulting on a native handle.

## Impl resolution (capability heuristic + override)

`Auto` (the plain-ctor default) takes the native fast path when the active device
advertises inline ray query (`Device.supports`) and the software floor otherwise.
`AsImpl.Software`/`Native` force the choice; `Native` falls back to software when the
device lacks ray query; `NativeNoFloor` additionally drops the software floor (assert all
consumers are supported native shapes). The `CAJETA_GPU_AS_IMPL=software|native` env var
overrides per process (env wins). Full preference semantics live in `cajeta/gpu/AsImpl`.
The verb follows the noun: the recorded impl, not the active backend, drives which
`RayQuery` path runs.

## What it does NOT do (v1)

- No host-side traversal/query — query in a kernel with `RayQuery`.
- No instance transforms, no top-level (instancing) AS, no rebuild/refit — a single
  bottom-level AS per handle.
- Triangle geometry is **CPU software only** in v1 (native triangle BLAS is a follow-up);
  `of` does not accept triangle geometry.
- Does not take ownership of the input vertex/AABB array.

## Worked example (mirrors CarameloSpatialIndexDeviceTests)

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
                                 KernelBuffer<float32> qz, KernelBuffer<uint32> out, uint32 n) {
        uint32 i = GpuThread.globalIdX();
        if (i < n) {
            RayQuery rq;                                   // see cajeta/gpu/RayQuery
            rq.initialize(scene, 0, 255, qx[i], qy[i], qz[i], 0.0f,
                          0.0f, 0.0f, 1.0f, 0.001f);
            uint32 c = 0;
            while (rq.proceed()) { if (rq.candidateType() == 1) { c = c + 1; } }
            out[i] = c;
        }
    }

    public static int32 run() {
        uint32 np = 3;
        float32[] boxes = heap float32[np * 6];           // 6 floats per AABB
        // ... fill boxes ...
        AccelerationStructure scene = heap AccelerationStructure(boxes, np);
        KernelBuffer<uint32> out = heap KernelBuffer<uint32>(4);
        GpuStream s = GpuStream.current();
        countHits.launch(s, grid: [1], block: [64])(scene, qx, qy, qz, out, 4u);
        s.sync();                                         // scene borrowed until here
        // device BVH freed automatically at scope exit
        return 0;
    }
}
```

To force the software BVH on a ray-query-capable device, swap the build line for
`AccelerationStructure scene = AccelerationStructure.of(boxes, np, AsImpl.Software);`
(add `import cajeta.xpu.AsImpl;`) — the recorded impl then drives the kernel onto the
SoftwareRayQuery walk, and results match the native path.
