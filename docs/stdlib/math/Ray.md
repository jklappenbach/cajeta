# Ray

`cajeta.math.Ray` — a half-line `origin + t * direction` for `t >= 0`, the
parametric query primitive shared by picking, culling, and the software
ray-query paths. A value type over the builtin `Vector<float32,3>`:
stack-allocated by default, no heap, no drop chain. `direction` is stored
as-given (not auto-normalized); intersection helpers that assume a unit
direction document that requirement.

```cajeta
package snip.ray;

import cajeta.math.Ray;

public final class Demo {
    public static void run() {
        Vector<float32,3> o = stack Vector<float32,3>(0.0f, 0.0f, 0.0f);
        Vector<float32,3> d = stack Vector<float32,3>(0.0f, 0.0f, 1.0f);
        Ray r = stack Ray(o, d);
        Vector<float32,3> p = r.at(5.0f);   // (0, 0, 5)
        return;
    }
}
```

## Methods

| Signature | |
|---|---|
| `Ray(Vector<float32,3> origin, Vector<float32,3> direction)` ⚑ | Build a ray from its origin and direction (direction stored as given) |
| `Vector<float32,3> origin()` | The ray's start point |
| `Vector<float32,3> direction()` | The ray's direction (stored as given; not auto-normalized) |
| `Vector<float32,3> at(float32 t)` | The point `origin + t * direction` |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/math/Ray.cajeta`](../../../runtime/src/cajeta/math/Ray.cajeta)
- [Camera](Camera.md) — view/projection builders for picking, [Transform](Transform.md) — object placement
