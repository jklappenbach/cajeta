# math-fma-host — `Math.fma` and the element-wise forms on the host (defect spec)

> Status: **active**, scope widened and approved by Julian 2026-11-15 ("Add the
> Math.fma work"; decisions below). Filed 2026-10-25 by benchmark-reference
> unit 5 on v0.46.3. Plan: [host-simd](../agents/host-simd-plan.md).

## 1. Definition

`Math.fma(a, b, c)` computes `a * b + c` with one rounding. Inside a kernel it
lowers to `llvm.fma`. In host code it does not exist, and a host call fails
with `CAJETA_ERROR_MEMBER_NOT_FOUND`, although `cajeta.lang.Math` lists it among
the intrinsics that lower directly on every backend. Host code never fuses a
multiply and an add on its own either, so a host loop that wants a fused
multiply-add has no way to ask for one.

The same reference says every intrinsic applies element-wise to a
`Vector<T,N>`. On the host that is false for the native forms. The host path
widens each argument to `float64`, and `Math.sqrt(v)` on a `Vector<float32,4>`
stops the LLVM backend (`Cannot select … anyext from v4f32`, measured on
v0.53.7).

Both block cajeta-ml's host matrix product, which sits at the ceiling of
separate multiplies and adds (about 65 GFLOP/s a core on Zen 2, spela-kernels
10.2.7), and a vectorized AdamW, which needs a lane-wise `sqrt`.

Decisions (Julian, 2026-11-15):

- `Math.fma` computes at its operands' own precision. Float32 operands make a
  float32 fma, float64 a float64 one. Widening to float64 first would round
  twice and would not be a fused multiply-add.
- The unit covers `Math.fma` on `Vector<T,N>` lane by lane, and the native
  element-wise forms on the host: `sqrt`, `abs`, `floor`, `ceil`, `trunc` and
  `round` on a Vector keep the Vector's lane type.

The transcendentals on a host Vector (`sin`, `exp`, …) are outside this unit.
Two guide errors from the original filing remain in scope: the guide says
`instanceof` checks the static type (it checks the runtime type), and its
untyped single-parameter lambda in a declaration, `(x) -> x + k`, does not
parse.

## 2. Use cases

- **2.1** As a host program, when I call `Math.fma(a, b, c)` on float32 or
  float64 values, I get a fused multiply-add at that precision (`llvm.fma`), and
  the docs say so.
- **2.2** As a host program, when I call `Math.fma` on three `Vector<T,N>` of
  one floating type, every lane is fused, and a mixed scalar and Vector call is
  a compile error that names the operand.
- **2.3** As a host program, when I call `Math.sqrt`, `abs`, `floor`, `ceil`,
  `trunc` or `round` on a `Vector<T,N>`, I get a Vector of the same type, lane
  by lane, where today the backend stops.
- **2.4** As the test suite, a JIT test calls `Math.fma` on the host at a case
  where fused and unfused results differ (`(1 + 2^-23)^2 - (1 + 2^-22)` is
  `2^-46` fused and 0 unfused), scalar and Vector, both precisions.
- **2.5** As cajeta-ml, HostGemm's register tile accumulates with `Math.fma`
  and reaches past the unfused ceiling.

## 3. Acceptance

- The `lang/math-intrinsics` row uses `Math.fma` in place of its exact
  `a*0.5+1.0` substitute.
- The scalar float32 host forms already in place (`sqrt` and the rest) are
  unchanged: they still compute through float64, as today.
- The two guide errors are fixed. The lambda one was fixed before this unit:
  `(int32) -> int32 mul = (x) -> x * k;` compiles and runs on main at
  d0f58bc75 (measured 2026-11-15), so only the `instanceof` paragraph changed.
