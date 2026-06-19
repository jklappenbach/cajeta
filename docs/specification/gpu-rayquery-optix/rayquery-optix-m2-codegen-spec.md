# OptiX RT-Core Ray Query — M2: NVPTX→OptiX Codegen Spec

## 1. Background

M0 proved the OptiX RT-core path reproduces cajeta's software ray-query oracle on
the 4090; M1 landed the runtime AS provider (`CAJ_AS_IMPL_OPTIX` — build + record an
OptiX AS, `docs/specification/gpu-rayquery-optix/rayquery-optix-cuda-{spec,plan}.md`). What's
missing is the **verb**: a cajeta `@Kernel` using `RayQuery` actually traversing the
OptiX AS on the RT cores. Today NVPTX RayQuery kernels lower to the `SoftwareRayQuery`
walk (`accelImpl() == SoftwareBvh`); M2 adds an OptiX lowering tier.

## 2. The two hard problems (and how grounded they are)

**(A) The `optixTrace` ABI from LLVM — DE-RISKED.** OptiX's `optixTrace` is not a
normal call; `optix_device_impl.h` implements it as **inline PTX asm**:

```
asm volatile("call (%0,..,%31), _optix_trace_typed_32, (%32,..,%80);"
             : "=r"(p[1]), .. 32 outputs ..
             : "r"(type),"l"(handle),"f"(ox),.., "r"(p[1]),.. 49 inputs ..);
```

The OptiX module compiler (in `nvoptix.dll`) recognizes the `_optix_trace_typed_32`
call site in the PTX and lowers it to RT-core traversal. LLVM IR supports inline asm
with the same constraint string (`call asm sideeffect`), and the module compiler
does not care whether nvcc or LLVM produced the PTX. So cajeta can emit the identical
inline-asm call. The getters are simpler single-result inline-asm calls
(`_optix_read_primitive_idx`, `_optix_get_triangle_barycentrics`, `_optix_get_ray_tmax`,
`_optix_get_attribute_*`, etc.). **Conclusion: the ABI is mechanically reproducible —
not a research risk.**

**(B) The inline-enumeration → callback restructuring — THE REAL WORK.** cajeta's
`RayQuery` is inline: `rq.initialize(...); while (rq.proceed()) { ...candidate... }`.
OptiX has no inline traversal — the per-candidate body must move into an `__anyhit__`
program and the per-committed-hit body into `__closesthit__`, with state carried in
payload registers. For arbitrary loop bodies this is a CPS-style transformation
(unbounded difficulty). v1 restricts to the **canonical shapes** whose body maps to a
fixed callback, recognized structurally.

## 3. Goal

A cajeta `@Kernel` using `RayQuery` against an OptiX-impl `AccelerationStructure`
runs on the 4090's RT cores via an OptiX pipeline, producing results identical to the
software oracle — for the canonical shapes — selected by the existing impl tier
(`AsImpl.Native`/`CAJETA_GPU_AS_IMPL=optix`), with AUTO on CUDA then able to default
to OptiX.

## 4. Requirements

- **R1 — OptiX device program emission.** For an OptiX-tier RayQuery kernel, the
  NVPTX backend emits an OptiX module's PTX: a `__raygen__<kernel>` entry (launch
  index ← `_optix_get_launch_index`), the `optixTrace` inline-asm call, and the
  minimal `__anyhit__`/`__closesthit__`/`__miss__`(/`__intersection__` for AABB)
  program set, plus the `params` launch-param `.const` global.
- **R2 — Shape lowering.** Recognize and lower the canonical shapes:
  (a) **count** AABB candidates in range (anyhit: payload++ then ignore);
  (b) **nearest hit** (closesthit: committed T / primitive index);
  (c) **candidate getters** (barycentrics, front-face).
  An unsupported RayQuery shape throws a clear diagnostic (XPU-N0x), never miscompiles.
- **R3 — Launch path.** Build the OptiX module + program groups + pipeline + SBT
  from the emitted PTX (extending the M1 glue), and `optixLaunch` in place of
  `cuLaunchKernel` for OptiX-impl RayQuery kernels. Kernel buffer args → launch
  params.
- **R4 — Context unification.** The OptiX AS, the pipeline, and the launch must use
  ONE CUDA context. M1's glue uses the primary context; the runtime's kernel path
  uses its own `cuCtxCreate` context. M2 reconciles these (the noted M1 follow-up):
  either the runtime adopts the primary context, or the glue adopts the runtime's.
- **R5 — Selection.** With the verb available, AUTO on CUDA may resolve to OptiX
  (flip `caj_cuda_resolve_as_impl`'s AUTO case); software stays the floor / forced
  fallback. A `Device.supports` capability reports OptiX RT availability (the M1-
  deferred item).
- **R6 — On-device parity.** Device tests run the canonical-shape RayQuery kernels
  through the cajeta compiler on the OptiX path; results match the software oracle
  (the `kRqMinDriver`/`kNearestDriver`/`kBaryDriver` scenes → 777).

## 5. Non-Goals

- General CPS restructuring of arbitrary `proceed()` loop bodies (only the canonical
  shapes; others diagnose).
- `confirm`/`generate` for non-canonical flows, multi-level AS / instancing, motion,
  SER, OptiX-IR input (PTX only), and any non-CUDA backend.
- Replacing the software floor or the Vulkan native path.

## 6. Acceptance Criteria

1. A minimal `__raygen__` + `optixTrace` PTX emitted from LLVM is accepted by
   `optixModuleCreate` + `optixPipelineCreate` on the 4090 (the Phase-1 spike).
2. The count / nearest / getter RayQuery kernels lower to OptiX programs and run on
   the RT cores, matching the software oracle (777) on the 4090.
3. One CUDA context across AS build + launch; no leaked/again-clobbered context.
4. AUTO-on-CUDA → OptiX selectable; forced-software still correct; no regression to
   the NVPTX software path, the Vulkan native path, or M0/M1 tests.
5. Unsupported RayQuery shapes throw a clear diagnostic.
6. Docs + memory updated; spec/plan under `docs/specification/gpu-rayquery-optix/`.
