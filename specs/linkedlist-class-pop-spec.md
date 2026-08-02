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
- 1.2a **ROOT CAUSE CONFIRMED 2026-08-02.** `CajetaClass::fieldHasOwnershipBit`
  (`src/cajeta/type/CajetaClass.cpp:912`) returns **false** for a
  `cajeta.lang.String` field — a deliberate exclusion, rationale in the comment
  there and in §5.1.6: "String-the-class transfers by the store-site
  share/resolve machinery and its fields stay always-owned."

  So a String field carries NO ownership bit, and in `LinkedListNode<String>`:

  - `this.value #= value` (ctor) records nothing — there is no bit to set;
  - `T t #= #node.value` (pop's fused claim) decays nothing — no bit to decay;
  - the node's drop treats the String member as ALWAYS-OWNED and frees it.

  Both `#` spellings are silently inert for a String field. The element is freed
  while it is being returned.

  This explains every measurement in 1.1: user classes carry bits and survive
  (including one with String's exact 4-field layout, and one owning an array);
  String does not and dies; `tail()` works because nothing drops; and
  `ArrayList<String>` works because it never extracts-and-then-drops-the-holder.

  Reduced to a 20-line repro with no stdlib container involved —
  `miniBox*` in `test/collections/LinkedListClassPopTests.cpp`: a generic
  `MiniNode<T>` holding `T value`, stored with `#=`, extracted with `#n.value`.
  `MiniBox<String>` returns garbage; `MiniBox<Tg>` is correct.

- 1.3 Suspected shape (ORIGINAL, now DISPROVED — see 1.2a): pop hands the element back
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

## 2a. Fix direction (needs a call — §5.1.6 is ratified)

The always-owned exclusion is not an oversight; it is how String transfers
today. §5.1.6 justifies it over the two PUT spellings, both of which leave the
holder owning. What it does not cover is EXTRACTION — `T t #= #node.value` is
neither put spelling, and there is no mechanism for a String field to give up
ownership.

Two shapes fit the existing machinery:

- **(A) Share on extract.** Lower a fused claim of a String-typed field to a
  stake acquire rather than a title move: the runtime already has the
  vocabulary (`CAJ_STR_SHARED_BIT`, `__cajeta_shared_release`, and
  `__cajeta_string_drop_claimed`'s SHARED branch, which frees only on the last
  stake). Holder and extracted value then both release; the last one frees.
  Keeps §5.1.6 intact — the field stays always-owned — and makes extraction
  mean "share", which is what a String-returning accessor arguably wants.
- **(B) Give String fields an ownership bit** like any other class field,
  dropping the exclusion. Conceptually simpler and makes `#=`/`#field` mean the
  same thing for String as for everything else — but it changes a ratified rule
  and touches every String field in the stdlib.

(A) is the smaller change and the one that fits the design as written. Either
needs the full sweep: this is drop lowering.

## 3. Acceptance

- 3.1 Regression tests: class-typed popHead/popTail/remove round trips
  (JIT + AOT), plus a drop-chain-clean assertion; tour demo un-gated.
- 3.2 The bounding tests in `LinkedListClassPopTests` stay green — a fix must
  not be a blanket change to the pop path, which demonstrably works for every
  non-String element shape tried.
- 3.3 The five `DISABLED_` tests in that file are the acceptance set; enable
  them when the String path lands.
