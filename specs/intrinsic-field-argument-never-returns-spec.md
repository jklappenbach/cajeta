# intrinsic-field-argument-never-returns — `this.p.waitMillis(this.ms)` never returns (defect spec)

> Status: **draft**, filed 2026-10-25 by the dev.cajeta.bench author (0.1.4,
> `ServiceBenchmark`) from a standalone probe on v0.46.3. A miscompile.

## 1. Definition
A timed wait whose argument is a field read, `this.p.waitMillis(this.ms)`,
never returns; the same call with a local or a literal times out after the
given 300 ms as it should. The library binds such arguments to locals and a
test run hung past its 400 s cap before the cause was found. The shape is an
intrinsic call whose argument expression is a field load through `this`.

## 2. Use cases
- **2.1** As a developer, when I pass a field read as an intrinsic's argument,
  then the intrinsic receives the field's value; the argument is evaluated
  once before the call like any other.
- **2.2** As the test suite, then a JIT test calls the timed wait with a
  field argument and asserts it returns within the timeout, and a second
  test does the same for another intrinsic that takes an integer argument.

## 3. Acceptance
`ServiceBenchmark` can drop its local bindings; the test passes.
