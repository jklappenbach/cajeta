---
id: gpu-rayquery-class
applies-to: [cajeta/gpu/RayQuery]
title: RayQuery — device-only inline ray-query cursor and its proceed() protocol
description: How to stack-construct, initialize, and walk a RayQuery's candidate loop inside a kernel, plus its committed accessors and Vulkan-only limits.
---

# RayQuery

A `RayQuery` is a **device-only, function-local cursor** that traces ONE ray through an `AccelerationStructure` and walks its intersection candidates, entirely inside a compute kernel (the inline `SPV_KHR_ray_query` model — no ray-tracing pipeline, no hit shaders). It is the *verb* over an `AccelerationStructure` *noun*.

It is NOT a host handle. Unlike `AccelerationStructure`/`GpuBuffer`/`Texture2D`, you never create or touch a `RayQuery` on the host — it lowers to an `OpVariable Function` of `OpTypeRayQueryKHR`. There is no `deviceHandle`, no drop chain, no `close()`/`free()`; it lives and dies with its kernel stack frame.

## Access-point flag

Construct-on-stack inside a kernel. You build a `RayQuery` yourself (`stack RayQuery()`); you do NOT receive one from a factory. To get one you must already hold an `AccelerationStructure` (built on the host, passed as a `@Kernel` argument) to initialize it against.

## The required protocol (call order is load-bearing)

```
stack RayQuery()            // 1. construct on the kernel stack
  .initialize(...)          // 2. begin a query against a bound AS
while (proceed())           // 3. advance; true = a candidate to inspect
    candidateType()/...     //    inspect the CURRENT candidate
    confirm/generate...     //    (optional) commit it as the hit
committed*()                // 4. read the result AFTER the loop ends
```

`initialize` must precede `proceed`; the `candidate*` accessors are only valid *inside* the loop (they read the current candidate); the `committed*` accessors are only valid *after* `proceed()` returns false. Reading a committed value mid-loop, or a candidate value after the loop, is meaningless.

## Construction

`stack RayQuery()` — the no-arg constructor. (The lone `int32 reserved` field exists only so the host front-end gives the class a non-empty layout; ignore it.) No ownership transfer, nothing to free.

## The methods that matter

- `void initialize(AccelerationStructure as, uint32 rayFlags, uint32 cullMask, float32 originX, originY, originZ, float32 tMin, float32 dirX, dirY, dirZ, float32 tMax)` — begin the query. `as` is **borrowed**, not consumed (the kernel borrows every AS argument until the next `GpuStream.sync()`). `rayFlags` = standard ray-flags bitmask (0 = none); `cullMask` = 8-bit instance cull mask (255 = all). The ray is `(origin, direction)` over parametric extent `[tMin, tMax]`. Origin and direction are passed as **three float32 scalars each** — RayQuery deliberately does not depend on `Vector<T,N>`; the lowerer assembles the `<3 x float>` vectors.
- `boolean proceed()` — advance to the next candidate. `true` while a candidate remains to inspect, `false` once traversal is complete. This is the loop condition.
- `uint32 candidateType()` — type of the CURRENT candidate: `0` = triangle, `1` = AABB (procedural). Read inside the loop to decide how to test.
- `uint32 candidatePrimitiveIndex()` — caller primitive index of the current candidate (which indexed datum it is). Read inside the loop to recover your data point.
- `void confirmIntersection()` — commit the current TRIANGLE candidate as the hit; shrinks the ray's effective `tMax` to this candidate's `t`, so remaining traversal only finds closer hits (call per accepted triangle → committed ends up nearest).
- `void generateIntersection(float32 t)` — commit an AABB (procedural) candidate at your own computed distance `t` (must be within the current extent); also shrinks `tMax`.
- `uint32 committedType()` — read AFTER the loop: `0` = none committed, `1` = triangle, `2` = generated. `0` is the "no hit" sentinel.
- `committedDistance()`, `committedPrimitiveIndex()`, `committedBarycentricU/V()`, `committedFrontFace()` — committed-hit geometry, valid only when `committedType() != 0`.

Candidate geometry getters — `candidateDistance()`, `candidateBarycentricU/V()`, `candidateFrontFace()` — exist for the in-loop refinement (e.g. RTNN exact-distance) but see "What it does NOT do".

## When you do NOT need confirm/generate

For a pure **spatial-index** query (nearest-neighbour / range / count), you do not commit anything: cast a degenerate near-zero-length ray (`tMax ≈ 0`) centred on the query point and just *count or gather* the AABB candidates the loop visits — the candidate visit itself is the payload. Commit only when you want a single nearest geometric hit reported through `committed*`.

## Idiomatic example — range count via spatial index

```cajeta
package myapp;

import cajeta.gpu.RayQuery;
import cajeta.gpu.AccelerationStructure;
import cajeta.gpu.GpuBuffer;
import cajeta.gpu.GpuThread;

// Count, per query point, how many AABB leaves a zero-length ray overlaps.
@Kernel
void countNeighbors(AccelerationStructure scene,
                    GpuBuffer<float32> queries,   // xyz packed per point
                    GpuBuffer<uint32>  counts) {
    uint32 i = GpuThread.globalIdX();
    float32 px = queries[i * 3u];
    float32 py = queries[i * 3u + 1u];
    float32 pz = queries[i * 3u + 2u];

    RayQuery rq = stack RayQuery();
    rq.initialize(scene, 0u, 255u,          // AS (borrowed), rayFlags=none, cullMask=all
                  px, py, pz, 0.0f,          // origin, tMin
                  0.0f, 0.0f, 1.0f, 0.0f);   // direction, tMax=0 (degenerate)

    uint32 n = 0u;
    while (rq.proceed()) {
        if (rq.candidateType() == 1u) {      // an AABB / procedural candidate
            n = n + 1u;                       // visit it; do NOT commit — keep walking
        }
    }
    counts[i] = n;                            // committedType() would be 0 here (nothing committed)
}
```

Host side builds the noun and launches; the AS is freed by its own drop chain after `sync()` (see `cajeta/gpu/AccelerationStructure`).

## State, lifecycle, concurrency

Per-invocation and single-use in spirit: each kernel thread holds its own stack `RayQuery`; it is mutated by `initialize`/`proceed`/`confirm`/`generate`. Re-`initialize` to trace another ray from the same cursor. Nothing is shared across threads and nothing needs disposal — it dies with the stack frame.

## What it does NOT do / sharp edges

- **Vulkan-only.** `SPV_KHR_ray_query` is an `[EnvVulkan]` extension; these ops lower only on the Vulkan/SPIR-V backend. A `RayQuery` in a kernel on any other backend is a clean "not supported on backend" diagnostic — not a runtime fault. (The portable software tier is a *separate* path, `SoftwareRayQuery` + `SwRayCursor`, that the lowerer routes a `RayQuery` to on non-native devices; you still write `RayQuery`.)
- **No host use.** Constructing/using a `RayQuery` on the host is unsupported.
- **Native candidate-geometry getters are v1-incomplete.** `candidateDistance()`, `candidateBarycentricU/V()`, `confirmIntersection()`, `generateIntersection()`, and the `committed*` getters are **software-tier (CPU) only in v1** — the native Vulkan path needs a fork SPIR-V intrinsic not yet carried. On the native path, v1 reliably gives you `proceed()`, `candidateType()`, `candidatePrimitiveIndex()`, and `committedType()` — enough for the candidate-visit spatial walk. Do not depend on native committed distance/barycentrics yet.
- **`candidateFrontFace()` is unreliable on some drivers** (observed non-deterministic on RADV for an *unconfirmed* candidate). For a dependable winding result, `confirmIntersection()` then read `committedFrontFace()`. The software path is reliable for both.
- **No exceptions.** These are intrinsics lowered at the call site, not real calls; failure is a compile-time backend diagnostic, not a thrown error. `committedType() == 0` is the "no hit" sentinel — there is no null.

See `cajeta/gpu/AccelerationStructure` for building/owning the noun and the `AsImpl` software-vs-native choice that decides whether a `RayQuery` lowers to the hardware op or the software walk.
