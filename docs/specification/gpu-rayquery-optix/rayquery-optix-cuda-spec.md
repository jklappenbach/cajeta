# OptiX-Backed Native RT-Core Ray Query (NVIDIA CUDA) — Spec

## 1. Background

cajeta's `RayQuery` runs today on four backends. Three use the **portable
software BVH** (`SoftwareRayQuery` walk over a `Buffer<float32>`): CPU, NVPTX
(CUDA), AMDGPU (HIP). One uses a **native hardware** path: Vulkan, via
`OpRayQuery` over a `VK_KHR_acceleration_structure` BLAS/TLAS — now on-device
validated on the RTX 4090 (see `docs/specification/gpu-rayquery-native/`).

On NVIDIA's **CUDA** backend the RT cores are *not* reachable from a compute
kernel: there is no inline-ray-query NVVM/PTX intrinsic (verified — empty grep of
the fork's `IntrinsicsNVVM.td`). The only way to use the RT cores is **OptiX**,
which is a *pipeline* programming model, not an inline one:

- Programs: `__raygen__`, `__miss__`, `__closesthit__`, `__anyhit__`,
  `__intersection__`, compiled to PTX/OptiX-IR.
- An OptiX **module** + **program groups** + a linked **pipeline**.
- A **Shader Binding Table** (SBT) mapping geometry to hit programs.
- `optixAccelBuild` for the acceleration structure.
- `optixLaunch(pipeline, stream, params, sbt, w, h, d)`; `optixTrace` inside
  raygen invokes the RT cores; results return via payload registers and the
  `optixGet*` device functions inside anyhit/closesthit.

The environment is ready (see `[[optix-env]]`): SDK 9.1.0 installed, `nvoptix.dll`
present (driver 610.62), `optixInit()` verified on the 4090, and the SDK compiles
and links under cajeta's mingw toolchain (`-lcfgmgr32 -ladvapi32`).

## 2. The core problem (impedance mismatch)

cajeta `RayQuery` is **inline enumeration** inside an ordinary `@Kernel`:

```
RayQuery rq;
rq.initialize(scene, flags, mask, ox,oy,oz, tmin, dx,dy,dz, tmax);
uint32 c = 0;
while (rq.proceed()) {
    if (rq.candidateType() == 1) { c = c + 1; }   // AABB candidate
}
out[i] = c;
```

OptiX has **no inline ray query**. The equivalent must be expressed as a
pipeline: the kernel body becomes a `__raygen__` program that calls `optixTrace`
once, and the per-candidate logic (the loop body) becomes an `__anyhit__`
(invoked per intersection — accumulate, then `optixIgnoreIntersection()` to keep
traversing) or `__closesthit__` (the single committed/nearest hit). Mapping the
inline enumeration onto callback programs is a **compiler transformation**, and
the acceleration structure must be an **OptiX AS**, not cajeta's portable blob.

## 3. Goal

Add an **OptiX-backed native RT-core impl** of `RayQuery`/`AccelerationStructure`
on the NVIDIA CUDA path, selectable alongside the software BVH, with results
matching the software oracle on the RTX 4090 — establishing a third native tier
(after Vulkan native and AMD's pending native) so all GPU vendors can reach their
RT hardware.

## 4. Requirements

- **R1 — Runtime stack.** cajeta's C runtime can `optixInit`, create an
  `OptixDeviceContext` over the existing CUDA context (`[[nvidia-on-device-validation]]`
  already dlopens `nvcuda.dll`), build an OptiX AS (`optixAccelBuild`) over AABB
  and triangle geometry, build a pipeline + SBT, and `optixLaunch`.
- **R2 — AS noun impl.** A new `CajetaAsImpl` tier (e.g. `CAJ_AS_IMPL_OPTIX`) the
  CUDA noun provider builds; recorded on the `AccelerationStructure` noun and read
  back via `implTag()`, exactly as the Vulkan native impl is.
- **R3 — Verb lowering.** The cajeta NVPTX codegen emits OptiX-compatible device
  code for a `RayQuery`-using kernel: a raygen entry (`Thread.globalIdX()` →
  `optixGetLaunchIndex`), the `optixTrace` call (the `_optix_trace_*` ABI), and the
  minimal program set, with the proceed-loop's candidate body restructured into
  the anyhit/closesthit program.
- **R4 — Canonical shapes (phased).** Support, in order: (a) **count AABB
  candidates** within range (the Toffee spatial-index / RTNN pattern — anyhit
  accumulate), (b) **nearest hit** (closesthit: committed type / T / primitive
  index), (c) **candidate getters** (barycentrics, front-face). General arbitrary
  proceed-loop bodies are explicitly out of v1.
- **R5 — Selectable + degradable.** `AsImpl.Native`/`AsImpl.Software` and
  `CAJETA_GPU_AS_IMPL` select the impl; AUTO on CUDA prefers OptiX when available,
  else the software BVH floor. A `RayQueryNative`-style capability reports OptiX
  availability on the CUDA backend.
- **R6 — On-device parity.** Device tests force the OptiX path on the 4090 and
  assert parity with the software oracle (the same `kRqMinDriver`/`kTriDriver`/…
  scenes, 777), and a `implTag()` probe proves the OptiX impl actually ran.
- **R7 — Build integration.** cajeta's CMake links the OptiX runtime under mingw
  (header-only SDK + `-lcfgmgr32`), gated so a box without the SDK still builds
  (the software floor stays the default).

## 5. Use Cases

- Toffee `SpatialIndex` neighbour queries running on the 4090's RT cores via OptiX
  (today they run the software walk on CUDA).
- A vendor-complete RT story: Vulkan native (done), AMD native (pending hardware),
  NVIDIA-CUDA native via OptiX (this spec).

## 6. Non-Goals

- **General inline-RayQuery → OptiX transformation** for arbitrary proceed-loop
  bodies (CPS-style restructuring of unknown control flow). v1 covers the
  canonical shapes only (R4).
- **OptiX-IR** input — v1 targets PTX input to the OptiX module compiler (simpler;
  OptiX-IR is an optimization).
- **Motion blur, instancing/multi-level AS, SER, curves/spheres** — flat single
  BLAS over AABB + triangle geometry only.
- **AMD HIP native** — separate effort (`image_bvh_intersect_ray` + HIPRT), and
  unvalidatable on this box (no AMD GPU).
- **Replacing the software floor** — OptiX is an added tier, never the only path.

## 7. Acceptance Criteria

1. `optixInit` + device-context + AS build + pipeline + `optixLaunch` work from
   cajeta's runtime on the 4090 (proven by Milestone 0's standalone harness).
2. The OptiX AS impl is selectable and recorded (`implTag()` reports it).
3. Counting (R4a), nearest-hit (R4b), and candidate-getter (R4c) device tests on
   the OptiX path match the software oracle on the 4090.
4. AUTO on CUDA prefers OptiX when the SDK/runtime are present; software floor
   otherwise; no regression to the existing CUDA software-BVH ray-query tests.
5. The build links OptiX under mingw and stays buildable without the SDK.
6. Docs (matrix + RayQuery.md) and memory updated; spec/plan under
   `docs/specification/gpu-rayquery-optix/`.
