# Cajeta GPU — the shared device foundation

`cajeta-gpu` is the **shared GPU foundation** that both compute and graphics build
on. Anything that is *not* specific to the kernel/compute execution model and *not*
specific to the graphics pipeline lives here: the codegen/device/driver plumbing, the
memory & buffer model, the value types, the math intrinsics, and textures/images.

```
cajeta-gpu        (this layer — the foundation)
   ▲                       ▲
   │ depends on            │ depends on
cajeta-xpu             cajeta-gfx
(compute:              (graphics:
 science/ML/stats)      pipeline + engine)
```

**Why a separate layer.** The value types (`Vector`/`Matrix`/`Quaternion`), the math
intrinsics, the SPIR-V/cubin/hsaco emit machinery, the device/driver layer, the
memory/buffer model, and textures are needed *identically* by compute kernels and by
graphics shaders. Factoring them out keeps the compute layer
([`xpu/CajetaXPU.md`](xpu/CajetaXPU.md)) and the graphics layer
([`gfx/CajetaGFX.md`](gfx/CajetaGFX.md)) from forking the substrate. `cajeta-xpu` and
`cajeta-gfx` are **siblings** over this base — gfx does *not* depend on xpu.

> **One axis, one model.** Allocation + borrow follow a single rule: storage class is
> the axis (stack = copy / heap = ref). The foundation does not add GPU-special
> allocation or borrow paths — device buffers (`Buffer<T>`) and images are ordinary
> RAII values whose lifetime is the scope-exit drop chain.

This doc is the **map** of the foundation. The authoritative forward plan is
[`plans/gpu/cajeta-gpu-plan.md`](../../plans/gpu/cajeta-gpu-plan.md); the per-backend,
per-feature capability matrix is [`xpu/CajetaXPU-Matrix.md`](xpu/CajetaXPU-Matrix.md).

---

## What lives in the foundation

### 1. Value types (host + device, identical lowering)

First-class, by-value POD types that lower to LLVM vectors and run **identically** on
the host JIT and every device backend. The canonical signature reference is
[`ValueTypeCatalog.md`](ValueTypeCatalog.md).

| Type | Lowers to | Docs |
|------|-----------|------|
| `Vector<T,N>` | `<N x T>` | [`ValueTypeCatalog.md`](ValueTypeCatalog.md) |
| `Matrix<T,R,C>` | `<R*C x T>` | [`ValueTypeCatalog.md`](ValueTypeCatalog.md), [`MatrixDeterminantInverse.md`](MatrixDeterminantInverse.md) |
| `Quaternion<T>` | `<4 x T>` (w,x,y,z) | [`Quaternions.md`](Quaternions.md) |

Operator rule worth knowing up front: **comparisons on value types yield per-lane
masks** (`<N x i1>`), not a reduced boolean — reduce with `.all()`/`.any()`, blend with
`.select(a,b)`. The branchless mask/select primitive is documented in
[`MaskSelect.md`](MaskSelect.md).

Covered surface: construction, component/swizzle reads (`.xyz`/`.rgba`/`.xxyy`),
element-wise arithmetic + scalar broadcast, `dot`/`length`/`normalize`,
`cross`/`reflect`/`refract`/`distance`/`clamp`/`lerp`/`min`/`max`; matrix
`matmul`/transpose/identity/`determinant`/`inverse` (square 2–4, float); quaternion
Hamilton product, vector rotation, `nlerp`/`slerp`.

### 2. Math & numeric intrinsics

- **Integer dot product** — `Vector<int8,4>.dot` (DP4a): the Vulkan hardware unit
  (`SPV_KHR_integer_dot_product`, `OpSDot`/`OpUDot`) with a bit-exact portable
  fallback elsewhere. See [`IntegerDotProduct.md`](IntegerDotProduct.md).
- **Bit instructions** — `Bits.*` (reverse / popcount / rotate): per-invocation scalar
  bit ops with no wave/execution-model dependence. See
  [`BitInstructions.md`](BitInstructions.md).
- **Device transcendentals** — the `TrigEmitter`/`LoweringTarget::transcendental` seam
  (host emits `llvm.acos`/`sin`/…; device routes per backend), used by e.g. quaternion
  `slerp`.
- **bf16** element type + arithmetic landed (Shader flavor); **fp8** deferred pending
  upstream LLVM backend support.

### 3. Textures & images

- `Texture2D<T = float32>` + `Sampler` — 2-D, normalized coords, nearest/bilinear,
  clamp/wrap; the `LoweringTarget::sampleTexture` seam lowers `tex.sample(s, u, v)` per
  backend (CPU C bilinear, VK `OpImageSampleExplicitLod`, AMD `__ockl_image_sample_2D`,
  NV `tex.2d`). The type parameter `T` is the texel scalar (defaults to `float32`, so
  every bare `Texture2D` is `Texture2D<float32>`); `sample` returns `Vector<T, 4>`.
  Multi-channel formats via `TextureFormat` (R32F/R8_UNORM/RGBA8_UNORM/RGBA32F/R16F/
  RGBA16F). **`sample` is float-only** — integer textures (below) can't be filtered.
- **texelFetch** — `tex.fetch(x, y)` reads the texel at the **exact integer** coordinate,
  unfiltered and with **no Sampler** (LOD 0); the unfiltered twin of `sample`, for data
  textures / lookup tables. The `LoweringTarget::fetchTexture` seam lowers per backend:
  VK `OpImageFetch` (via the `llvm.spv.resource.load.level` intrinsic — the sampled-image
  `Sampled=1` branch picks Fetch, distinct from `Image2D`'s storage `OpImageRead`; the
  mandatory `Lod` operand is supplied), AMD `__ockl_image_load_2D`, CPU exact texel read.
  Returns `Vector<T, 4>`; CPU/Vulkan/AMD on-device. (NV emit-deferred.)
- **Integer textures** — `Texture2D<int32>` / `Texture2D<uint32>` with a raw-integer
  `TextureFormat` (`R32I`/`R32UI`/`RGBA32I`/`RGBA32UI`) store exact integers (no
  normalization). They are **fetch-only** — `fetch` returns `Vector<int32|uint32, 4>`
  verbatim; `sample` is a compile error (the hardware sampler can't filter integer
  texels). Same `fetchTexture` seam, threading the texel type `T`: VK emits an integer
  `OpImageFetch` (i32-sampled image), CPU reads raw i32, AMD reuses the only ockl 2-D
  image load (v4f32) and **bitcasts** its raw result to `<4 x i32>` (the HW `image_load`
  is raw on a non-normalized integer SRD). CPU/Vulkan/AMD on-device, bit-exact.
- **Writable / storage images** — `Image2D` `imageStore`/`imageRead`, the writable twin
  of `Texture2D` and the bridge toward graphics. See
  [`WritableImages.md`](WritableImages.md). (Storage-image read/write needs a *known*
  format, e.g. `R32f`, on the `vulkan1.3-compute` triple.)

### 4. Device, codegen & memory (the plumbing)

These have no standalone doc yet — they are specified in the plan
([`plans/gpu/cajeta-gpu-plan.md`](../../plans/gpu/cajeta-gpu-plan.md) Part A) and the
capability matrix ([`xpu/CajetaXPU-Matrix.md`](xpu/CajetaXPU-Matrix.md) §1–§5):

- **Codegen pipeline** — per-backend device triple + `*Backend`
  (`nvptx64-nvidia-cuda` → cubin via `ptxas`; `amdgcn-amd-amdhsa` → hsaco via `ld.lld`;
  `spirv64-unknown-vulkan1.3-compute` → SPIR-V binary; CPU → LLJIT), entry-point
  decoration, and in-process module registration.
- **Device & driver layer** — driver acquisition via `dlopen`
  (`libcuda`/`libamdhip64`/`libvulkan`), backend detection + selection order
  `CUDA → HIP → Vulkan → CPU`, per-backend module load.
- **Memory & buffer model** — `Buffer<T>` RAII (alloc/upload/download), the
  address-space model (Global/Shared/Constant/Private/Generic mapped per backend), and
  the Vulkan descriptor-set SSBO model (BDA has no IR path — a load-bearing finding).
- **Multi-arch bundling** — AMD fatbin via `clang-offload-bundler` (`--xpu-arch=…`);
  NV fatbin pending hardware.

### 5. Part C — cutting-edge SPIR-V (foundation-level)

Delivered, on-device: **ray query** + acceleration structures (compute-callable; also
used by gfx inline RT) and **cooperative matrix → Shader flavor** (tensor-core matmul).
These ride the downstream LLVM fork. Tracked in
[`plans/gpu/cajeta-gpu-plan.md`](../../plans/gpu/cajeta-gpu-plan.md) Part C.

---

## Backends & evidentiary weight

**NV** NVPTX→cubin · **AMD** AMDGPU→hsaco · **VK** SPIR-V · **CPU** LLJIT. "On-device"
means real hardware (AMD gfx1151 Strix Halo; VK via RADV). The NVIDIA column is
**emit-only** today (no NV hardware on the dev box), pending the B5 WSL2+CUDA runner.
The full, honest per-cell status is the capability matrix
([`xpu/CajetaXPU-Matrix.md`](xpu/CajetaXPU-Matrix.md)); the cross-backend design
discipline is [`xpu/CajetaXPU-Variance.md`](xpu/CajetaXPU-Variance.md).

---

## Status & definition of done

- **B1** (linear-algebra value types) and **B2** (device math intrinsics) — **landed**.
- **B3** (textures/images) — **partial** (writable + readable storage images landed).
- **B4/B5** — remaining (texture/memory completeness; NV on-device runner).
- **Part C** ray query + cooperative matrix — **delivered, on-device**.

The foundation's definition of done is a **frozen value-type / math / texture / memory
dependency contract** that `cajeta-xpu` and `cajeta-gfx` both target. Until B3–B5 land,
that contract stays open. Full criteria: the plan's "Definition of done for the
foundation".

---

## See also

- Compute layer — [`xpu/CajetaXPU.md`](xpu/CajetaXPU.md) (spec),
  [`xpu/CajetaCPU.md`](xpu/CajetaCPU.md) (CPU backend),
  [`xpu/CajetaXPU-Matrix.md`](xpu/CajetaXPU-Matrix.md) (capability matrix).
- Graphics layer — [`gfx/CajetaGFX.md`](gfx/CajetaGFX.md),
  [`gfx/CajetaRender.md`](gfx/CajetaRender.md).
- Plans — [`plans/gpu/cajeta-gpu-plan.md`](../../plans/gpu/cajeta-gpu-plan.md),
  [`plans/gpu/xpu/cajeta-xpu-plan.md`](../../plans/gpu/xpu/cajeta-xpu-plan.md),
  [`plans/gpu/gfx/cajeta-gfx-plan.md`](../../plans/gpu/gfx/cajeta-gfx-plan.md).
