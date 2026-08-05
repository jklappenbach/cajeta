# iface-downcast-generic-call — generic-returning call through an iface→iface downcast segfaults

## 1. Definition

Found 2026-08-05 implementing ml-classification-gaps U5 (cajeta-ml
`Scorers`). Calling a **generic-returning** method (`#Tensor<float64>`)
on an interface value obtained by **downcasting another interface** —
either spelling — segfaults at the call:

```cajeta
Predictor est = heap LogisticRegression(...);      // ProbClassifier extends Predictor
// (a) instanceof binding
if (est instanceof ProbClassifier pc) {
    Tensor<float64> pr = pc.predictProba(x);       // SIGSEGV
}
// (b) explicit cast
ProbClassifier pc2 = (ProbClassifier) est;
Tensor<float64> pr2 = pc2.predictProba(x);         // SIGSEGV
```

Bounding probes (all green):
- `est.predict(x)` through the base `Predictor` — a generic-returning call
  through a DIRECTLY-assigned interface — works (the old
  `iface-generic-returns` blockage noted in `Split.crossValScore`'s comment
  is stale for this shape).
- `est instanceof ProbClassifier pc` with the binding used only as a
  boolean — works.
- `ProbClassifier pc = heap LogisticRegression(...)` (direct
  class→interface assignment) then `pc.predictProba(x)` — works.

So the broken piece is the **fat pointer produced by the iface→iface
downcast**: its vtable evidently does not serve the target interface's
own method slots (the sub-interface's first own method lands on garbage).
The 2026-08-04 `instanceof-interface-lhs` fix added iface→iface answers
and bindings, but its pins apparently never CALLED a method through the
bound value with a generic return.

## 2. Requirements

- **2.1** When an interface value is downcast to another interface it
  implements (instanceof binding or cast), method calls through the result
  — including generic-returning methods — dispatch correctly.
- **2.2** A regression pin calls a `#Tensor<E>`-returning method through
  both downcast spellings.

## 3. Workaround (in use)

Enumerated **concrete-class** downcasts (`instanceof LogisticRegression c1`
→ class-dispatch `predictProba`), shipped in cajeta-ml `Scorers.score`
(rocAuc branch). Loses open-world extensibility — external estimators
cannot join the rocAuc path until this is fixed.

## 4. Reproduction

cajeta-ml @ U5: restore `instanceof ProbClassifier pc` +
`pc.predictProba(x)` in `Scorers.score` and run the suite —
`ModelSelectionTest::metricsSelectableByName` segfaults.
