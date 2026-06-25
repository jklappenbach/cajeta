---
id: gfx-texture2d
applies-to: [cajeta/gpu/Texture2D]
title: Texture2D — read-only sampled/fetched 2-D device image
description: Construct/upload a Texture2D (format, mip levels) on the host, then read it device-only via filtered sample/sampleLod (needs a Sampler) or unfiltered fetch/fetchLod by integer texel.
---

`Texture2D<T = float32>` is the canonical **read-only** 2-D image you read inside a
kernel. A bare `Texture2D` is `Texture2D<float32>`. It is the **entry-point / access
point** for textured reads: you construct and `upload` it on the host, pass it (and a
`Sampler`, for filtered reads) as a kernel argument, and read it **only on the device**.
`T` is the per-channel scalar; every read returns `Vector<T, 4>` (RGBA).

Two read paths, picked by *what you need*, not by preference:

- **`sample` / `sampleLod`** — filtered, normalized `(u,v)` in `[0,1]`, **requires a
  `Sampler`**. Hardware blends neighbouring texels (bilinear/trilinear). **Float-only.**
  Use for images/lookups where an interpolated value is wanted.
- **`fetch` / `fetchLod`** — unfiltered, **exact integer texel** `(x,y)`, **no Sampler**.
  Returns the one stored texel. Use for data textures / lookup tables, and it is the
  **only** read for integer textures (`T = int32`/`uint32`).

## Does NOT do

- **No host-side reads.** `sample`/`fetch`/`sampleLod`/`fetchLod` are device intrinsics
  (lowered at the call site inside an `@Kernel`/`@Device` fn); their bodies are
  placeholders, never a real call. Calling them outside a kernel is unsupported.
- **No writes.** Texture2D is read-only — there is no `store`. For a writable texel grid
  use `Image2D` (load/store).
- **No `fetch` filtering or addressing.** `fetch` has no wrap/clamp (those are `Sampler`
  properties); `(x,y)` MUST be in bounds (`0 <= x < width`, `0 <= y < height`).
- **No `sample` on integer textures** — the hardware cannot filter integer texels; an
  integer `Texture2D` is fetch-only.

## Construction & ownership (host-side, RAII)

Construct on the heap; the constructor **acquires** the device image and `~Texture2D()`
**releases** it via the drop chain at scope exit (the `KernelBuffer` RAII convention) —
null-guarded and idempotent. You do not free it manually.

- `Texture2D(uint32 width, uint32 height)` — `R32F` (1 float channel), 1 mip level.
- `Texture2D(uint32 width, uint32 height, int32 format)` — explicit `TextureFormat`
  ordinal (the enum constant resolves to its `i32` ordinal).
- `Texture2D(uint32 width, uint32 height, int32 format, uint32 levels)` — mipmapped;
  `levels` is the chain length (level L dims `max(1,width>>L) x max(1,height>>L)`).

`TextureFormat` ordinals are the stable native contract: `R32F`(0), `R8_UNORM`(1),
`RGBA8_UNORM`(2), `RGBA32F`(3), `R16F`(4), `RGBA16F`(5), and the **fetch-only** integer
formats `R32I`(6)/`R32UI`(7)/`RGBA32I`(8)/`RGBA32UI`(9). Pair integer formats with
`T = int32`/`uint32`. See `cajeta/gpu/TextureFormat`.

## Upload (host-side)

- `void upload(T[] host)` — `width*height` row-major, channel-interleaved texels
  (`count = width*height*channels`). UNORM formats take floats in `[0,1]` (quantized on
  store); float/half stored as-is; integer formats stored verbatim.
- `void uploadLevel(uint32 level, T[] host)` — one mip level (dims as above); call once
  per level for a mipmapped texture.

The `host` array is **borrowed** for the duration of the copy (host→device); you keep
ownership and may free/reuse it after the call returns.

## Lifecycle across the launch boundary

A launch **borrows** each `Texture2D` argument until the next `GpuStream.sync()`. Keep it
(and the `Sampler`) alive until after `sync()`. `Sampler` is admissible as a kernel
argument **by name** (not as a POD struct) — see `cajeta/gpu/Sampler`
(`filterMode`: 0=nearest, 1=linear; `addressMode`: 0=clamp, 1=repeat).

## The methods (device-only, return `Vector<T, 4>`)

- `Vector<T,4> sample(Sampler s, float32 u, float32 v)` — filtered, level 0.
- `Vector<T,4> sampleLod(Sampler s, float32 u, float32 v, float32 lod)` — filtered at
  explicit `lod` (fractional blends two levels, trilinear); float-only.
- `Vector<T,4> fetch(uint32 x, uint32 y)` — unfiltered exact texel, level 0.
- `Vector<T,4> fetchLod(uint32 x, uint32 y, uint32 lod)` — unfiltered in mip `lod`;
  `(x,y)` are in that level's own grid.

Single-channel formats land in `.r`/`.x` (G/B = 0, A = 1); RGBA formats fill all four.

## Worked example (filtered sample, mirrors XpuCudaDispatchDeviceTests)

```cajeta
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
            Vector<float32, 4> c = tex.sample(s, us[i], vs[i]);  // device-only
            out[i] = c.x;
        }
    }

    public static int32 run() {
        uint32 w = 2; uint32 h = 2;
        float32[] pixels = heap float32[4];                  // row-major R32F
        pixels[0] = 0.0f; pixels[1] = 1.0f; pixels[2] = 2.0f; pixels[3] = 3.0f;
        Texture2D tex = heap Texture2D(w, h);                // R32F, acquires device image
        tex.upload(pixels);                                  // pixels borrowed during copy
        Sampler samp = heap Sampler(1, 0);                   // linear, clamp-to-edge
        // ... fill KernelBuffer us/vs/out (see cajeta/gpu/KernelBuffer) ...
        GpuStream s = GpuStream.current();
        sample.launch(s, grid: [1], block: [64])(tex, samp, us, vs, out, n);
        s.sync();                                            // borrow of tex/samp ends here
        // tex + samp device image released automatically at scope exit
        return 0;
    }
}
```

For an integer **fetch** texture, parameterize both the field and the `heap`:
`Texture2D<int32> tex = heap Texture2D<int32>(w, h, TextureFormat.R32I);` then
`tex.fetch(x, y).x` in the kernel (no `Sampler`).

For the `#`-move spelling (transferring the handle out of a scope) the moved-from
destructor is a no-op — see the `KernelBuffer` ownership rules. Buffers/streams:
`cajeta/gpu/KernelBuffer`, `cajeta/gpu/GpuStream`.
