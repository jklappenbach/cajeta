# Math

`cajeta.lang.Math` — the one stop for scalar and vector numeric functions. It
has two kinds of members: the declared, method-level-templated `max` / `min` /
`clamp` below (each monomorphized per `T` at the call site, zero dispatch
cost), and compiler intrinsics — `sqrt`, `floor`, `ceil`, `trunc`, `round`,
`abs`, `fma`, plus the transcendentals `sin`, `cos`, `tan`, `asin`, `acos`,
`atan`, `atan2`, `exp`, `exp2`, `log`, `log2`, `log10`, `pow`, `rsqrt` — which
are not declared in the class; the compiler intercepts the call and emits the
right instruction on the host and inside an `@Kernel`. Intrinsics also apply
element-wise to a `Vector<T,N>`, and `@FastMath` on a kernel relaxes IEEE FP
for its whole body.

```cajeta
int32 a = Math.max(3, 7);               // 7   (max<int32>)
int32 b = Math.min(3, 7);               // 3
float64 c = Math.clamp(1.5, 0.0, 1.0);  // 1.0 (clamp<float64>)
```

## Methods

| Signature | |
|---|---|
| `static T max<T>(T a, T b)` ⚑ | The larger of `a` and `b`; per-`T` monomorphization |
| `static T min<T>(T a, T b)` ⚑ | The smaller of `a` and `b` |
| `static T clamp<T>(T x, T lo, T hi)` ⚑ | `x` clamped to `[lo, hi]` |

⚑ = `@EntryPoint`

v1 limitation: the bodies use `>` / `<`, which work for built-in primitives
but won't link for user classes until operator overloading flows through
method-level template specialization.

## See also

- Tour: [MathDemo](../../../samples/tour/src/main/cajeta/tour/math/MathDemo.cajeta),
  [XpuTour](../../../samples/tour/xpu/src/tour/xpu/XpuTour.cajeta)
- Source: [`runtime/src/cajeta/lang/Math.cajeta`](../../../runtime/src/cajeta/lang/Math.cajeta)
