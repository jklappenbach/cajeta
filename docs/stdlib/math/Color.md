# Color

`cajeta.math.Color` — an RGBA color, the value type a renderer passes between
sRGB-encoded texture/UI space and linear light space. A `record` over four
`float32` channels (r, g, b, a), each in `[0,1]`: structural equality (two
colors with equal channels compare `==`), and the flat no-header layout — the
struct is just the four channel slots, no vtable. Construct through the
constructor (`Color(r, g, b, a)`), or with a record binding — named
(`Color { r: ..., g: ..., b: ..., a: ... }`) or positional in declared order
(`Color { r, g, b, a }`); the fields declare no defaults, so all four channels
must be given. Shading must happen in linear light, but textures and UI colors
are usually sRGB-encoded: `toLinear()` decodes sRGB → linear, `toSrgb()`
encodes linear → sRGB, both using the standard IEC 61966-2-1 piecewise
transfer function. Alpha is linear and passes through unchanged.

```cajeta
package snip.color;

import cajeta.math.Color;

public final class Demo {
    public static void run() {
        Color albedo = stack Color(0.5f, 0.25f, 0.1f, 1.0f);  // sRGB from a texture
        Color same = Color { r: 0.5f, g: 0.25f, b: 0.1f, a: 1.0f };
        boolean eq = albedo == same;                          // true — structural
        Color lit = albedo.toLinear();                        // shade in linear light
        Color enc = lit.toSrgb();                             // encode for the display
        return;
    }
}
```

## Methods

| Signature | |
|---|---|
| `Color(float32 r, float32 g, float32 b, float32 a)` ⚑ | Build a color from its r, g, b, a channels, each in `[0,1]` |
| `float32 r()` | Red channel |
| `float32 g()` | Green channel |
| `float32 b()` | Blue channel |
| `float32 a()` | Alpha channel |
| `static float32 srgbToLinearChannel(float32 c)` | Decode one sRGB-encoded channel to linear light |
| `static float32 linearToSrgbChannel(float32 c)` | Encode one linear-light channel to sRGB |
| `Color toLinear()` | This color decoded from sRGB to linear light (alpha unchanged) |
| `Color toSrgb()` | This color encoded from linear light to sRGB (alpha unchanged) |

⚑ = `@EntryPoint`

## See also

- Tour: [GeometryDemo](../../../samples/tour/src/main/cajeta/tour/math/GeometryDemo.cajeta)
- Source: [`runtime/src/cajeta/math/Color.cajeta`](../../../runtime/src/cajeta/math/Color.cajeta)
- [Texture2D](../gfx/Texture2D.md) — sRGB-encoded texture data
