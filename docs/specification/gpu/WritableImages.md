# Writable images: `Image2D.store`

`Image2D` is the writable twin of `Texture2D`. It is the compute-side surface
for a kernel that produces an image rather than sampling one. `Texture2D` is a
read-only `SAMPLED_IMAGE` sampled through a `Sampler` with hardware filtering.
`Image2D` is a writable `STORAGE_IMAGE` with no sampler and integer `(x, y)`
texel coordinates, and a kernel may store any texel.

```
Image2D img = heap Image2D(w, h);
fill.launch(s, ...)(img);          // a kernel writes img.store(x, y, value)
s.sync();
float32[] out = heap float32[w * h];
img.download(out);                 // read the produced texels back to the host
```

```
// inside the kernel
img.store(x, y, value);            // x, y are INTEGER texel coords; value is f32
float32 v = img.load(x, y);        // read a texel back (the read twin of store)
```

`store` and `load` together make an `Image2D` read-modify-write. A kernel can
read a texel an earlier dispatch wrote, transform it, and write it back, which
is the basis of image-to-image compute passes. The runtime inserts a
read-after-write barrier between dispatches on the same storage image.

## What it's for

`Image2D` is the gfx bridge. It is the capability that turns a compute kernel
into an image producer:

- **Procedural fills / generators** such as noise, gradients, and
  signed-distance fields.
- **Image-to-image passes** such as a compute blur/resize/tonemap that writes
  its result image. Read the input as a `Texture2D` and write the output as an
  `Image2D`.
- **Rasterization / splatting** that scatters points or primitives into a
  target.

It is the writable half of the texture surface. Pair `Texture2D.sample` (read,
normalized coords, filtered) with `Image2D.store` (write, integer coords,
exact).

## How it lowers

| Backend | Lowering |
|---|---|
| **Vulkan** (RADV, etc.) | a `STORAGE_IMAGE` descriptor bound in `VK_IMAGE_LAYOUT_GENERAL`; `store` is a single **`OpImageWrite`** and `load` a single **`OpImageRead`** (the fork `llvm.spv.resource.store.2d` / `load.2d` intrinsics). The image declares the **R32f** format (matching the runtime `VK_FORMAT_R32_SFLOAT`). |
| **AMD / NVIDIA / CPU** | *not in v1* — `store` / `load` reject at lowering (XPU-N01). Like `Texture2D`, writable images are a Vulkan-first capability. |

The host side allocates the image with `VK_IMAGE_USAGE_STORAGE_BIT |
TRANSFER_SRC` and transitions it to `GENERAL` before the dispatch with a layout
barrier recorded into the launch command buffer. Then `download(...)` reads it
back with `vkCmdCopyImageToBuffer` through a host-visible staging buffer.

### The R32f format (and why it is not `Unknown`)

A storage image with an `Unknown` format requires the SPIR-V capability
`StorageImageWriteWithoutFormat`, which the SPIR-V backend only makes available
in the Shader environment for SPIR-V ≥ 1.6. Cajeta's device triple
(`spirv-unknown-vulkan1.3-compute`) does not pin SPIR-V 1.6, so an `Unknown`
storage write is unsatisfiable, and `llc` aborts with "Unable to meet SPIR-V
requirements". Declaring the known R32f format, which is what the runtime image
actually is, sidesteps the capability entirely. It needs only the
always-present `Shader` capability, and the format matches the image view, so
the write is a plain format-typed store.

The fork addition is small and mirrors `store.typedbuffer`. It is a single new
intrinsic, `llvm.spv.resource.store.2d`, which takes a 2-component
integer-vector coord instead of the buffer's scalar index and is routed to the
existing `selectImageWriteIntrinsic`. That function is coordinate-agnostic and
passes the coordinate straight to `OpImageWrite`. The change is carried on
`cajeta-spirv`, since there is no in-tree producer for a 2-D image store and so
no upstream PR.

## Caveats

- **Single-channel `float32` (R32f), 2-D, v1.** `OpImageWrite` always takes a
  4-component texel, so `store` splats `value` into `<value, 0, 0, 0>` and `load`
  reads a `<4 x f32>` and extracts lane 0 (the R32f channel). Generic
  `Image2D<T>`, multi-channel formats, and 3-D are follow-ups.
- **Vulkan-only.** Like `Texture2D`, `Image2D` is not in the portable Tour,
  which must run on CPU. It is exercised by the device test on a real Vulkan
  device.
- **Texels start undefined.** There is no `upload`, because the kernel produces
  the contents. `download` reads whatever the last dispatch wrote.

---

**Rules.** `Image2D.store(uint32 x, uint32 y, float32 value)` and
`Image2D.load(uint32 x, uint32 y) -> float32` are device-only, Vulkan-only
storage-image write / read (a single `OpImageWrite` / `OpImageRead` via the
`cajeta-spirv` `llvm.spv.resource.store.2d` / `load.2d` intrinsics, R32f format,
`GENERAL` layout; a read-after-write barrier separates dispatches). Allocate with
`heap Image2D(w, h)` and read back on the host with `img.download(out)`. The
surface is device-verified bit-exact on RADV
(`XpuVulkanDispatchDeviceTests.{imageStoreOnDevice,imageLoadStoreRmwOnDevice}`)
and spirv-val-clean (`XpuVulkanEmitTests.lowersImage{Store,Load}ToSpirv`). See
`Texture2D` (`runtime/.../core/Texture2D.cajeta`) for the sampled read surface.
