# Texture2D\<T = float32\>

`cajeta.gfx.Texture2D` — a read-only 2-D image of texels, sampled in a kernel
through a [Sampler](Sampler.md) with hardware filtering. Like
[`KernelBuffer`](../xpu/KernelBuffer.md), it is a small host-side handle over
a device image resource, with the same RAII memory model: the constructor
acquires the device image, `~Texture2D()` releases it at scope exit, and a
launch borrows each `Texture2D` argument until the next `KernelStream.sync()`.
The type parameter `T` defaults to `float32`, so every bare `Texture2D`
spelling is exactly `Texture2D<float32>`; `sample`/`fetch` return
`Vector<T, 4>`. The storage format is chosen at construction from
`TextureFormat` (default `R32F`). `sample` is float-only; integer textures
(`T = int32`/`uint32`) are fetch-only — the hardware cannot filter integer
texels.

```cajeta
uint32 w = 16;
uint32 hgt = 16;
float32[] pixels = heap float32[w * hgt];
Texture2D tex = heap Texture2D(w, hgt);   // R32F, one float channel
tex.upload(pixels);
// in an @Kernel: tex.sample(samp, u, v) / tex.fetch(x, y)
```

## Methods

| Signature | |
|---|---|
| `Texture2D(uint32 width, uint32 height)` ⚑ | RAII constructor: allocate a `width` x `height` float32 (`R32F`) device image |
| `Texture2D(uint32 width, uint32 height, int32 format)` ⚑ | RAII constructor with an explicit storage `TextureFormat` ordinal, e.g. `TextureFormat.RGBA8_UNORM` |
| `Texture2D(uint32 width, uint32 height, int32 format, uint32 levels)` ⚑ | RAII constructor with explicit mip levels — a mipmapped texture; upload each level with `uploadLevel` |
| `uint32 textureWidth()` | Image width in texels |
| `uint32 textureHeight()` | Image height in texels |
| `void upload(T[] host)` | Host → device transfer of `width`*`height` row-major, channel-interleaved texels |
| `void uploadLevel(uint32 level, T[] host)` | Host → device transfer of one mip level (dims `max(1, width>>level)` x `max(1, height>>level)`) |
| `Vector<T, 4> sample(Sampler s, float32 u, float32 v)` | Device-only intrinsic: filtered sample at normalized (u, v) in [0, 1] through `s`, returning the RGBA texel |
| `Vector<T, 4> fetch(uint32 x, uint32 y)` | Device-only intrinsic: unfiltered, sampler-free read of the texel at exact integer (x, y), mip level 0 |
| `Vector<T, 4> fetchLod(uint32 x, uint32 y, uint32 lod)` | Device-only intrinsic: explicit-LOD `texelFetch` — the mip-aware twin of `fetch` |
| `Vector<T, 4> sampleLod(Sampler s, float32 u, float32 v, float32 lod)` | Device-only intrinsic: explicit-LOD sample; fractional LODs blend two levels (trilinear); float-only |

⚑ = `@EntryPoint`

`sample`, `fetch`, `fetchLod`, and `sampleLod` are lowered at the call site
inside an `@Kernel` / `@Device` function to the backend's image ops; calling
them on the host is unsupported.

## See also

- Tour: [XpuTour](../../../samples/tour/xpu/src/tour/xpu/XpuTour.cajeta)
- [Sampler](Sampler.md) — the filtering + addressing configuration `sample` takes
- [KernelBuffer](../xpu/KernelBuffer.md) — the same RAII handle convention for plain device memory
- Source: [`runtime/src/cajeta/gfx/Texture2D.cajeta`](../../../runtime/src/cajeta/gfx/Texture2D.cajeta)
