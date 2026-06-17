# Cajeta XPU — the launch & kernel-arg FFI contract

This is the **frozen C ABI** that external code — the numerics stack, the
PyTorch/Keras ports, the Toffee/SPELA trainer — registers kernels and dispatches
compute through. It is the Stage-12 deliverable of
[`../../../plans/gpu/xpu/cajeta-xpu-plan.md`](../../../plans/gpu/xpu/cajeta-xpu-plan.md).
The single source of truth for every declaration here is the header
[`runtime/native/cajeta_xpu_abi.h`](../../../runtime/native/cajeta_xpu_abi.h);
this document is the prose contract around it.

> **Scope.** What is frozen here is the **launch + kernel-arg marshalling**
> contract and **per-launch device targeting**. Device *enumeration/selection*
> and the alloc/buffer/texture FFI are `cajeta-gpu` concerns (their `__cajeta_xpu_*`
> lifecycle entry points are listed under *Companion surface* below but specified
> there).

---

## 1. Versioning

```c
#define CAJETA_XPU_ABI_VERSION 2
int32_t __cajeta_xpu_abi_version(void);   // the value compiled into the runtime
```

- An external caller checks `__cajeta_xpu_abi_version()` against the
  `CAJETA_XPU_ABI_VERSION` it compiled with **before** dispatching, to detect a
  runtime built against a different contract.
- The version is bumped when the surface grows in a way a downstream may want to
  require (e.g. `>= 2` to use the spec-constant override of `__cajeta_xpu_launch_v3`).
- **Two append-only rules keep older symbols stable as the surface grows:**
  1. A launch needing a new field is added as a **new symbol suffix**
     (`__cajeta_xpu_launch_v4`, …) — never by repurposing an existing argument.
     Old symbols keep their exact signature and behavior (`_v2`/`_v1` are now thin
     shims over `_v3`).
  2. The parameter-kind enum (`CajetaXpuParamKind`) is **append-only**: new kinds
     are added at the end; existing wire values are never renumbered.

**Version history:** v1 — register trio + `_v2` launcher (deviceId). v2 —
`__cajeta_xpu_launch_v3` adds host **specialization-constant override**
(`specCount`/`specValues`).

---

## 2. The register + launch trio

These three entry points are the entire surface needed to drive a kernel:

```c
// (a) Register the compiled image (SPIR-V / cubin / hsaco) under its entry name.
void __cajeta_xpu_register_module(const char* kernelName,
                                  const void* image, uint64_t len);

// (b) Register the kernel's per-parameter launch metadata (declaration order).
void __cajeta_xpu_register_kernel_params(const char* name, int32_t count,
                                         const uint8_t* kind,        // CajetaXpuParamKind[count]
                                         const uint32_t* byteSize);  // [count]

// (c) Launch it.
void __cajeta_xpu_launch_v3(const char* kernelName,
                            int32_t gridX, int32_t gridY, int32_t gridZ,
                            int32_t blockX, int32_t blockY, int32_t blockZ,
                            uint32_t sharedBytes, void* argv,
                            int64_t streamHandle, int32_t deviceId,
                            int32_t specCount, const int32_t* specValues);
```

**`specCount` / `specValues` — host specialization-constant override.** A kernel's
`Spec.geti(slot, default)` / `Spec.getf(slot, default)` reads user spec slot `slot`;
the launch overrides it by supplying `specValues[i]` for slot `i` (`specCount` =
how many leading slots are given; trailing/unset slots keep their compile-time
default). Each value is a **raw 4-byte word** — `Spec.geti` reads it as i32,
`Spec.getf` reinterprets it as f32 (the Cajeta frontend packs a float `spec:`
element as its bit pattern; mixed `spec:[3, 1.5f]` packs each slot by its own
type). `specCount == 0` / `NULL` = no override (every slot reads its default —
identical to `_v2`). Honored as a genuine pipeline-time `OpSpecConstant` on Vulkan,
a runtime read on CPU (identical observed results), and a read from per-launch
constant-memory globals on CUDA/HIP/NVPTX/AMDGPU (set via `cu/hipModuleGetGlobal` +
a host copy; safe-by-default — zero-init reads the compile-time default). The
device-backend path is **emit-verified**; on-device execution is pending hardware
(AMD comgr / NVIDIA HW). The Cajeta launch surface is
`kernel.launch(s, grid:[…], block:[…], spec:[v0,v1,…])(args)`.

`_v2` is a shim = `_v3(…, specCount=0, specValues=NULL)`:

```c
void __cajeta_xpu_launch_v2(const char* kernelName,
                            int32_t gridX, int32_t gridY, int32_t gridZ,
                            int32_t blockX, int32_t blockY, int32_t blockZ,
                            uint32_t sharedBytes, void* argv,
                            int64_t streamHandle, int32_t deviceId);
```

The `kind`/`byteSize` arrays passed to (b) must remain valid for the process
lifetime (the runtime stores the pointers, not copies — matching how the compiler
emits them as module constants).

A compat shim is retained for the compiler's own emit path (it has no
device-targeting language surface yet); it is exactly `_v2` with `deviceId = -1`:

```c
void __cajeta_xpu_launch(const char* kernelName,
                         int32_t gridX, int32_t gridY, int32_t gridZ,
                         int32_t blockX, int32_t blockY, int32_t blockZ,
                         uint32_t sharedBytes, void* argv, int64_t streamHandle);
```

---

## 3. The `argv` marshalling contract

`argv` points to an array of **one pointer per kernel parameter, in declaration
order**. What each slot points at is determined by that parameter's
`CajetaXpuParamKind` (the i-th entry of the `kind` array registered in (b)):

| Kind (`CajetaXpuParamKind`) | value | `argv[i]` points at | `byteSize[i]` |
|---|---|---|---|
| `CAJETA_XPU_KP_SCALAR`       | 0 | the by-value scalar/POD, packed declaration-order, vtable-stripped | the packed footprint in bytes |
| `CAJETA_XPU_KP_BUFFER`       | 1 | an `int64` device handle | 0 |
| `CAJETA_XPU_KP_TEXTURE`      | 2 | an `int64` texture handle (1D/2D/3D/Cube/2DArray) | 0 |
| `CAJETA_XPU_KP_SAMPLER`      | 3 | a POD `{int32 filterMode, int32 addressMode}` | 8 |
| `CAJETA_XPU_KP_ACCEL`        | 4 | a POD `{int64 handle, uint32 count, int32 impl}` | its footprint |
| `CAJETA_XPU_KP_IMAGE`        | 5 | an `int64` storage-image handle (writable `Image2D`) | 0 |
| `CAJETA_XPU_KP_BUFFER_ARRAY` | 6 | a slot laid out `[int64 count, int64 h0, … h(count-1)]` (bindless) | 0 |

- **`byteSize` must be computed against the host `DataLayout`** so the packed
  footprint matches what the launch site writes and the runtime copies into a
  transient SSBO (Vulkan) — a mismatch reads past the buffer.
- **Lifetime:** every slot and the value it points at are **caller-owned** and
  must outlive the launch call. The runtime copies what it needs during the call
  and retains nothing from `argv` afterward.
- **Backend translation:** on Vulkan the runtime turns scalar/POD slots into
  transient single-element SSBOs (driven by this metadata); buffers/textures/
  images bind their existing descriptors; the bindless array binds a descriptor
  array of `count`. CUDA/HIP pass the slots in the `cuLaunchKernel`/
  `hipModuleLaunchKernel` argument convention. CPU reads the aggregate directly.

---

## 4. Streams & device targeting

- **`streamHandle`** — `0` is the default stream. A non-zero handle (from
  `__cajeta_xpu_stream_create`) orders the launch with the async copies queued on
  that stream. Honored on CUDA/HIP; accepted-but-serialized on Vulkan/CPU
  (concurrency is Stage 10, not this contract).
- **`deviceId`** — selects the target device:
  - `-1` — the current active device (no targeting; the pre-Stage-12 behavior).
  - `0` — the default device on **every** backend (equivalent to `-1` for a
    single-device process).
  - `>= 0` — an index into the active backend's enumerated devices. The index
    space comes from `cajeta-gpu` device enumeration; xpu consumes the index.
  - **Out of range** (`deviceId >= deviceCount`) is a **defined no-op**: a
    diagnostic on `stderr`, no launch, never UB.
- **v1 selection — "where it is cheap + correct":** HIP genuinely selects
  `deviceId > 0` via `hipSetDevice` (then restores the default). Multi-device
  `> 0` on CUDA/Vulkan needs per-device contexts not yet built and is a defined
  *"not yet implemented"* (a diagnostic + no-op), **not** a silent wrong-device
  launch.
- **GpuBuffer-affinity contract (caller's responsibility):** every buffer/texture/
  image handle passed to a launch must already reside on the target device.
  Cross-device residency/migration is a `cajeta-gpu` concern; this layer does
  **not** silently migrate.

---

## 5. Stability tiers

| Tier | Symbols | Guarantee |
|---|---|---|
| **Frozen FFI** (this doc) | `__cajeta_xpu_abi_version`, `__cajeta_xpu_register_module`, `__cajeta_xpu_register_kernel_params`, `__cajeta_xpu_launch_v3`, `__cajeta_xpu_launch_v2`, `__cajeta_xpu_launch`, `CajetaXpuParamKind`, `CAJETA_XPU_ABI_VERSION` | Stable under the §1 append-only rules. Downstream may depend on these directly. |
| **Companion surface** (`cajeta-gpu`) | `__cajeta_xpu_buffer_*`, `__cajeta_xpu_texture*`, `__cajeta_xpu_image_*`, `__cajeta_xpu_accel_*`, `__cajeta_xpu_stream_*` / `_event_*` / `_fence_*`, `__cajeta_xpu_device_supports` | Stable, but specified by `cajeta-gpu` (alloc/lifecycle/enumeration), not here. |
| **Internal** | `__cajeta_xpu_register_backend`, `__cajeta_xpu_register_cpu_kernel`, in-kernel coordinate/wave/atomic intrinsic thunks, everything else | Implementation detail. No stability guarantee; do not call from external code. |

---

## 6. Worked example (C)

```c
#include "cajeta_xpu_abi.h"

assert(__cajeta_xpu_abi_version() == CAJETA_XPU_ABI_VERSION);

// saxpy(GpuBuffer<float> y, GpuBuffer<float> x, float a, uint32 n)
__cajeta_xpu_register_module("saxpy", image, image_len);
const uint8_t  kinds[4] = { CAJETA_XPU_KP_BUFFER, CAJETA_XPU_KP_BUFFER,
                            CAJETA_XPU_KP_SCALAR, CAJETA_XPU_KP_SCALAR };
const uint32_t sizes[4] = { 0, 0, sizeof(float), sizeof(uint32_t) };
__cajeta_xpu_register_kernel_params("saxpy", 4, kinds, sizes);

int64_t yh = /* buffer handle on the target device */;
int64_t xh = /* … */;
float a = 2.0f; uint32_t n = 1024;
void* argv[4] = { &yh, &xh, &a, &n };

__cajeta_xpu_launch_v2("saxpy",
                       /*grid=*/(n + 63) / 64, 1, 1, /*block=*/64, 1, 1,
                       /*sharedBytes=*/0, argv,
                       /*streamHandle=*/0, /*deviceId=*/-1);
```

The compiler-emitted launch path is itself an in-tree consumer of exactly these
symbols (it emits `__cajeta_xpu_register_module` + `__cajeta_xpu_register_kernel_params`
+ the `__cajeta_xpu_launch` shim), so the entire `test/xpu/*` suite exercises this
contract end to end on CPU + on-device AMD/Vulkan.
