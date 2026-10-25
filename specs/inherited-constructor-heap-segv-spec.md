# inherited-constructor-heap-segv — a subclass with no constructor of its own, whose base declares one, is unusable after `heap` (defect spec)

> Status: **draft**, filed 2026-10-25 by benchmark-reference unit 17 (cajeta-ml
> bench), reproduced with a probe on v0.46.3.

## 1. Definition
A class `B extends A` where `A` declares a constructor and `B` declares none
compiles; `heap B()` returns an object whose first field access segfaults.
Declaring an explicit constructor on `B` (even one that only calls `super()`)
fixes it. Every benchmark class in `cajeta-ml/bench` and `cajeta-xgboost/bench`
carries an explicit constructor for this reason.

## 2. Use cases
- **2.1** As a developer, when I write a subclass without a constructor and
  the base has one, then either the implicit constructor runs the base's
  no-argument constructor and the object is usable, or the compiler reports
  by name that the base has no no-argument constructor to inherit.
- **2.2** As the test suite, then a JIT test allocates such a subclass with
  `heap`, reads a base field and a subclass field, and asserts both.

## 3. Acceptance
The explicit constructors in the two bench projects can be deleted and the
smokes stay green.
