# Camera

`cajeta.math.Camera` — the projection and view matrix builders a renderer's
camera produces. A pure static-utility class (no instance state); each method
returns a `Matrix<float32,4,4>`. Conventions are locked: right-handed view
space (camera looks down −z, +x right, +y up), clip-space depth range
`[0, 1]` (Vulkan / D3D, not OpenGL's `[-1, 1]`), and column-vector transforms
(`m * v`) with row-major storage.

```cajeta
package snip.camera;

import cajeta.math.Camera;

public final class Demo {
    public static void run() {
        Matrix<float32,4,4> proj = Camera.perspective(1.0472f, 1.7778f, 0.1f, 1000.0f);
        Vector<float32,3> eye = stack Vector<float32,3>(0.0f, 2.0f, 5.0f);
        Vector<float32,3> at = stack Vector<float32,3>(0.0f, 0.0f, 0.0f);
        Vector<float32,3> up = stack Vector<float32,3>(0.0f, 1.0f, 0.0f);
        Matrix<float32,4,4> viewMat = Camera.lookAt(eye, at, up);
        Matrix<float32,4,4> viewProj = proj * viewMat;   // builtin matrix multiply
        return;
    }
}
```

## Methods

| Signature | |
|---|---|
| `static Matrix<float32,4,4> perspective(float32 fovyRadians, float32 aspect, float32 near, float32 far)` ⚑ | Right-handed perspective projection with clip depth `[0,1]`; `fovyRadians` is the vertical field of view in radians |
| `static Matrix<float32,4,4> ortho(float32 left, float32 right, float32 bottom, float32 top, float32 near, float32 far)` ⚑ | Right-handed orthographic projection with clip depth `[0,1]` |
| `static Matrix<float32,4,4> lookAt(Vector<float32,3> eye, Vector<float32,3> center, Vector<float32,3> up)` ⚑ | Right-handed view matrix placing the camera at `eye` looking toward `center` with the given `up` reference |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/math/Camera.cajeta`](../../../runtime/src/cajeta/math/Camera.cajeta)
- [Transform](Transform.md) — object placement, [Rotation](Rotation.md) — quaternion builders
