---
id: math-scene-transform
applies-to: [cajeta/math/Matrix, cajeta/math/Transform, cajeta/math/Rotation, cajeta/math/Camera, cajeta/math/Color]
title: Scene & render math — Transform, Rotation, Camera, Matrix, Color
description: How the cajeta.math render value types cooperate (TRS transform, quaternion builders, view/projection matrices, sRGB color) and the conventions they all share.
---

# Scene & render math (cajeta.math)

The value types a renderer/scene-graph stacks together to place objects, point a
camera, and convert color. All are **stack value types** — no heap, no `close()`,
no drop chain; they live and die with their scope and are passed/returned by value.
Members (`translation`, `r`, etc.) are public, so a returned value is fully owned by
the caller; nothing is borrowed across these boundaries.

## Pick the right member

| Want to… | Use |
| --- | --- |
| Place an object (translate+rotate+scale a point/dir) | `Transform` (`transformPoint`/`transformVector`) |
| Build a rotation from axis+angle or Euler | `Rotation.fromAxisAngle` / `Rotation.fromEuler` → `Quaternion<float32>` |
| Make a camera view or projection matrix | `Camera.lookAt` / `Camera.perspective` / `Camera.ortho` → `Matrix<float32,4,4>` |
| Multiply matrices / matrix·vector, transpose, invert | builtin `Matrix<T,R,C>` operators (`*`, `transpose()`, `inverse()`) |
| Convert sRGB ↔ linear light | `Color.toLinear()` / `Color.toSrgb()` |

Not here: `Transform` has **no** `toMatrix()` — it maps points directly via
quaternion math; build a 4×4 only through `Camera`. `Rotation`/`Camera` are
**static-utility classes** (like `cajeta.lang.Math`) — do not instantiate them.
The quaternion *algebra* (`q*q`, `q*v`, `conjugate()`, `slerp`, `nlerp`, `dot`,
`length`) lives on the builtin `Quaternion<float32>`, not on `Rotation`.

## Locked conventions every member shares

- **Right-handed view space** — camera looks down −z, +x right, +y up.
- **Clip depth [0,1]** (Vulkan/D3D), NOT OpenGL's [−1,1].
- **Column-vector, `m * v`** — a point is `Matrix<float32,4,4> * Vector<float32,4>`.
- **Row-major storage** — element (r,c) at flat lane `r*C+c`; index as `m[r][c]`.
- **TRS order = SCALE → ROTATE → TRANSLATE** for `Transform.transformPoint`:
  `translation + rotation * (scale ⊙ p)`.
- **Quaternion storage is `(w, x, y, z)`** — the constructor is
  `Quaternion<float32>(w, x, y, z)`; identity is `(1,0,0,0)`.
- **Angles are radians.** `Camera.perspective` fovy, `Rotation` angles — all radians.

## Object graph / data flow

`Rotation` produces a `Quaternion<float32>` → fed into a `stack Transform(tr, rot, sc)`
or used directly as `q * v`. `Camera` consumes plain `Vector<float32,3>` eye/center/up
and emits `Matrix<float32,4,4>`; chain projection·view with the builtin matrix multiply
`proj * view`. `Transform` and `Camera` are independent paths to "place geometry" — a
scene graph composes `Transform`s (`compose`), the renderer's camera uses `Camera`
matrices. `Color` is orthogonal (shading), sharing only the value-type lifecycle.

## Worked example — full scene-math path

```cajeta
import cajeta.math.Transform;
import cajeta.math.Rotation;
import cajeta.math.Camera;
import cajeta.math.Color;

// Build a rotation, place an object with a TRS transform.
Vector<float32,3> up = stack Vector<float32,3>(0.0f, 1.0f, 0.0f);
Quaternion<float32> rot = Rotation.fromAxisAngle(up, 0.7853982f);  // 45deg about +y
Vector<float32,3> tr = stack Vector<float32,3>(0.0f, 0.0f, 5.0f);
Vector<float32,3> sc = stack Vector<float32,3>(2.0f, 2.0f, 2.0f);
Transform t = stack Transform(tr, rot, sc);
Vector<float32,3> p = stack Vector<float32,3>(1.0f, 0.0f, 0.0f);
Vector<float32,3> world = t.transformPoint(p);     // scale, then rotate, then translate

// Camera: view * projection, both 4x4, multiplied with builtin matrix multiply.
Vector<float32,3> eye = stack Vector<float32,3>(0.0f, 2.0f, 5.0f);
Vector<float32,3> at  = stack Vector<float32,3>(0.0f, 0.0f, 0.0f);
Matrix<float32,4,4> view = Camera.lookAt(eye, at, up);
Matrix<float32,4,4> proj = Camera.perspective(1.0472f, 1.7778f, 0.1f, 1000.0f);
Matrix<float32,4,4> viewProj = proj * view;        // m * v applies view first
float32 m00 = viewProj[0][0];                       // row-major m[r][c]

// Color: shade in linear, store/display in sRGB.
Color albedo = stack Color(0.5f, 0.25f, 0.1f, 1.0f);  // sRGB from a texture
Color lit = albedo.toLinear();                        // alpha passes through unchanged
Color outc = lit.toSrgb();
```

`Transform.identity()` and `Rotation.identity()` give the no-op starting points.
`t.compose(child)` applies `child` then `this`; `t.inverse()` undoes `t`.

## Sharp edges

- **One operator per statement.** A compound `Vector`/`Quaternion`/`Matrix`
  expression in a single statement crashes host codegen today — break into
  intermediate locals (this is why the source bodies look verbose). Also store a
  cross-method-call result in a local before using it as an operator operand.
- **`Matrix * Matrix` and `Matrix * Vector` are matrix multiply**, not element-wise;
  `m * scalar` scales; the element-wise product is `m.hadamard(b)`. Matrix comparison
  (`==`) yields a per-lane mask `Matrix<boolean,R,C>` — whole-matrix equality is
  `(a == b).all()`.
- **`compose`/`inverse` stay exact only for UNIFORM scale** (or identity rotation).
  Non-uniform scale + rotation is not closed under the TRS form — documented
  limitation; use a full 4×4 / polar decomposition if you need it.
- **`Color` channels are assumed in [0,1]**; the sRGB transfer is the IEC 61966-2-1
  piecewise curve (linear segment near black, gamma-2.4 above). Convert per-channel
  via the static `srgbToLinearChannel`/`linearToSrgbChannel` if you need scalar form.
- `Rotation.fromEuler(pitchX, yawY, rollZ)` applies roll(+z), then yaw(+y), then
  pitch(+x): `qZ * qY * qX`. `fromAxisAngle` normalizes the axis for you.

For per-class operator/method detail see the `Matrix` class skill; for the builtin
`Quaternion<float32>`/`Vector<float32,N>` algebra see the builtin-types skills.
