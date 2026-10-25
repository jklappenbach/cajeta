# jit-tensor-lambda-crash — `Jit` over a Tensor lambda compiles and crashes at the call (defect spec)

> Status: **draft**, filed 2026-10-25 by benchmark-reference unit 12 on v0.46.3.

## 1. Definition
`Jit` applied to a lambda over a `Tensor` compiles and SIGSEGVs at the call,
even for `t*t + t`. `Jit(Vmap(f))` over a scalar `f` works and is what
`nucleo/transforms-jit` measures. From the same unit: `Fuse` accepts one
tensor parameter (five give `TRANSFORM_UNSUPPORTED_BODY`, and a captured
tensor does not resolve inside the body); `Vmap` has no batching rule for
`Tensor.sum` (`TRANSFORM_NO_BATCH_RULE`).

## 2. Use cases
- **2.1** As a developer, when I `Jit` a tensor lambda, then it runs, or the
  compiler rejects the body by name; a crash is never the answer.
- **2.2** As the test suite, then a JIT test runs `Jit((t) -> t*t + t)` on a
  small tensor and checks the values; a second test asserts the diagnostic
  for an unsupported body.

## 3. Acceptance
`nucleo/transforms-jit` uses the tensor form and its note is removed.
