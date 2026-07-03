# Sampler

`cajeta.gfx.Sampler` — filtering + addressing configuration for
[`Texture2D.sample`](Texture2D.md). A small host-side value carrying the mode
bits; the runtime builds the backend sampler object (Vulkan VkSampler,
CUDA/HIP texture-object sampler state, or CPU emulation) from these at launch.
A `Sampler` is admissible as a kernel argument by name. v1 limits: two filter
modes and two address modes, isotropic — no mip bias, anisotropy, border
color, or compare.

```cajeta
Sampler lin = stack Sampler(1, 0);    // linear filter, clamp-to-edge
Sampler wrap = heap Sampler(0, 1);    // nearest filter, repeat
```

## Methods

| Signature | |
|---|---|
| `Sampler(int32 filterMode, int32 addressMode)` ⚑ | Build a sampler from its filter mode (0 = nearest, 1 = linear) and address mode (0 = clamp-to-edge, 1 = repeat) |

⚑ = `@EntryPoint`

## See also

- Tour: [XpuTour](../../../samples/tour/xpu/src/tour/xpu/XpuTour.cajeta)
- [Texture2D](Texture2D.md) — the texture this sampler filters
- Source: [`runtime/src/cajeta/gfx/Sampler.cajeta`](../../../runtime/src/cajeta/gfx/Sampler.cajeta)
