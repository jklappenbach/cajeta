# mesh-simplifier-quadratic-host — `MeshSimplifier` is host code with a quadratic driver (defect spec)

> Status: **draft**, filed 2026-10-25 by benchmark-reference unit 14 on v0.46.3.

## 1. Definition
`cajeta.xpu.mesh.MeshSimplifier` runs on the host and its driver is O(T²) in
triangles: 2048 → 204 takes 184 ms against meshoptimizer's 0.45 ms, and the
catalog's 1e6 → 1e5 shape does not finish, so `xpu/mesh-simplify` measures
19,800 → 1,980 and records the page shape as a skip.

## 2. Use cases
- **2.1** As a caller, when I simplify a 1e6-triangle mesh, then it completes
  in seconds on the host (a priority queue over edge collapses, O(T log T))
  and on the device where the package lives.
- **2.2** As the test suite, then a test simplifies 1e5 triangles under a
  time bound and checks the Hausdorff bound.

## 3. Acceptance
`xpu/mesh-simplify` runs the page shape and its skip row is removed.
