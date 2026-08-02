# Cross-.cja exception catch crash — spec (defect)

Found by the tour-quality unit review (findings/unit.md, Defects).

## 1. Definition

- 1.1 Catching an exception thrown inside a linked library `.cja`
  (dev.cajeta.unit) from the consumer binary crashes instead of unwinding to
  the catch. Documented workaround notes in
  `cajeta-unit/samples/tour/README.md:70-73` and StubbingDemo/InOrderDemo.
- 1.2 Impact: blocks any library whose API contract includes throwing —
  cajeta-unit's `thenThrow`, `InOrder` failure path, and `AssertionFailure`
  demos are all gated on this.

## 2. Use cases

- 2.1 As a developer, when a method inside a dependency `.cja` throws and my
  code catches that exception type, then the catch runs — identically to the
  same code compiled in one unit.
- 2.2 As the cajeta-unit tour, when 2.1 holds, then the thenThrow /
  InOrder-failure / AssertionFailure demos are un-gated.

## 3. Acceptance

- 3.1 Regression test: throw in a separately-archived `.cja`, catch in the
  consumer, across JIT and AOT paths; unit-tour gated demos enabled and green.

---

**CLOSED — verified fixed on cajeta 0.14.0 (8ca5b362), 2026-08-01.** Re-ran this
spec's repro against a freshly built 0.14.0 compiler; the defect no longer
reproduces. Archived per td-project-workflow (spec -> archive, INDEX row dropped).
