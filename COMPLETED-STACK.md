# COMPLETED — template / Tensor capture work

Newest on top. Items popped off `STACK.md` once green + committed.

- nested + supertype capture targets (capture plan 5a) — `Box<Box<int32>>` and
  `List<int32> extends Container<int32>` capture targets, exact + supertype +
  nested, with discriminating negative arms. **No production change needed** —
  already satisfied by the existing design: nested falls out of the recursive
  `toCanonical()` string compare in `__cajeta_instanceof_named`, and supertype
  works because `TemplateInstantiator` records the *instantiated* super
  (`Container<int32>`) in the RTTI `parentNames`, which the runtime walks. Added
  5 regression tests to `ReifiedCaptureTests` to lock the behavior.
- `tryAs<T>()` → `Optional<T>` (capture plan 4b/4d) — non-throwing capture:
  present on match (`.get()` is the same object), empty on mismatch and on null.
  Built on `CajetaClass::heapConstruct` + `__cajeta_instanceof_named`.
  Landed on `main` as `6e773c4d`.
- throwing capture cast + `ClassCastException` — `(T) w` capture form that throws
  on mismatch. Landed on `main` as `8ca30390`.
- reusable class-construction helpers — extracted `heapConstruct` / construction
  helpers used by the capture casts. Landed on `main` as `c2e89379`.
