# LinkedList class-typed pop SIGSEGV — spec (defect)

Found by the tour-quality stdlib remediation (unit 3, LinkedListDemo
rewrite, 2026-07-31).

## 1. Definition

- 1.1 `LinkedList<String>` (any class-typed `T`) crashes on `popTail()` /
  `popHead()`: 12-line repro — `addTail("alpha"); addTail("beta");` then
  `popTail()` SIGSEGVs (fault addr near-null) on the 0.12.0 toolchain.
  `addTail`/`tail()`/`count()` work; `LinkedList<int32>` pops work.
- 1.2 No code in the repo ever instantiated a class-typed LinkedList before
  (grep: only a doc-comment) — the gap was never exercised.
- 1.3 Suspected shape: pop hands the element back while dropping the owning
  node (`add` moves the value in via `heap LinkedListNode<T>(#value)`);
  for class `T` the node's drop frees the element before/while the return
  transfers it — the returned reference dangles.

## 2. Use cases

- 2.1 As a developer, when I pop from a `LinkedList<String>` (or any class
  element), then I receive the element alive and owned per the method's
  contract, and the node is reclaimed without touching it.
- 2.2 As the tour, when 2.1 holds, then LinkedListDemo's undo-history
  scenario stores the edit descriptions themselves instead of int32 IDs
  (the current gated workaround).

## 3. Acceptance

- 3.1 Regression tests: class-typed popHead/popTail/remove round trips
  (JIT + AOT), plus a drop-chain-clean assertion; tour demo un-gated.
