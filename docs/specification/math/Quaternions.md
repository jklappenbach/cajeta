# cajeta.math.Quaternion

A `Quaternion<T>` is a compact, gimbal-lock-free representation of a 3-D rotation
— four floats `(w, x, y, z)` = `w + x·i + y·j + z·k`, lowering to a flat
`<4 x T>` (float element only). It does three things a rotation matrix does
clumsily: **compose** rotations cheaply, **rotate** a vector, and **interpolate**
smoothly between orientations.

```
// w is the scalar part; (x,y,z) the axis. A rotation by angle θ about a unit
// axis (ax,ay,az) is (cos(θ/2), ax·sin(θ/2), ay·sin(θ/2), az·sin(θ/2)).
Quaternion<float32> q = heap Quaternion<float32>(0.70710678f, 0.0f, 0.0f, 0.70710678f); // 90° about z
```

## Rotate a vector — `q * v`
`*` with a `Vector<T,3>` applies the rotation (the branchless q·v·q⁻¹):
```
Vector<float32,3> r = q * v;          // v rotated by q
```

## Compose rotations — `q1 * q2`
`*` with another quaternion is the Hamilton product — the rotation "first `q2`,
then `q1`". Cheaper than a 3×3·3×3 matmul, and it never drifts to a
non-orthogonal matrix under accumulation:
```
Quaternion<float32> spin = yaw * pitch;     // one quaternion doing both
```

## Undo a rotation — `conjugate()`
For a unit quaternion the inverse is the conjugate `(w, -x, -y, -z)`:
```
Vector<float32,3> back = q.conjugate() * (q * v);   // == v
```

## Interpolate orientation — `slerp(other, t)` / `nlerp(other, t)`
Blend between two orientations along the shortest arc (`t` ∈ [0,1]):
```
Quaternion<float32> a_to_b = a.slerp(b, 0.5f);   // constant angular velocity
Quaternion<float32> cheap  = a.nlerp(b, 0.5f);   // normalized lerp (faster)
```
`slerp` interpolates at constant angular velocity (the "correct" rotation blend);
`nlerp` is a cheaper normalized lerp that's very close for small angles. **Both
run on CPU, Vulkan, and AMD** — `slerp` uses `acos`/`sin` (the device
transcendentals) and falls back to `nlerp` automatically when the two
orientations are nearly parallel (where the sin-divide would blow up). Both are
branchless (the shortest-arc flip is a `select`).

## The rest
`normalize()` (re-unit a quaternion that drifted under accumulation), `length()`,
`dot(other)` (relates to the cosine of the half-angle between two orientations),
and `+ -` (element-wise, for interpolation setup).

## Why a quaternion, not the alternatives?
| Alternative | Cost |
|---|---|
| **3×3 rotation matrix** | 9 floats; composition is a 27-mul matmul; **drifts non-orthogonal** under repeated multiply; no clean interpolation. |
| **Euler angles** (yaw/pitch/roll) | **Gimbal lock**; order-dependent; interpolation is ill-defined near the poles. |
| **Axis-angle** | Compact, but composition and interpolation both require conversions. |

A quaternion is 4 floats, composes in 16 muls, **stays a rotation** under
re-normalization, and interpolates with `nlerp`/`slerp`. The same source runs on
CPU, Vulkan, and AMD — see `samples/Tour` (the `QuaternionDemo` host walk and the
XPU `quaternion` kernels). Surface details in `ValueTypeCatalog.md`.
