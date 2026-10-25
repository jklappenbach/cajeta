# spawn-qualified-static-rejected — `spawn Class.method(...)` is rejected as an instance call (defect spec)

> Status: **draft**, filed 2026-10-25 by benchmark-reference unit 7 on v0.46.3.

## 1. Definition
`spawn Class.method(args)` on a static method fails with
`CAJETA_ERROR_ASYNC_R3A` ("doesn't support instance-method calls"); the bare
`spawn method(args)` inside the same class works. The qualified form is the
only way to spawn a static method of another class.

## 2. Use cases
- **2.1** As a developer, when I spawn a qualified static method, then it
  spawns; the R3A check distinguishes a type receiver from a value receiver.
- **2.2** As the test suite, then a JIT test spawns `Other.work(1)` and
  awaits its result, and a test asserts that `obj.method()` still reports
  R3A.

## 3. Acceptance
The concurrent benchmarks' spawn sites can use the qualified form.
