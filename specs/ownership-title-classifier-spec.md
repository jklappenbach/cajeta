# Spec: One ownership-title classifier for every consumer position (`ownership-title-classifier`)

**active** — filed 2026-09-07, approved 2026-09-07 (every §5 decision resolved
with the developer in the interactive review; the amendments are marked
*Resolved*; plan in `agents/ownership-title-classifier-plan.md`). Measured basis:
the classifier survey of the working tree on 2026-09-07 (six copies of the
same shape classifier, a dozen disagreements; table in §3), the conditional
(`c ? a : b`) defect family it produced
(`ternary-local-double-free`: a local, a `#=` store, a `#` return, a call
argument and a re-assignment each had to be fixed separately, in
`27bd6c78`, `96868d70` and `b15541b7`), and probes
`tmp/probe-emit/src/probe/{T,U,V,W,X,Y}.cajeta`.

## 1. Definition

### 1.1 Purpose

Every position that consumes an expression's value has to answer one question
before it can act: **what title does this value carry?** A borrow that someone
else frees, a fresh owned value, a title moved out of a local, a title that
rides a callee's runtime flag, or a mode decided at runtime by a formal's
transfer-word bit or a slot's own-bit.

Today that question is answered by an if/else chain over AST node types
written separately at each consumer: the local declaration, the `#=` store
wrapper (`MoveExpression`), the field / element / local assignment paths in
`BinaryOpExpression`, the return statement's static checks and its flag
composition, the call-argument transfer word and its after-call reclaim, the
constructor-argument twins in `CreatorRest`, and the conditional's own per-arm
helper. Six copies. They disagree on a dozen shapes (§3), and every new shape
has to be added to each one — the conditional was added to five of them over
two days, one measured double-free at a time.

This spec defines **one classifier** with two entry points — a static shape
verdict usable before any IR exists, and a runtime flag materialised once
after the value's codegen — and migrates every consumer onto it, so a shape is
classified in one place, a disagreement is a decision recorded once, and a
consumer that needs a static answer and a runtime answer for the same node
reads both from the same source.

### 1.2 Scope

- **In:** the `TitleShape` static classification and its diagnostic label;
  the runtime flag materialisation (drop-entry flag, return-flag TLS,
  transfer-word bit, conditional phi, slot own-bit / element take); the
  consumer policy table (which answer each consumer role acts on); cast
  peeling and conditional recursion as classifier concerns; arena eligibility
  as a classifier concern; the migration of every site listed in §3; a
  measured-equivalence method (IR corpus diff per migrated site); an
  exhaustiveness test over `Expression` subclasses.
- **Out:** the ownership *model* itself (what lends, what transfers — CLAUDE.md
  §1–3 stands); the runtime protocols (the return-flag TLS, the transfer word,
  field own-bits, drop entries, the String dual-role protocol 3.4.3); the
  diagnostic codes and their user-facing text (the labels move, the codes do
  not); new syntax.

### 1.3 Non-goals

- No behaviour change on any site whose current answer agrees with the table
  in §3. Where a site's answer is measured wrong (a ⚠ cell), the change is a
  decision recorded in §5, with the probe that shows the leak or double-free.
- No attempt to make ownership statically decidable where it is runtime state
  (§7.2 discipline: a check fires only on a proven borrow, never on "cannot
  prove").

### 1.4 Constraints

- Behaviour is **measured, never reasoned**: each migrated site ships with an
  IR diff over a fixed corpus (the stdlib, the ownership test programs, the
  probe set) showing identical output modulo folded constants, plus the
  ownership suites green, plus the fleet building.
- One site per commit, in dependency order (§4), so a regression bisects to a
  site.
- Every runtime read of the return-flag TLS happens **immediately** after the
  call whose flag it is (the next call clobbers it). The classifier reads it;
  consumers never do.
- **As optimal as possible — inline, if possible** (Julian, 2026-09-07). On
  the compiler side the classifier is header-`inline` and `constexpr` where
  the language allows, dispatches on a node-kind tag in one `switch` (no
  chain of `dynamic_pointer_cast`), takes no `std::function`, and allocates
  nothing on a codegen path. In the emitted code the ownership bookkeeping
  is IR where it is a few instructions — a drop-entry flag read is a load,
  arming is a store, a re-assignment is compare / branch / indirect drop /
  two stores — so a constant flag folds the branch away; a runtime call
  remains only for what genuinely needs the runtime (a virtual drop, a
  String resolve, chain validation). The emitted-instruction count per unit
  may not rise (§7).

## 2. The classifier

### 2.1 Static shape (`TitleShape classify(expr, module)`)

Peels reference casts, then classifies the expression by provenance:

| Shape | Recognised as | Static answer |
|---|---|---|
| `TextLiteral` (String / text block) | `Literal` | Borrow (static alias) |
| `Identifier` | `LocalRead{hasEntry, isParam, isTransferredParam, arena}` | Borrow of the named slot; policy may treat a transferred formal as a frame title (the Exception-ctor idiom) |
| `this` | `This` | Borrow |
| `DotExpression` | `FieldRead` | Borrow (interior view) |
| `ArrayIndexExpression` | `ElementRead` | Borrow (interior view) |
| `NewExpression` / `AggregateInitializer` | `Fresh{heap, stack, shared}` | heap → Owned; stack → StackBound; shared → Borrow |
| `ArrayLiteral` | `Fresh{heap, stack, arena}` | heap → Owned; else StackBound |
| String `+` concat | `Concat{arena}` | arena → Borrow (frame arena); else Owned |
| `MoveExpression` | `Move{inner}` | Owned when the inner is a static owner; Runtime(DE / WORD / SLOT) otherwise |
| `MethodCallExpression` | `CallResult{stance: owned / plain / view}` | any callee that emits a return flag → Runtime(TLS), whether declared `#R` or plain: a plain return may carry a title (§2.1 ride) and a `#R` return may carry a borrow (`return #= x` is its sanctioned escape) — *measured 2026-09-07: folding a `#R` arm to a constant 1 dropped the flag read in `Report::baseline`*; the `#R` declaration travels as a flag (`kOwnedDecl`) for the checks that key on it; view (`^`) → Borrow; a plain body whose every return is an interior read (`Method::returnsInteriorView`) → Borrow ONLY when the dispatch is static (static / private / final method, or final class) — through a virtual call the scan proves the base's returns and an override may ride a title out, so that stays Runtime(TLS) (*measured 2026-09-07 in Unit 4: `DynFrame sch = this.__schemaOf()` on the non-final `Table<T>` lost its flagged entry to a static Borrow*); the callee is the call's own resolution when its codegen has run (exact, overloads included), else the shallow name+arity resolution; a callee that stores no flag — a `@Native` (its forwarding body is a bare `ret`), a body-less intrinsic, a synthesized raw-IR method — → its declared stance, statically (*measured 2026-09-07: reading the TLS after one is a stale read*); an abstract or interface method dispatches to a body that stores it → Runtime(TLS); unresolvable before codegen → Runtime(TLS), so consumers classify AFTER the call's codegen; AFTER codegen a call whose own resolution is still null was lowered by an intrinsic (`Cajeta.stringSliceBorrow`, `TcpStream.connectAsyncNative` — a raw runtime call, the cajeta stub never runs, no flag stored): the consumer keeps its static mode, never a TLS read (*measured 2026-09-08 in Unit 6: 24 stale reads when the shallow answer was trusted post-codegen*). A static Owned for `#R` callees needs a signature bit "no mode-carrying return" (plan 7.2.3) |
| `CallExpression` (closure), and a method-call-shaped invocation of a function-typed local or property (`maker()`, `c.supplier()` — the declaration's former M5b rule) | `ClosureCall` | the function type decides: sret return → StackBound; a non-class return → Scalar; a class pointer → Runtime(TLS) |
| `BooleanSwitchExpression` | `Conditional{arms}` | the join of the arms: equal static answers fold; otherwise Runtime(PHI) |
| `SwitchExpression` (expression form) | `Conditional{arms}` | the join of its case arms, exactly as the conditional |
| `LambdaExpression` / `MethodReferenceExpression` | `Closure` | Owned (a fresh closure; `__cajeta_closure_drop`) |
| any scalar-typed expression (numbers, booleans, comparisons, `instanceof`, arithmetic) | `Scalar` | NoTitle |

**The classifier is total** (*Resolved 2026-09-07: there is no `Unknown`
answer*). Every expression the language can produce has a determinate
ownership answer — it reads a value someone else owns, makes a fresh one,
moves one, or its title is decided at runtime by a protocol that already
exists — so an unnamed shape is a defect in the compiler's source, not a
property of the program. The exhaustiveness test (§4.4) enumerates every
concrete `Expression` subclass and fails until each is named; a new subclass
fails the compiler's own suite until it is classified. The static entry
point's "cannot prove" answer is `Runtime` with a named flag source, never a
default, and no consumer carries a polarity of its own any more (the
`bindingTakesTitle` "unknown ⇒ owned" versus "unknown ⇒ do not reclaim"
conflation that cost a release is exactly what this removes).

The callee stance for a `CallResult` comes from a shallow, order-independent
resolution (the existing `resolveArgCalleeShallow`), never from codegen state
on the node — the owned-bind check's silence in `cajeta-llm` (§6) is what
reading `getResolvedMethod()` before codegen costs.

### 2.2 Runtime flag (`llvm::Value* titleFlag(expr)`)

For a `Runtime` answer, one i64 read, emitted right after the value's codegen
and cached on the node for the rest of that codegen:

| Source | Read |
|---|---|
| DE | `__cajeta_drop_entry_flag(entry)` of the named local |
| WORD | the enclosing function's transfer-word bit for the formal |
| TLS | `__cajeta_return_flag_get()` after the call |
| PHI | the conditional's `tern_title` phi (constant when both arms agree) |
| SLOT | the field's own-bit / `__cajeta_tail_elem_take_flag` |

Static answers materialise as `ConstantInt` 0 / 1 so every consumer can fold
its branch. The existing per-node fields (`MoveExpression::runtimeTitleFlag`,
`BooleanSwitchExpression::runtimeTitleFlag`, `MethodCallExpression`'s stashed
arg / flagged-title values) become this one cache.

### 2.3 Consumer policy

Consumers do not re-classify. Each declares its **role** and asks the policy
table for the action:

| Role | Borrow | Owned | Runtime | Scalar / StackBound |
|---|---|---|---|---|
| bind (`T x = e`) | no entry | armed entry | flagged entry | none / stack-drop |
| `#=` store (String) | resolve a copy | move the wrapper | branch resolve / move | n/a |
| `#=` store (class field / slot) | own-bit 0 | own-bit 1 | own-bit = flag | own-bit 0 |
| `=` re-assign of a binding with an entry | entry untouched (old value lives to scope exit) | release the displaced value, re-arm on the new | `__cajeta_drop_reassign` on the flag | entry untouched |
| return under `#T` | **error** OWNED_RETURN_OF_BORROW | flag 1 | flag + TITLE_MISS contract | **error** STACK_RETURN_ESCAPES / n/a |
| return under plain `T` | flag 0 | **error** FRESH_RETURN_NEEDS_TRANSFER | ride the flag | flag 0 / n/a |
| argument to plain formal | word bit 0 | class: word bit 1; String: reclaim after the call | class: word bit = flag; String: guarded reclaim | word bit 0 |
| argument to `#T` formal | **error** TRANSFER_REQUIRED (a proven borrow: field read, element read, literal, borrow-returning call, entry-less local — *Resolved 5.8*) | pass | pass | n/a |
| conditional / switch arm | 0 | 1 | flag | 0 |

Two store-role rows the table folds in (Unit 5): a `#`-declared formal
stored by bare name (`take(#String s) { this.v = s; }`) is Owned — the frame
holds that title unconditionally (no entry, the word bit is not consulted)
and storing it consumes it; and a `stack` value moved into a String or class
slot is the **error** STACK_TRANSFER (5.11), an array whose slots lend frame
locals the **error** ARRAY_SLOT_BORROWS_LOCAL (5.10). A bare name that is not
a `#` formal lends in every store role, the array slot included (5.10).

The labels for the two error rows ("a field read", "a literal", "a bare local
or formal (no `#`)", "a call returning a borrow") come from the shape, in one
table.

## 3. The sites, and where they disagree today

Rows are AST shapes, columns are the consumer sites (survey names). **B** =
borrow, **O** = owned, **RT:x** = runtime flag from x, **E** = compile error,
**—** = shape not recognised (site default). ⚠ marks a cell that disagrees
with the row's majority and is a decision in §5.

| Shape | LVD init (A8) | ternary arm (B1) | class field own-bit (C4) | tail elem (C5) | local re-arm (C8) | `#` return static (D4) | return flag (D10) | call word (E5) | `#T` check (E6) | reclaim (E7) |
|---|---|---|---|---|---|---|---|---|---|---|
| `#x` move | owned → RT | O / RT | O + RT | O / RT | move + RT | pass | O / RT | RT | skip | skip |
| `heap X()` | O | O | O | O | O | pass | n/a | O | skip | n/a |
| `stack X()` | stack-drop | B | B | B | B | E | n/a | B | skip | n/a |
| aggregate `heap` | O (default) | O | **B** ⚠ | **B** ⚠ | **B** ⚠ | pass | n/a | **B** ⚠ | — | n/a |
| String concat | O (default) | **O, no arena check** ⚠ | B ⚠ | B | B | pass | n/a | B (3.4.3) | — | O (drop) |
| call, `#R` callee | O + §4.6 error | RT:TLS | O | O | O | pass | RT:TLS | O | skip | O if String |
| call, plain class callee | RT:TLS | RT:TLS | **B** ⚠ | **`bindingTakesTitle`** ⚠ | **B** ⚠ | pass | RT:TLS | RT:TLS | E ⚠ | RT (receiver) |
| closure call | RT:TLS | RT:TLS | **B** ⚠ | **B** ⚠ | **B** ⚠ | pass | RT:TLS | RT:TLS | — | — |
| literal | B | B | B | B | B | E | n/a | B | — | — |
| identifier | B | B | B (O if `#` formal) | B | B | E | RT:DE (formals) | RT:DE / WORD | **B, no error** ⚠ | — |
| field read | B | B | B | B | B | E | RT:SLOT via `#` | — | E | — |
| element read | B | B | B | B | B | E | RT:SLOT via `#` | — | E | — |
| cast | **lvalue (A5)** ⚠ | **not peeled** ⚠ | B | B | B | **not peeled** ⚠ | peeled | not peeled | not peeled | not peeled |
| conditional | RT:PHI | RT:PHI | RT:PHI (fixed 2026-09-07) | **B** ⚠ | RT:PHI | per arm | RT:PHI | RT:PHI (class) | per arm | RT:PHI (String) |
| array literal `heap` | O (default) | **B** ⚠ | **B** ⚠ | B | B | pass | n/a | O | — | — |

Sites not in the table that carry their own two-shape rule and migrate too:
the interface-slot kind (LVD A6, BinaryOp C1: only `#x` counts as owned, so a
`heap X()` into an interface local is recorded borrowed), the closure-drop
rule (A11), the legacy `producesOwnedString` (A10, name-keyed), the
`operator[]=` word (C2: only `#x` contributes a bit), the String-array sidecar
and field-array element stores (C6, C7), and the four `CreatorRest` twins of
the call-argument sites.

## 4. Use cases

- **4.1** When a consumer needs a verdict before any IR exists (a return or
  argument diagnostic), the static shape answers Borrow / Owned / Runtime /
  StackBound / Unknown with a label, and the consumer never inspects node
  types itself.
- **4.2** When the answer is Runtime, the consumer asks for the flag once; the
  read is emitted immediately after the value's codegen and reused by every
  consumer of that node in the same codegen.
- **4.3** When both arms of a conditional classify the same way statically,
  the flag is a constant and the consumer's branch folds; no runtime test is
  emitted.
- **4.4** When a concrete `Expression` subclass has no named classification,
  the exhaustiveness test fails; the compiler never emits code for an
  unclassified shape. (*Resolved 2026-09-07: there is no Unknown answer.*)
- **4.5** When a reference cast wraps the expression, it is peeled before
  classification, at every consumer.
- **4.6** When a new `Expression` subclass is added, an exhaustiveness test
  fails until the classifier names it.
- **4.7** When a migrated site's new answer differs from its old one, the IR
  corpus diff shows it, and §5 records the decision with the probe.
- **4.8** When a `CallResult` stance is needed before codegen, it comes from
  the shallow resolver, so the same source compiles to the same verdict
  regardless of entry point or file order (§6).
- **4.9** When the migration is complete, the per-node flag fields, the
  conditional's arm helper, and the per-site shape chains are gone; grepping
  `dynamic_pointer_cast<MoveExpression>` in a consumer finds only the
  classifier.

## 5. Decisions on the disagreements (resolved 2026-09-07 with the developer)

*All of 5.1–5.7 adopted as proposed; 5.8 decided (reject); 5.9 superseded by
totality (§2.1). Every adopted change ships with a witness test that fails on
the current compiler.*

- **5.1 Aggregate `heap` initialiser** stored to a class field, a tail slot or
  a re-armed local is Owned (today: borrow → leak).
- **5.2 Closure-call result** in the same three sites is Runtime(TLS) (today:
  borrow → a `#`-returning lambda's result leaks).
- **5.3 Plain-callee call result** in the field own-bit and re-arm sites is
  Runtime(TLS), the measured §2.1 rule (today: borrow); the tail-element site
  drops `bindingTakesTitle` (unknown ⇒ owned) for the same Runtime read.
- **5.4 String concat** in the conditional arm classifier gains the arena
  check; a frame-arena concat is Borrow.
- **5.5 Cast peeling** everywhere; the `initFromLvalue` retain rule keeps a
  cast as an lvalue only when the peeled inner is one.
- **5.6 Array literal `heap`** is Owned in the conditional arm and field
  own-bit sites (today: borrow → leak).
- **5.7 Interface-slot kind** and the `operator[]=` word compose from the
  classifier (today: `#x` only).
- **5.8 Identifier argument to a `#T` formal** — *Resolved: reject a proven
  borrow.* Measured 2026-09-07 (probe `srcZ/probe/Z.cajeta`): a bare local
  that OWNS its value is already rejected today ("write `#k`"); a bare local
  that holds a BORROW (no drop entry) compiles silently, the call passes a
  transfer word of 0, and the callee's `#T` formal arms nothing — no double
  free, but a method that declared `#T` because it stores the value now
  stores a borrow that dangles once the caller's owner dies. An entry-less
  local is a proven borrow (the declaration classified it so), so rejecting it
  is within §7.2: `CAJETA_ERROR_TRANSFER_REQUIRED` naming the local and that
  it holds a borrow; the fix is a copy, a fresh value, or an owned local. A
  local with an inactive entry (a borrow-initialized local some later
  assignment may arm) keeps the existing rule: `#k` forwards its flag. Pinned
  by a fires-test and a does-not-fire test.
- **5.9 `bindingTakesTitle` (unknown ⇒ true)** — *Superseded by totality
  (§2.1)*: no consumer default survives; the accessor is deleted with the
  last consumer that read it.
- **5.10 Array literal and element store of a bare owned local** — *Resolved
  2026-09-08: LEND.* `#` only ever appears on a NAME that owns something;
  fresh values and literals have nothing to mark. An array literal is a
  fresh construction (`heap T[n]` followed by slot stores), so the binding
  owns the array with plain `=`, and each element follows the store rule
  exactly as `ArrayList.add` does: `[.., out]` and `a[i] = out` record a
  borrow, `[.., #out]` and `a[i] = #out` transfer, a literal element is a
  borrow of static storage, a `heap X()` element is owned by the slot. Today
  the literal is the one position that moves a bare name silently (the
  corpus tool's `Command` use-after-free, plan 5.1.4); that ends. The one
  idiom the rule changes, `String[] r = [a, b]; return #r;` with owned
  locals, gets a compile error: an array whose slots borrow frame locals may
  not escape the frame (`CAJETA_ERROR_ARRAY_SLOT_BORROWS_LOCAL`, fix-it
  `[#a, #b]`). The store site records "slot borrows local X" on the array's
  field; a `#` return, a `#` argument or a `#=` store of that array checks
  the set. Two refinements from building it (Unit 5, 2026-09-08): a String
  slot never borrows — resident String slots always own, so a lent String
  is resolved into the slot's own copy (a shared stake past 256 B), exactly
  as a String field store does, and String arrays are exempt from the
  escape check; and only a local that PROVABLY owns (a static entry) is
  recorded — a runtime-flagged local (`Class<?> c = registryAt(i); arr[j] =
  c; return #arr;` in `cajeta.reflect.Class`) is the borrow-collecting
  idiom, and the callee's lend is the programmer's assertion there, as it is
  for any plain return.
- **5.11 Transfer of a `stack` value** — *Resolved 2026-09-08: compile
  error in every transfer position, never a promotion or a copy.* `stack`
  is a placement promise; promoting at the transfer would insert a malloc
  exactly where the programmer asked for none, and a "shallow copy" of a
  value with owned fields is a move-construct that leaves the still-in-scope
  original hollow. The legitimate escape already exists with zero copies:
  a plain `T` return of `stack X(...)` lands in the caller's frame (sret /
  NRVO). So `#x` of a StackBound answer into a retaining position — a `#T`
  return (`CAJETA_ERROR_STACK_RETURN_ESCAPES`, today), a `#=` store into a
  field or slot, a `#T` argument — is rejected, with the fix-it naming both
  alternatives ("construct with `heap`, or return it by value"). A `#=`
  bind to another local in the same frame is not a transfer out of the
  frame and stays legal. Allocation-site promotion ("this value is
  transferred, so allocate it on the heap from the start") is the
  principled ergonomic alternative — zero copies, decided at compile time —
  but it changes what `stack` means and is its own spec item, not part of
  this plan. Witness: Julian's `TransferOfBorrowTests` DISABLED test (a
  stack instance retained by a field of an escaping heap object, 2026-09-07).
- **5.12 The indexed store is the sink model** — *Resolved 2026-09-08 with
  Julian: `m[k] = v` lends; `m[k] = #v` and `m[k] #= v` carry the value's
  title; an indexed store must not force the transfer.* `HashMap.operator[]=`
  took `#K key, #V value` "to forward the caller's word to `put`", but a
  `#`-declared formal is the frame's own title, a constant 1, so every
  indexed store adopted whatever it was handed (the audit corpus found the
  tour's `freq[w] = …` on a borrowed key, 8.2.1). The forwarding the doc
  meant is what a PLAIN formal's `#` does — it forwards the word bit that
  arrived — so the fix is the signature: `operator[]= (K key, V value) {
  this.put(#key, #value); }`. The lowering already composes the word from
  each argument's classified title (`m[k] = v` → 0, `m[k] = #v` → the
  value's flag, `m[k] #= v` → its mode, a fresh value → 1). Consequences:
  `m[k] = ownedCall()` is now `CAJETA_ERROR_OWNED_RESULT_NEEDS_TRANSFER`
  like every plain receipt of a `#R` result (spell `m[k] #= …`), and a bare
  owned local at its final use in an indexed store gets the last-use
  advisory the call site already gives. Fleet sites that relied on the
  adoption are found by the compiler, not by a sweep.
  The same principle in `RedBlackTree`: the registry (`nodes`) owns every
  node and every link is a borrow — the developer's "we don't need
  parent-to-child ownership" — so every link store is `#=`, mode-carrying,
  and no local ever owns a node (a fresh node goes straight into the
  registry; the frame keeps a borrow from it). A `#=` from a name that still
  owns would move the title into the link, and a `#=` from a name the
  registry add had spent is a use after move: both rejected, both measured.

- **5.13 Closures are titled like class values** — *Decided 2026-09-09:
  Julian asked for the closure-argument leak to be fixed; the sink model
  is the rule already in force for every other keeper.* A lambda literal is
  a fresh owner (its record and capture block are `__cajeta_alloc`ed and
  live-set tracked). Measured before this unit: passed straight to any
  formal it was never dropped — two live objects per call — because a
  function-typed argument never rode the transfer word and a function-typed
  formal never got a drop entry; a closure held in a local was dropped by
  the local. The rule: a function-typed formal takes part in the hidden
  transfer word exactly as a class formal does, and the callee arms a
  `__cajeta_closure_drop` entry from its bit. So `forEach((x) -> …)` moves
  the literal into the callee, whose formal drops it on exit; `forEach(p)`
  lends `p` and the local keeps dropping it; `forEach(#p)` moves the local.
  A keeper stores with `#=` (the ArrayList model — `this.pred #= pred`
  records the mode that arrived, a field ownership bit like any class
  field, and the holder's drop releases an owned closure); a plain `=` of
  a function-typed parameter into a field is
  `CAJETA_ERROR_CAPTURED_BORROW_PARAM` like any class parameter. Callers
  do not change spelling: literals transfer by being fresh, names lend. A
  FACTORY that hands its function-typed formal on to a keeper forwards it
  with `#fn` (`return heap FilterStream<T>(this, #pred)`), exactly as a
  class formal is forwarded: the keeper takes the title that arrived and
  the factory's entry is deactivated by the move. Handed on plainly, the
  keeper records a borrow and the factory's exit frees the record under it
  (measured: cabra's WebFront through `Middleware.of`).
- **5.14 A store of a borrow of the slot's own value keeps the slot's
  title** — *Decided 2026-09-09.* `slot = b` where `b` borrows the value the
  slot already owns cannot revoke the slot's title: nothing changed
  hands. Measured before this unit: an element slot cleared its bit and
  leaked the value (`ParallelDriver.foldWorker`'s `partials[slot] =
  acc`), and a class field released the displaced value first and then
  stored a borrow of freed memory (a use-after-free). The displaced
  release fires only when the object differs, and the bit written keeps
  the old bit when it is the same object. `foldWorker` itself spells the
  re-store `#=`: the identity case records the borrow it holds (and the
  slot keeps its title), a fresh accumulator moves its title into the slot. Two
  more rules the parallel merges needed (2026-09-09): a LOCAL re-assigned
  with `#=` from a slot forwards the slot's bit as its declaration form
  does (a borrow slot records a borrow, never a panic), so a merge that was
  handed back the slot's own value can take the slot (`if (acc ==
  partials[ci]) acc #= partials[ci]`); and a drop entry lends its title
  only while it still describes the local's current object — a borrow
  re-assign leaves it on the displaced value by design, and `#x` / `#= x` /
  a return read 0 for the new object instead of forging a title (the
  displaced value is still freed at scope exit).

## 6. Related finding: the owned-bind check was never order-dependent

`CAJETA_ERROR_OWNED_RESULT_NEEDS_TRANSFER` was recorded on 2026-09-06 as
"order-dependent" because a hand-built `--emit=cja` of `cajeta-llm` rejected
`SafetensorsFile.cajeta:199,214,221,228` while `run-tests.sh` built the same
sources clean. Measured 2026-09-07: the check fires on a first-generation
generic call in isolation (probe `Y.cajeta`), and `run-tests.sh` line 115
exports `CAJETA_OWNED_BIND=warn` (line 116: `CAJETA_CAPTURED_BORROW=warn`),
which demotes both checks to notes that the script's `>/dev/null` discards.
The same build with the notes captured reports **451 owned-bind and 9
captured-borrow sites** across `cajeta-llm` (main and test sources). The
check is deterministic; the harness was in migration mode. *Resolved the
same day:* the 388 owned-bind and 5 captured-borrow unique sites are migrated
to `#=` and `run-tests.sh` defaults both switches to `error` (`cajeta-llm`
`0a47d6d`, suite 360/0/1 on both legs), so the fleet oracle in §7 is
meaningful from the first unit. The reassign-leak family the same probes
exposed (an owned binding re-assigned an owned value leaked the new one) is
fixed in cajeta `b15541b7` (`__cajeta_drop_reassign`; the policy row in §2.3
describes it), so the re-assign consumer migrates onto a correct baseline.

A second, unrelated defect surfaced from the same probes: two sibling files
each declaring a nested `static class Cell` bind the bare name `Cell` inside
one outer to the other file's class (`W.cajeta` + `X.cajeta` in one root:
`W.sinkCell(c ? heap Cell(8) : r.a)` fails `NO_MATCHING_OVERLOAD` with
candidate `sinkCell(probe.W.Cell)`; `W` alone compiles). That is the
nested-class short-name binding family, recorded there, not here.

A third surfaced writing Unit 4's tests (2026-09-07): an interface local bound
from an interface-typed field read (`Shape v = b.s`) aliased the owner's own
fat-pointer body and still pushed an active `__cajeta_iface_drop` entry on it,
so the borrowing frame freed the owner's square (`free(): double free
detected` at the owner's drop). The declaration's interface-entry rule was
"every interface local", with the kind word expected to make a borrow's drop a
no-op — true for a body the local builds itself, false for a body it aliases.
Under the classifier the rule is the policy answer: a Borrow pushes no entry.
Pinned by `DeclarationOwnershipTests.interfaceLocalFromFieldReadBorrows`.

A fourth, from the first Unit 4 sweep: `HashMapEntryStream.next()` built its
yielded pair with the OWNING constructor from the map's own slot key and
value (`heap Pair<K,V>(k, v)`). `Pair(#K, #V)` is a contract — the stores set
the own-bits regardless of the transfer word — so every entry pair claimed
the map's Strings. It never showed because `Optional<Pair<K,V>> o #= s.next()`
armed the local's entry from a constant 0 (the `#=`-of-an-sret-call rule), so
no pair was ever dropped: a leak per entry. The classifier's StackBound answer
arms the Optional's stack drop, the pair drops, and the map's Strings were
freed under it (PluginEmitter's `ActionResult.outputs`). The fix is a
borrowing constructor, `Pair(K, V, boolean borrowed)`, whose plain formals let
`#=` record what the caller tendered; the call the stream made is exactly the
shape decision 5.8 rejects once Unit 7 lands. The general lesson is the one
§1.1 states: a consumer armed correctly for the first time exposes every
upstream claim of title that was never true.

A fifth is the mirror image, and it is the reason a classifier must answer
"what title does this local HOLD" rather than "does it have an entry": the
String element store took a bare local as owned whenever the local had a drop
entry, which was a sound proxy only while borrow-holding locals had none. Once
a local bound from a plain call carries a flagged entry (armed by the callee's
flag), `names[0] = tsName` in `Exec.applyResample` took a borrowed column name
as owned and the result frame freed it under the plan node. The store now asks
`heldTitleFlag` — the entry's flag for a runtime owner, 1 for a static owner, 0
for none — which is the per-role reading of the same fact the declaration
armed the entry with. Every remaining "has an entry" test in the consumer sites
(Units 5–7) is the same latent bug and migrates the same way.

Three more from the return statement's migration (Unit 6, 2026-09-08):

- **An intrinsic-lowered call stores no return flag.** `Cajeta.stringSliceBorrow`
  and `TcpStream.connectAsyncNative` are replaced at the call site by raw
  runtime calls; the cajeta stub (when there is one) never runs. The
  classifier's shallow name+arity resolution is a pre-codegen aid only:
  AFTER codegen the call's own resolution is the truth, and a null one means
  "no flag was stored" — the consumer's static mode stands. Trusting the
  shallow answer post-codegen rode a stale TLS at 24 corpus sites; the
  declaration and store sites had 0 such reads, and the return site has 0
  again. (§2.1's CallResult row is amended accordingly.)
- **A constant title must not be armed at run time.** The declaration passed
  the classifier's constant 1 to `__cajeta_drop_set_flag` and marked the local
  a runtime owner, so `#=` from a `#`-declared native (`String.caseFold`'s
  `out`) paid a call at the bind and an entry read at every `#` return. A
  pushed entry is active already; the constant folds to nothing and the local
  is a static owner. The general rule: a Runtime *source* is only for a value
  the classifier could not decide — a policy row that promotes to Owned
  produces a constant, and every consumer must let a constant fold.
- **A String literal under a `#String` return is an adoption.** Its static
  wrapper is never in the live set, so the caller's drop is a no-op; the enum
  `toName()` idiom (`return "error";` from every arm) is sound and keeps the
  static 1. A literal ARM of a conditional stays rejected: a mixed phi would
  reach the TITLE_MISS contract with a 0. The by-value counterpart of 5.11 is
  confirmed for classes: `Cell f() { return stack Cell(i); }` compiles as an
  sret return and lands in the caller's frame — accepted, balanced, no title.

And from the call arguments' migration (Unit 7, 2026-09-08):

- **What a name HOLDS changes at a re-assignment, and nothing static says
  so today.** `Cell k = f(); if (c) { k = p; } take(#k)` hands the callee a
  title on `p`'s cell (measured, verdict 21): the entry still describes the
  displaced value — kept alive to scope exit, correctly, lends of it stay
  valid — but the name holds a borrow. Demoting the name at a borrow
  re-assign (the classifier's answer for the right-hand side, Reassign
  role) was tried and reverted: the scope's move marking is
  flow-insensitive, so one arm's borrow re-assign made every later `#=` of
  the name a rejection, which §7.2 forbids ("what the analysis cannot prove
  is allowed", pinned by `CapturedBorrowParamTests.unprovableCaptureIs-
  Allowed`). The precise fix is flow-sensitive name state at the join —
  its own spec (plan 8.2.2); the witness stays as a DISABLED test.
- **The `#T`-formal row keeps two idioms the table marked E.** A String
  literal into a `#String` formal is the adoption of §6's third finding
  (`heap SomeException("…")`); and `this` into a `#T` constructor formal is
  the consumed-receiver idiom (`heap FilterStream<T>(this, pred)`, 48
  stdlib instantiations of four stages): its word bit was 0 before the
  migration, the `#` formal adopts regardless, and a named receiver's own
  later drop is the runtime's idempotent no-op. Rejecting it would break
  the Stream API for a hazard the runtime absorbs; the language gap is a
  `#this` receiver spelling (plan 8.2.2). Everything else the row rejects —
  a field or element read, an entry-less local (5.8), a borrowed formal, a
  proven-borrow call, a `stack` value (5.11) — it rejects at the
  constructor exactly as at the call: the constructor's own check had keyed
  on "a class local with an active entry" alone, and `fromErrno(String
  detail) { return heap XException(detail); }` (11 sites) handed each
  exception a title on its caller's caller's String.
- **A `#R` callee is statically Owned when its every return is a
  kind-decidable title** (`Method::returnsStaticTitle`, an AST scan: a
  `heap` construction, an aggregate, a `heap` array literal, a
  concatenation, a String literal — never a name, a call, a conditional or
  `#= x`). That is the §2.1 amendment 7.2.3 asked for, decided without a
  signature bit: the callee's block is at hand in-module, and a `.cja`
  callee (no block) stays Runtime. Corpus: 1,484 return-flag reads and 576
  runtime entry armings folded.

And from the close-out's grep audit (Unit 8, 2026-09-08):

- **`m[k] = v` transfers, whatever its doc says.** `HashMap.operator[]=(#K
  key, #V value) { this.put(#key, #value); }` documents its `#` formals as
  the way to FORWARD the caller's transfer word to `put` — so that an
  indexed store lends when the caller lends. Measured through the
  classifier: `#key` of a `#`-declared formal is the constant 1 (Unit 3's
  row, the frame owns a `#` formal unconditionally), so every indexed store
  adopts its key and value whatever the caller passed, and the tour's
  `freq[w] = …` with a borrowed `w` is exactly 5.8's rejection. The two
  readings cannot both hold. Decided the same day: 5.12 — plain formals,
  the indexed store is the sink model; the tour's `freq[w] = …` is the lend
  it always meant.
- **A `#=` between two slots forwards; a `#=` into a local claims.** The
  element→element `#=` (`dst[i] #= src[j]`, the shift/sift primitive) has
  forwarded the source slot's bit since Unit 3; the field→field `#=`
  (`x.right #= y.left`, a tree rotation over borrow links) was still a
  claim, and the field take panicked TITLE_MISS on a borrow (measured:
  `uncaught exception (value=0x3)` in `RedBlackTree.rotateLeft` once every
  link became `#=`). Slot→slot `#=` now forwards for fields too — a borrow
  link stays a borrow, a title moves — and `T x #= slot` into a local keeps
  the claim (the Unit 3 doctrine: take a plain borrow when that is what you
  meant). One rule, stated once: `#=` records what the source holds; only a
  NAME receiving it demands that this be a title.
- **An indexed store on a class must dispatch or fail, never fall through.**
  `m[k] #= call()` had no resolved value type before the call's codegen, so
  the `operator[]=` dispatch was skipped and the store ran the ARRAY element
  path over a `HashMap` (SIGSEGV, measured). The callee's declared return
  type is the value's type (the classifier's shallow resolution), and a class
  that declares `operator[]=` but did not dispatch is an error, not an array.
- **An intrinsic lowering stores no return flag; reading the TLS after one
  is a stale read.** `File.readAllBytes(path)` is a stub declaration whose
  call site is lowered to `__cajeta_file_read_all` directly, and the
  classifier's shallow resolution found the stub AFTER that codegen and
  answered Runtime(TLS). The flag it then read was whatever the previous
  class-pointer call had left — so `ModelConfig.parse`'s `raw #=
  File.readAllBytes(path)` recorded a title when a `#R` call preceded it and
  a BORROW when a borrow-returning read did, and cajeta-llm leaked one
  config buffer per model exactly when its prefill had touched a
  `hostBuffer()` first (measured 2026-09-08: `BenchTest.loadFreeCycles-
  ReturnResidentToBaseline` 110 vs 116; bisected to Unit 3 by commit,
  then to the preceding call's stance by probe). The classifier now knows
  when a call has been generated (`MethodCallExpression::hasGenerated`): a
  null resolution after codegen is an intrinsic, and its stance is a
  CONSTANT: what the lowering recorded, else owned — an intrinsic produces
  a fresh allocation (the pre-Unit-3 `bindingTakesTitle` rule). The stub's
  own `#` is a type-resolution shape, not a stance: `Cajeta.allocBytes` and
  `System.env.get` have no declaration at all and allocate (measured on the
  corpus: answering "no flag" for them dropped the entry that frees their
  result in `Utf8.toString` and `BackendRegistry.select*`). The 8.2.2
  "intrinsic-stance table" gap stays open for a borrow-returning intrinsic,
  which this rule would over-claim exactly as before Unit 3.

## 7. Acceptance

- Every ownership suite green; the 18 `TernaryOwnershipTests` and every
  existing ownership diagnostic test unchanged.
- IR corpus diff per migrated site: identical modulo folded constants, or a
  §5 decision.
- Fleet builds (`cajeta-llm`, `cajeta-jinja`, `cajeta-logging`,
  `cajeta-unit`, `cajeta-xgboost`, `cajeta-ml`, `cabra`) compile; `cajeta-llm`
  cpu suite green.
- Emitted-instruction and runtime-call counts on the corpus before and
  after, recorded in the plan per unit, itemized by protocol call. The IR
  diff is the correctness gate. The optimality gate is EXECUTED cost
  (*Julian, 2026-09-08: "I want executed cost to be the dominant metric"*):
  what the hot path pays in instructions, calls and branches, with a
  constant flag folding the whole sequence. The emitted count is a proxy
  for that — it catches protocol calls that appear or vanish between units
  — and it is not a reason to decline an inline: a call counts as one
  emitted op while its inline body counts as many, which is backwards for
  what the program pays (5.2.3 was declined on the count and reversed on
  this rule). A unit may raise the emitted count only for protocol it
  reaches for the first time (a title recorded where a borrow was assumed),
  itemized; anything else is fixed before the unit closes. Where executed
  cost is in doubt, measure it (`perf stat -e instructions:u` on the corpus
  programs built as executables, Unit 8).
- The exhaustiveness test exists and passes.
