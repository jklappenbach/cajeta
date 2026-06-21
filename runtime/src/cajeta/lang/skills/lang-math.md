---
id: lang-math
applies-to: [cajeta/lang/Math]
title: Math — scalar/vector numeric utilities and device math intrinsics
description: The one access point for numeric functions — templated max/min/clamp plus compiler math intrinsics (sqrt, sin, pow, …) on host and in @Kernel.
---

# Math

The single entry point for numeric functions. Reach for `Math.` for any scalar
or vector math: the larger/smaller/clamped value, square roots, trig,
exponentials, fused multiply-add. It is a **static utility class — you never
construct it and there is no instance.** All members are `static`, called as
`Math.foo(...)`.

```cajeta
import cajeta.lang.Math;

int32   a = Math.max(3, 7);            // 7
int32   c = Math.clamp(15, 0, 10);     // 10  (above range -> hi)
float64 r = Math.sqrt(16.0);           // 4.0
float64 p = Math.pow(2.0, 10.0);       // 1024.0
```

## Two kinds of members

`Math` has two families. **Routing rule:** `max`/`min`/`clamp` are the only
methods *declared* in `Math.cajeta`; everything else (`sqrt`, `sin`, `pow`, …)
is a **compiler intrinsic** the compiler intercepts — there is no declaration
to read, the call lowers to a hardware/`libm`/device-library op directly.

1. **Declared, method-level-templated** — `max<T>`, `min<T>`, `clamp<T>`.
   Monomorphized per `T` at the call site (zero dispatch cost; `static`,
   non-virtual). `T` is inferred from the arguments — do not write the type
   parameter explicitly at the call site.

   - `static T max<T>(T a, T b)` — the larger; returns `a` when `a > b`, else `b`.
   - `static T min<T>(T a, T b)` — the smaller.
   - `static T clamp<T>(T x, T lo, T hi)` — `x` confined to `[lo, hi]`, built as
     `max(lo, min(hi, x))`. Caller must pass `lo <= hi`; there is no validation.

2. **Compiler intrinsics** — not declared here, work both on host and inside an
   `@Kernel`:
   - **Native (lower on every backend, no device library):** `sqrt`, `floor`,
     `ceil`, `trunc`, `round`, `abs`, `fma(a,b,c)` (= `a*b+c`).
   - **Transcendentals:** `sin`, `cos`, `tan`, `asin`, `acos`, `atan`,
     `atan2(y,x)`, `exp`, `exp2`, `log`, `log2`, `log10`, `pow(x,y)`,
     `rsqrt(x)` (= `1/sqrt(x)`).

## Vectorized forms

Every intrinsic also applies **element-wise** to a `Vector<T,N>`, returning a
new `Vector<T,N>`:

```cajeta
Vector<float32,4> v = Vector<float32,4>(0.0f, 0.5f, 1.0f, 2.0f);
Vector<float32,4> s = Math.sin(v);    // (sin v.x, sin v.y, sin v.z, sin v.w)
```

Note: for an **element-wise** max/min over two vectors use the vector's own
`a.max(b)` method — `Math.max` is the scalar reduction `T -> T`, not the vector
form.

## Ownership, state, concurrency

- **No ownership concerns.** Operands and results are primitive value types (or
  `Vector` value types) passed and returned **by value** — nothing is heap-owned,
  no `#` transfer, nothing to `close()`/free, no nullable returns.
- **Stateless and pure.** No instance, no shared state; safe to call from any
  thread/fiber and from inside kernels.

## Inside a kernel / precision

Intrinsics realize per backend in an `@Kernel` (CPU `libm`, Vulkan
`OpExtInst GLSL.std.450`, AMD `ocml`, NVIDIA `libdevice`):

```cajeta
@Kernel
public static void rotate(GpuBuffer<float32> out, float32 angle, uint32 n) {
    uint32 i = Thread.globalIdX();
    if (i < n) { out[i] = Math.cos(angle) - Math.sin(angle); }
}
```

Add `@FastMath` to a `@Kernel` to relax IEEE FP for the whole body (fuse
multiply-adds, use reciprocals, approximate transcendentals — the LLVM
fast-math flags). Opt-in; trades precision for speed.

## Sharp edges

- **v1: primitives only.** The `Comparable` bound on `max`/`min`/`clamp` is not
  enforced yet — the body uses `>`/`<`, which work for built-in numeric
  primitives but **won't link for user-defined classes** until operator
  overloading flows through method-level template specialization. Do not call
  these on your own types yet.
- **`clamp` does not validate `lo <= hi`.** Swapped bounds give a meaningless
  result, no error.
- **No instances, no overloads to construct.** Don't try `new Math()` or look
  for a vector-`max` here; see the note above.
