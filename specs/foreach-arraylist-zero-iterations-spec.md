# foreach-arraylist-zero-iterations — `for (v : list)` over an `ArrayList` or a stream runs zero iterations (defect spec)

> Status: **draft**, filed 2026-10-25 by benchmark-reference unit 6 on
> v0.46.3, measured while writing `collection/arraylist-iterate`.

## 1. Definition
`for (int32 v : list)` with `list` an `ArrayList<int32>`, and
`for (v : list.stream())`, both compile and both run zero iterations: the
body never executes and no diagnostic is raised. The same loop over a `T[]`
iterates. The benchmark walks by index until this is fixed and says so on
its page.

## 2. Use cases
- **2.1** As a developer, when I `foreach` over any stdlib collection or a
  `Stream`, then the body runs once per element, in order.
- **2.2** As the test suite, then JIT tests cover `foreach` over `ArrayList`,
  `LinkedList`, `HashSet`, a `HashMap`'s `keys()` stream and an
  `ArrayStream`, asserting element counts and sums; and a test asserts that
  an iterable type without the protocol is rejected by name rather than
  iterated zero times.

## 3. Acceptance
`collection/arraylist-iterate` moves to `foreach`, `lang/loop-foreach-arraylist`
reports the same sum as `lang/loop-foreach-array`, and the smokes pass.
