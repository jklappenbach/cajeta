# Transform

`cajeta.math.Transform` — a TRS (translate / rotate / scale) rigid+scale
transform, the object-placement value type a scene graph and a renderer's
model matrix are built from. A value type over the builtin
`Vector<float32,3>` (translation, scale) and `Quaternion<float32>`
(rotation): stack-allocated, no heap, no drop chain. Applied to a point the
order is scale, then rotate, then translate; `transformVector` drops the
translation. Compose and inverse keep the TRS form exactly for uniform scale
— a non-uniform scale combined with a rotation is not closed under TRS (a
documented limitation).

```cajeta
package snip.transform;

import cajeta.math.Transform;
import cajeta.math.Rotation;

public final class Demo {
    public static void run() {
        Vector<float32,3> tr = stack Vector<float32,3>(0.0f, 0.0f, 5.0f);
        Quaternion<float32> rot = Rotation.identity();
        Vector<float32,3> sc = stack Vector<float32,3>(2.0f, 2.0f, 2.0f);
        Transform t = stack Transform(tr, rot, sc);
        Vector<float32,3> p = stack Vector<float32,3>(1.0f, 0.0f, 0.0f);
        Vector<float32,3> w = t.transformPoint(p);   // (2, 0, 5)
        return;
    }
}
```

## Methods

| Signature | |
|---|---|
| `Transform(Vector<float32,3> translation, Quaternion<float32> rotation, Vector<float32,3> scale)` ⚑ | Build a transform from its translation, rotation, and scale components |
| `static Transform identity()` ⚑ | The identity transform: no translation, no rotation, unit scale |
| `Vector<float32,3> translation()` | The translation component |
| `Quaternion<float32> rotation()` | The rotation component (a unit quaternion) |
| `Vector<float32,3> scale()` | The scale component |
| `Vector<float32,3> transformPoint(Vector<float32,3> p)` | Map a point through the transform: translate(rotate(scale(p))) |
| `Vector<float32,3> transformVector(Vector<float32,3> v)` | Map a direction: rotate(scale(v)) — no translation |
| `Transform compose(Transform child)` | The transform that applies `child` first, then `this` (exact for uniform scale) |
| `Transform inverse()` | The inverse transform: `inverse().transformPoint(transformPoint(p)) == p` (exact for uniform scale) |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/math/Transform.cajeta`](../../../runtime/src/cajeta/math/Transform.cajeta)
- [Rotation](Rotation.md) — builds the rotation component, [Ray](Ray.md) — the parametric query primitive
