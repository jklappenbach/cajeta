# math-fma-host — `Math.fma` is kernel-only though documented as native everywhere (defect spec)

> Status: **draft**, filed 2026-10-25 by benchmark-reference unit 5 on v0.46.3.

## 1. Definition
`Math.fma` works inside a kernel and not in host code, while the stdlib
reference lists it among the numeric utilities available everywhere. The
`lang/math-intrinsics` row substitutes an exact `a*0.5+1.0` and says so.
Two documentation errors from the same pass: the guide says `instanceof`
checks the static type (it checks the runtime type), and the guide's
untyped single-parameter lambda in a declaration, `(x) -> x + k`, does not
parse.

## 2. Use cases
- **2.1** As a host program, when I call `Math.fma(a, b, c)`, then I get a
  fused multiply-add (the `llvm.fma` intrinsic, or a correctly rounded
  software fallback) and the docs say which.
- **2.2** As the test suite, then a JIT test calls `Math.fma` on the host and
  checks a case where fused and unfused differ.

## 3. Acceptance
`lang/math-intrinsics` uses `Math.fma`; the two guide errors are fixed.
