# Rotation

`cajeta.math.Rotation` — quaternion construction for a renderer or scene
graph. A pure static-utility class: each method returns a unit
`Quaternion<float32>` ready to feed [Transform](Transform.md) or the builtin
rotate-vector form `q * v`. The builtin `Quaternion<float32>` already
provides the algebra (Hamilton product, vector rotation, `conjugate()`,
`slerp`, `nlerp`, `dot`, `length`); this class adds the missing ways to build
a rotation from an axis+angle or Euler angles. Quaternion storage is
`(w, x, y, z)`.

```cajeta
package snip.rotation;

import cajeta.math.Rotation;

public final class Demo {
    public static void run() {
        Vector<float32,3> up = stack Vector<float32,3>(0.0f, 1.0f, 0.0f);
        Quaternion<float32> yaw = Rotation.fromAxisAngle(up, 0.7853982f);  // 45 deg
        Vector<float32,3> v = stack Vector<float32,3>(1.0f, 0.0f, 0.0f);
        Vector<float32,3> r = yaw * v;   // builtin rotate-vector
        return;
    }
}
```

## Methods

| Signature | |
|---|---|
| `static Quaternion<float32> identity()` | The identity rotation: the unit quaternion `(1,0,0,0)` |
| `static Quaternion<float32> fromAxisAngle(Vector<float32,3> axis, float32 angleRadians)` ⚑ | A rotation of `angleRadians` about `axis` (right-handed) |
| `static Quaternion<float32> fromEuler(float32 pitchX, float32 yawY, float32 rollZ)` ⚑ | A rotation from Euler angles (radians), applied roll (about +z), then yaw (about +y), then pitch (about +x): `q = qZ * qY * qX` |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/math/Rotation.cajeta`](../../../runtime/src/cajeta/math/Rotation.cajeta)
- [Transform](Transform.md) — consumes the quaternions built here, [Camera](Camera.md) — view/projection builders
