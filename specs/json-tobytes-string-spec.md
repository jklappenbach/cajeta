# Json.toBytes\<T\> String-field write path — spec (defect)

Found by the tour-quality stdlib review (findings/stdlib.md, Defects).

## 1. Definition

- 1.1 `cajeta.codec.json.Json.toBytes<T>` is unstable when `T` has `String`
  fields; the stdlib tour works around it by restricting JSON round-trips to a
  primitives-only class (`samples/tour/.../JsonDemo.cajeta:38-44`).
- 1.2 Constraint: fix the write path; do not design tour or user code around
  it (no-half-measures).

## 2. Use cases

- 2.1 As a developer, when I call `Json.toBytes<T>` on a class with `String`
  fields and `parse<T>` the result, then the round trip is lossless and
  crash-free.
- 2.2 As the tour maintainer, when 2.1 holds, then JsonDemo round-trips a
  realistic mixed-field record and the primitives-only restriction is removed.

## 3. Acceptance

- 3.1 Regression test: mixed String/primitive round-trip in the codec/json
  suite; JsonDemo un-restricted; tour green.
