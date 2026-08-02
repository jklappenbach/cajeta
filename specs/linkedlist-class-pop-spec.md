# LinkedList class-typed pop SIGSEGV — spec (defect)

Found by the tour-quality stdlib remediation (unit 3, LinkedListDemo
rewrite, 2026-07-31).

## 1. Definition

- 1.1 `LinkedList<String>` returns a DEAD element from `popTail()` /
  `popHead()`: `addTail("alpha"); addTail("beta");` then `popTail()`.
  `addTail`/`tail()`/`count()` work; `LinkedList<int32>` pops work.

  **RE-SCOPED 2026-08-02 (was: "any class-typed `T`").** It is String only,
  and pop only. Measured on HEAD with five probes, now permanent tests in
  `test/collections/LinkedListClassPopTests.cpp`:

  | Shape | Result |
  |---|---|
  | user class, caller SURRENDERS (`heap Tag(9)`) | **works** |
  | user class, caller LENDS (named local) | **works** |
  | user class OWNING AN ARRAY (spurious drop observable) | **works**, incl. under `MALLOC_PERTURB_` |
  | `int32` element | works |
  | String added, read via `tail()` — no pop | works |
  | bare String literal, no container | works |
  | **String popped — literal OR named local** | **fails: `size()` returns 0 / garbage** |

  The array-owning user class is the decisive one. It makes a spurious drop
  observable exactly the way String's owned `base` array does, and it
  survives — so the fault is NOT the general "T's drop frees something"
  shape the original filing assumed. Something specific to String is.

  The failure mode has also softened since the 0.12.0 report: garbage /
  zero-length rather than a SIGSEGV. Same underlying read-after-free, less
  obvious symptom.
- 1.2 No code in the repo ever instantiated a class-typed LinkedList before
  (grep: only a doc-comment) — the gap was never exercised.
- 1.3 Suspected shape (ORIGINAL, now doubtful): pop hands the element back
  while dropping the owning node; for class `T` the node's drop frees the
  element before the return transfers it.

  The evidence in 1.1 argues against this as stated — a user class owning an
  array takes the identical path and survives. What remains suspect is
  String's own representation: it carries a tagged inline/heap form
  (`lenTag`, `aux`, `base`, `cachedCpLength`, per
  `runtime/src/cajeta/lang/String.cajeta`), so a String value is not a plain
  object pointer the way `Tag`/`Bag` are. The next probe should ask whether
  the fused claim `T title #= #node.value` and the flagged return preserve a
  String's tagged form, rather than whether the node drops early.

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
- 3.2 The bounding tests in `LinkedListClassPopTests` stay green — a fix must
  not be a blanket change to the pop path, which demonstrably works for every
  non-String element shape tried.
- 3.3 The five `DISABLED_` tests in that file are the acceptance set; enable
  them when the String path lands.
