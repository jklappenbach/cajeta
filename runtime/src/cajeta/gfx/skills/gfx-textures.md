---
id: gfx-textures
applies-to: [cajeta/gpu/Texture1D, cajeta/gpu/Texture2D, cajeta/gpu/Texture3D, cajeta/gpu/TextureCube, cajeta/gpu/Texture2DArray, cajeta/gpu/Image2D, cajeta/gpu/Sampler, cajeta/gpu/TextureFormat]
title: GPU texture & image resources (sampled Texture* vs writable Image2D, Sampler, TextureFormat)
description: Pick and use cajeta.xpu image resources — read-only sampled Texture{1D,2D,3D,Cube,2DArray} through a Sampler vs writable Image2D, with host create/upload/free and device-only sample/fetch/store.
---

# GPU textures & images

Device image resources for kernels. Two families:

- **`Texture{1D,2D,3D,Cube,2DArray}`** — **read-only** sampled images. A kernel reads
  them with `sample` (hardware-filtered, through a `Sampler`) or `fetch` (unfiltered,
  by exact integer texel coord). Use when you want filtering/interpolation or
  addressing-mode wrap/clamp that a plain `KernelBuffer` read can't give.
- **`Image2D`** — a **writable** storage image. A kernel **writes** texels with
  `store(x,y,v)` and reads them back with `load(x,y)`; the host gets the result via
  `download`. No sampler, integer coords only. Use for compute-produced images
  (procedural fills, image-to-image passes, the gfx bridge).

`Sampler` and `TextureFormat` are support types, not images you allocate as resources.

## Pick the right type

| Want | Use |
|------|-----|
| Filtered read of a 1-D LUT / gradient / transfer curve | `Texture1D` |
| Filtered read of a 2-D image | `Texture2D` |
| Trilinearly-filtered read of a volume / voxel field (blends across slices) | `Texture3D` |
| Stack of 2-D layers, indexed (no cross-layer filtering) | `Texture2DArray` |
| Environment map / skybox sampled by a direction vector | `TextureCube` |
| Kernel that **writes** an image to read back on the host | `Image2D` |
| Exact stored value, no interpolation, no sampler | `fetch` on any Texture* (not Cube) |
| Just a flat array on the device (no filtering/addressing) | `KernelBuffer` (different component) |

Negatives that save a dead end:
- `TextureCube` has **no `fetch`** — a direction has no single integer texel; reads are
  always the projected filtered `sample`.
- Texture* are **read-only** — there is no `store`/`upload`-from-kernel; to write from a
  kernel use `Image2D`.
- `sample` is **float-only**: integer textures (`T = int32`/`uint32` with the raw-integer
  formats) are **fetch-only** — the hardware sampler cannot filter integer texels.
- `sample`/`fetch`/`store`/`load` are **device-only intrinsics**: calling them on the
  host (outside an `@Kernel`/`@Device` function) is unsupported. Their cajeta bodies are
  resolution placeholders, never emitted as real calls — they lower at the call site to
  the backend image op.
- Mip levels exist on **`Texture2D` only** (4-arg ctor + `uploadLevel`/`fetchLod`/
  `sampleLod`); the other textures are single-level (LOD 0) in v1.
- `Image2D` is v1 **Vulkan-only**, single-channel `float32`; other backends reject
  `store` at lowering.

## Lifecycle & ownership (whole family)

Every image is a small **host-side handle** (`deviceHandle` + dims + format ordinal)
over a device resource, governed by the **drop chain** — the KernelBuffer RAII convention:
the RAII constructor (`heap Texture2D(w,h)`) **acquires** the device image; the
destructor **releases** it at scope exit (null-guarded, idempotent — a moved-from `#tex`
or prior free is a no-op). You do **not** call a free method.

A launch **borrows** each image argument until the next `GpuStream.sync()` — do not free
or let an image drop before you've synced the launch that uses it.

`upload`/`download`/`uploadLevel` take a host `T[]` (default `T = float32`); the array is
**borrowed** for the copy, not retained — you keep ownership and may free it after.

## Construction

Texture ctors (RAII forms), all defaulting to `TextureFormat.R32F` when format is omitted:

```
heap Texture1D(width)                                     // or (width, format)
heap Texture2D(width, height)                             // or (.., format) or (.., format, levels)
heap Texture3D(width, height, depth)                      // or (.., format)
heap Texture2DArray(width, height, layers)               // or (.., format)
heap TextureCube(size)                                    // size x size x 6, or (size, format)
heap Image2D(width, height)                               // float32, writable; no format arg
heap Sampler(filterMode, addressMode)                     // value, not a device resource
```

`format` is a **`TextureFormat` ordinal** (the enum constant resolves to its i32
ordinal) — pass `TextureFormat.RGBA8_UNORM` etc.

## TextureFormat — ordinal IS the contract

`R32F`(0) `R8_UNORM`(1) `RGBA8_UNORM`(2) `RGBA32F`(3) `R16F`(4) `RGBA16F`(5) and the
**fetch-only** integer formats `R32I`(6) `R32UI`(7) `RGBA32I`(8) `RGBA32UI`(9). Two axes:
channels (1=R, 4=RGBA) and encoding (32-bit float / 16-bit half / 8-bit UNORM /
raw int). `sample`/`fetch` always return `Vector<T,4>` regardless: missing channels
expand (G/B=0, A=1), UNORM bytes decode to `[0,1]`, half decodes to float, raw int reads
back the stored integer verbatim. **Ordinal values are the stable native contract** (the
`__cajeta_xpu_texture_*` intrinsics switch on them in `runtime/native/cajeta_runtime.c`);
do not reorder the enum without updating the C side.

## Sampler

`Sampler(int32 filterMode, int32 addressMode)` — `filterMode`: 0 = nearest, 1 = linear
(bilinear); `addressMode`: 0 = clamp-to-edge, 1 = repeat (wrap). It is a host-side value
carrying mode bits; the runtime builds the backend sampler object at launch. It is
admissible as a kernel arg **by name** (its own descriptor on Vulkan), not as a POD
struct. Only `sample` uses it — `fetch` has no sampler and no addressing wrap/clamp, so
fetch coords must be in bounds.

## Reads (device-only, inside a kernel)

`sample` returns the filtered `Vector<T,4>`; read one channel with `.r`/`.x` or a
swizzle. `fetch` is the unfiltered twin by integer coord at LOD 0.

```
tex1d.sample(s, u)                  tex1d.fetch(x)
tex2d.sample(s, u, v)              tex2d.fetch(x, y)
tex3d.sample(s, u, v, w)          tex3d.fetch(x, y, z)
arr.sample(s, u, v, layer)        arr.fetch(x, y, layer)   // layer is an INTEGER index
cube.sample(s, dx, dy, dz)        // direction vector; need not be normalized; no fetch
tex2d.sampleLod(s, u, v, lod)     tex2d.fetchLod(x, y, lod)  // Texture2D mip only
```

Sample coords `u,v,w` are normalized `[0,1]`; fetch coords are exact texel/voxel indices.

## Upload layout

`upload(T[])` is row-major / channel-interleaved (R, or R,G,B,A per texel):
Texture1D = `width` texels; Texture2D = `width*height`; Texture3D =
`width*height*depth` (x fastest, then y, then z); Texture2DArray = `width*height*layers`
**layer-major**; TextureCube = `size*size*6` **face-major** in canonical order
**+X, −X, +Y, −Y, +Z, −Z**. UNORM formats take floats in `[0,1]` (quantized to bytes on
store); float/half stored as-is; integer textures take exact integers verbatim. Mipmapped
Texture2D uses `uploadLevel(level, T[])` once per level (level L dims =
`max(1,width>>L) x max(1,height>>L)`).

## Worked example — sampled Texture2D, end to end

Mirrors `test/xpu/XpuCpuDispatchTests.cpp` (`kTextureSampleSource`): a 2×2 R32F image,
bilinear sampling at per-lane `(u,v)`.

```
package app;
import cajeta.xpu.KernelBuffer;
import cajeta.gfx.Texture2D;
import cajeta.gfx.Sampler;
import cajeta.xpu.GpuStream;
import cajeta.xpu.GpuThread;

public class TexSample {
    @Kernel
    public static void sample(Texture2D tex, Sampler s,
                              KernelBuffer<float32> us, KernelBuffer<float32> vs,
                              KernelBuffer<float32> out, uint32 n) {
        uint32 i = GpuThread.globalIdX();
        if (i < n) {
            Vector<float32,4> c = tex.sample(s, us[i], vs[i]);  // device-only
            out[i] = c.x;
        }
    }

    public static int32 run() {
        uint32 w = 2; uint32 h = 2;
        float32[] pixels = heap float32[4];
        pixels[0] = 0.0f; pixels[1] = 1.0f; pixels[2] = 2.0f; pixels[3] = 3.0f;
        Texture2D tex = heap Texture2D(w, h);     // R32F; ~Texture2D frees at scope exit
        tex.upload(pixels);                        // host->device, row-major w*h
        Sampler samp = heap Sampler(1, 0);         // linear, clamp-to-edge

        uint32 n = 5;
        KernelBuffer<float32> us = heap KernelBuffer<float32>(n);
        KernelBuffer<float32> vs = heap KernelBuffer<float32>(n);
        KernelBuffer<float32> out = heap KernelBuffer<float32>(n);
        // ... fill + upload us/vs ...

        GpuStream s = GpuStream.current();
        sample.launch(s, grid: [1], block: [n])(tex, samp, us, vs, out, n);
        s.sync();                                  // tex/samp borrowed until here
        float32[] hout = heap float32[n];
        out.download(hout);
        return 777;
        // tex, buffers freed automatically at scope exit
    }
}
```

## Worked example — writable Image2D

Mirrors the Vulkan storage-image test: a kernel writes each texel, host reads back.

```
import cajeta.xpu.Image2D;
import cajeta.xpu.GpuStream;
import cajeta.xpu.GpuThread;

@Kernel
public static void fill(Image2D img, uint32 w, uint32 h) {
    uint32 i = GpuThread.globalIdX();
    if (i < w * h) { img.store(i % w, i / w, (float32)(i)); }  // device-only write
}
// host:
Image2D img = heap Image2D(w, h);
fill.launch(s, grid: [...], block: [...])(img, w, h);
s.sync();
float32[] out = heap float32[w * h];
img.download(out);                                            // host<-device, row-major
```

## See also

`KernelBuffer` (the flat-array sibling and RAII model these follow), `GpuStream`
(`current()`/`sync()` — the borrow boundary), `GpuThread` (`globalIdX()`), and the
`@Kernel`/`launch` dispatch path.
