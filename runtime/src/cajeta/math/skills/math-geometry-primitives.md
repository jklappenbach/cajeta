---
id: math-geometry-primitives
applies-to: [cajeta/math/Aabb, cajeta/math/Sphere, cajeta/math/Plane, cajeta/math/Ray, cajeta/math/Frustum]
title: Geometry primitives — Aabb, Sphere, Plane, Ray, Frustum for culling & picking
description: Stack value-type bounding/intersection primitives over Vector<float32,N> with inward-plane, unit-direction, and signed-distance conventions
---

# Geometry primitives (cajeta.math)

For culling and picking, pick by query shape, not by type:

| Want to… | Use |
| --- | --- |
| Coarse box bounds / BVH leaf, box-box and box-ray tests | `Aabb` |
| Rotation-invariant bounds, cheap sqrt-free containment | `Sphere` |
| A single half-space / signed-distance test | `Plane` |
| Picking / parametric `origin + t*dir` queries | `Ray` |
| Camera view-volume cull (the six clip planes) | `Frustum` |

All five are `@ValueType public final class` over the builtin `Vector<float32,N>`:
**stack-allocated by default, no heap, no drop chain** (same model as
`cajeta.lang.Optional`). Construct every one with `stack T(...)`. They are plain
values — pass by value, return by value; there is **no ownership transfer (`#`),
no `close()`, nothing to free**. Mutate by reassigning fields (all fields are
`public`). None of these mutate their arguments; every test returns a `boolean`
or `float32`.

## Members and roles

- **`Aabb`** — `min`/`max` corners (`Vector<float32,3>`). Invariant: `min[i] <= max[i]` on every axis (constructor does **not** enforce or sort it — pass them ordered). `center()`, `extent()`, `contains(p)`, `intersectsAabb(o)`, `intersectsRay(r)` (slab test).
- **`Sphere`** — `center` + `radius`. `contains(p)`, `intersectsSphere(o)` (both squared-distance, no sqrt), `intersectsRay(r)`.
- **`Plane`** — `normal` + constant `d`, the set `normal·x + d = 0`. `signedDistance(p)` is **positive on the side `normal` points toward**, negative behind, 0 on the plane. Factory `Plane.fromPointNormal(point, normal)` sets `d = -(normal·point)`.
- **`Ray`** — `origin` + `direction`, the half-line `origin + t*direction` for `t >= 0`. `at(t)` returns the point. `origin()`/`direction()` accessors.
- **`Frustum`** — six `Vector<float32,4>` planes `left/right/bottom/top/near/far`, each `(nx,ny,nz,d)`. `containsPoint(p)`, `intersectsSphere(s)`, `intersectsAabb(b)`.

## The conventions that tie them together

- **Inward-pointing plane normals.** A `Frustum`'s six plane normals all point *into* the volume, so "inside" is "signed distance `>= 0` on every plane" (`plane·(p,1) >= 0`). `Plane.signedDistance` uses the same positive-toward-normal sign.
- **Unit-direction ray assumption.** `Ray.direction` is stored **as given — not auto-normalized**. `Sphere.intersectsRay` *assumes* a unit-length direction (it uses the `a == 1` quadratic simplification); normalize the direction yourself before calling it. `Aabb.intersectsRay` (slab test) does not require unit length.
- **Unit-normal for metric distance.** `Plane.fromPointNormal` stores the normal verbatim (normalize first if you want `signedDistance` to be a true distance). `Frustum.fromViewProjection` normalizes each extracted plane for you.
- **Squared distance where possible.** `Sphere.contains`/`intersectsSphere` and `Aabb` tests avoid `sqrt`; `Sphere.intersectsRay` avoids it by reasoning on the discriminant and root signs.

## Frustum from the six clip planes

`Frustum.fromViewProjection(vp)` builds the six normalized planes from a
view-projection `Matrix<float32,4,4>` via Gribb-Hartmann row combinations, adapted
to `cajeta.math.Camera`'s convention (right-handed, clip depth `[0,1]`,
column-vector `m*v`, row-major). With rows `r0..r3`:

```
left = r0+r3   right = r3-r0   bottom = r1+r3   top = r3-r1   near = r2   far = r3-r2
```

`near` uses `row2` alone (the `[0,1]` depth range, not `r2+r3`). Each plane is
passed through `Frustum.normalizePlane`. `intersectsSphere`/`intersectsAabb` are
**conservative** culls: they return `false` only when the volume is fully outside
one plane, so a `true` may include some just-outside cases near corners — fine for
rejecting off-screen geometry, not exact.

## Worked example (mirrors test/gfx/GfxFrustumTests.cpp)

```cajeta
import cajeta.math.Frustum;
import cajeta.math.Sphere;
import cajeta.math.Aabb;

Matrix<float32,4,4> vp = proj * view;        // builtin matrix multiply (e.g. from Camera)
Frustum fr = Frustum.fromViewProjection(vp);

Vector<float32,3> c = stack Vector<float32,3>(0.0f, 0.0f, -5.0f);
Sphere bounds = stack Sphere(c, 1.0f);
boolean visible = fr.intersectsSphere(bounds);

Vector<float32,3> lo = stack Vector<float32,3>(-1.0f, -1.0f, -6.0f);
Vector<float32,3> hi = stack Vector<float32,3>(1.0f, 1.0f, -4.0f);
Aabb box = stack Aabb(lo, hi);
boolean boxVisible = fr.intersectsAabb(box);

// Picking with a ray (normalize the direction before the sphere test):
import cajeta.math.Ray;
Vector<float32,3> o = stack Vector<float32,3>(0.0f, 0.0f, 0.0f);
Vector<float32,3> d = stack Vector<float32,3>(0.0f, 0.0f, 1.0f);   // already unit
Ray r = stack Ray(o, d);
boolean hit = bounds.intersectsRay(r);
Vector<float32,3> p = r.at(5.0f);            // (0, 0, 5)
```

## Sharp edges

- **Codegen: one Vector operator/intrinsic per statement.** A compound `Vector`
  expression (e.g. `origin + direction * t` in one statement) or a method-call
  result used inline as an operator/intrinsic operand crashes host codegen today
  (cajeta-gfx §1 finding). Split into single-operator steps through an
  intermediate local — see `Ray.at`, `Aabb.center`, `Sphere.intersectsRay`. Write
  your own call sites the same way.
- **No divide-by-zero infinity reliance.** `Aabb.intersectsRay` special-cases
  axes with a zero direction component explicitly; the float codegen here does not
  guarantee IEEE-infinity behavior from a `1.0f/0.0f`.
- **What these do NOT provide:** no intersection *points/`t` values* (the ray
  tests return `boolean` only — use `Ray.at` to reconstruct a point), no Aabb
  union/grow/merge, no ray-vs-plane or ray-vs-frustum, no exact frustum cull, no
  auto-normalization, and no `min<=max`/unit-length validation. There are also no
  exceptions raised by this component — results are plain returns.

For the underlying `Vector<float32,N>` arithmetic, `length()`, `dot()`, and the
`Matrix` row accessor, see the builtin tensor/vector skills (`cajeta.math` tensor
primitives).
