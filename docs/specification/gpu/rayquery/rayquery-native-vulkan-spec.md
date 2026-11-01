# Native RT-Core Ray Query (Vulkan) — On-Device Spec

## 1. Background

cajeta's ray-query noun (`runtime/src/cajeta/gpu/core/RayQuery.cajeta`,
`AccelerationStructure.cajeta`) has two implementations behind one surface:

- **Software BVH.** `SoftwareRayQuery` walks a portable BVH blob built by the
  shared CPU builder (`runtime/native/cajeta_bvh.c`) and uploaded into a storage
  buffer. This is the floor, and it runs on every backend (CPU, NVPTX, AMDGPU,
  Vulkan).
- **Native.** `VK_KHR_acceleration_structure` BLAS+TLAS is built on the device
  (`cajeta_xpu_vk_accel_build_aabbs` / `_triangles`) and traced from the kernel via
  SPIR-V `OpRayQuery*` (`SpirvKernelLowering.cpp`). On an RTX 4090 this path runs
  on the RT cores.

The kernel-side `OpRayQuery` lowering is complete (initialize / proceed /
get-intersection-type / primitive-index / T / barycentrics / front-face /
confirm / generate). The native BLAS+TLAS builders are complete, and the triangle
path's TLAS wrapping was fixed earlier this arc. However, the native path has
never been validated on-device on any platform, and on Windows it is gated
off entirely:

```c
// runtime/native/cajeta_runtime.c
static int caj_native_rayquery_available(void) {
#if defined(CAJETA_RT_HAS_VULKAN) && !defined(_WIN32)   // <- Win32 gate
    return (cajeta_xpu_active_backend() == CAJ_XPU_VULKAN && g_xpu_vk.rayQuery) ? 1 : 0;
#else
    return 0;
#endif
}
```

This is inconsistent with the capability query, which was already un-gated
on Windows this arc:

```c
// __cajeta_xpu_device_supports(RayQueryNative) — line ~10869
return (be == CAJ_XPU_VULKAN && g_xpu_vk.rayQuery) ? 1 : 0;   // no Win32 gate
```

So today the 4090 reports `RayQueryNative == true` but every AS build silently
resolves to software BVH. The device already enables
`VK_KHR_ray_query` + `VK_KHR_acceleration_structure` +
`deferred_host_operations` + `buffer_device_address` and detects
`g_xpu_vk.rayQuery` on Windows. The plumbing is in place, and only the resolution
gate and on-device proof are missing.

## 2. Goal

The goal is to enable and validate the native hardware ray-query path (RT cores)
on the RTX 4090's Vulkan backend on Windows, with results matching the
software-BVH oracle. Native becomes the AUTO choice on a ray-query-capable
Vulkan device, and the software floor stays selectable and intact.

## 3. Requirements

- **R1 — Consistent availability.** `caj_native_rayquery_available()` reflects
  real device capability on Windows (remove the `!defined(_WIN32)` gate),
  matching `__cajeta_xpu_device_supports(RayQueryNative)`. The two must never
  disagree on the same device.
- **R2 — AABB native trace.** AABB ray query through native BLAS+TLAS +
  `OpRayQuery` returns hits/misses, primitive indices, and T values matching the
  software-BVH oracle on the 4090's Vulkan backend.
- **R3 — Triangle native trace.** Triangle ray query through native BLAS+TLAS
  returns hit/primitive-index/T/barycentrics matching the software oracle.
- **R4 — Selectable impl.** `CAJETA_GPU_AS_IMPL=software` forces software, and
  `=native` forces native (or the software floor if unsupported). AUTO resolves to
  native on a ray-query-capable Vulkan device and software elsewhere. The
  recorded impl on the noun always equals the impl actually used.
- **R5 — Device tests.** Tests force the native Vulkan path and assert parity
  with the software oracle. They SKIP cleanly rather than fail when the active
  Vulkan device lacks ray query.
- **R6 — Docs.** The capability matrix and ray-query doc reflect native ray
  query as on-device-validated on Windows Vulkan. The AS-impl-by-platform note
  is corrected (Windows Vulkan AUTO → native, no longer "software on Windows").

## 4. Use Cases

- Caramelo spatial-index queries (`CarameloSpatialIndexDeviceTests`) running on HW
  RT cores instead of the software walk.
- A future perf comparison (native RT cores vs software BVH) on identical inputs.
  Measuring it is out of scope here, but the parity tests establish the inputs.

## 5. Non-Goals

- **CUDA/NVPTX native ray query.** NVIDIA RT cores are reached from CUDA via
  OptiX rather than the NVPTX device path, so CUDA's AS stays software BVH. This
  is unchanged.
- **AMD native ray query.** AMD stays software BVH (parity already landed).
- **OptiX integration** of any kind.
- **Performance tuning / measurement.** Correctness parity only.

## 6. Acceptance Criteria

1. `caj_native_rayquery_available()` and `__cajeta_xpu_device_supports`
   agree on the 4090 (both report native available on Vulkan).
2. New native-Vulkan ray-query device tests (AABB + triangle) pass on the 4090
   with results matching the software oracle, and SKIP cleanly on a non-RT device.
3. Forced-software and forced-native are both selectable and correct, and AUTO picks
   native on the 4090's Vulkan.
4. No regression: the existing software-BVH ray-query suite (CPU / NVPTX / AMD /
   Vulkan-forced-software) stays green.
5. Full Vulkan device suite stays green on the 4090.
6. Docs/matrix updated per R6.
