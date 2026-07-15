# Ownership Transfer at Call Sites

> **AMENDED (2026-07-03, [`slice-spec.md`](slice-spec.md) §9):** with the `shared` state, `#`
> transfers an **owning stake** — formerly always *the unique* ownership, now possibly one
> stake among co-owners of an immutable slice backing. A `#`-move is **rc-neutral** (memcpy +
> source-drop deactivation, no count traffic — a codegen path distinct from copy). Everything
> below is unchanged in shape; read "ownership" as "stake." `#` remains the zero-cost escape
> path (`list.add(#s)` costs neither a copy nor a count); the explicit trichotomy is borrow
> (`=`) / move (`#`) / copy-detach (`clone()`), and **share has no spelling** — it is
> exclusively a compiler-inserted resolution.

> **Status.** Phases 1–3a (caller-side `#x` syntax, the `(#T, x)` rejection,
> and the body-side return/re-transfer escape check) are **shipped**:
> `CAJETA_ERROR_TRANSFER_REQUIRED` and `CAJETA_ERROR_BORROW_PARAM_ESCAPES`
> are thrown from `MethodCallExpression.cpp`, `CreatorRest.cpp`, and
> `Statement.cpp`. The two remaining body-side shapes — **field-store of a
> borrow** and **closure-capture of a borrow** — are deliberately deferred
> (see the end of this doc and [`BorrowSoundness`](BorrowSoundness.md)).

## Motivation

Cajeta's class-typed locals carry a drop entry tied to the declaring scope.
A function call that hands the local off to a callee — where the callee
will store it into a field, push it into a collection, or otherwise retain
the reference past the call — needs the caller's drop entry deactivated.
Without that, the original local AND the callee's retained reference both
own the same allocation; both drop chains fire at scope exit and produce a
double free.

The codebase already had one half of the answer: `MethodCallExpression.cpp` (the `#`-transfer block, ~5881+)
deactivates the caller's drop entry when the callee's formal parameter is
marked `#T`. The other half — what shape this takes at the **source** level
when both ends want to participate — is what this document specifies.

## Use cases that drive the design

Two real shapes coexist in cajeta code, and a one-sided model breaks one
of them:

**Owning containers.** `Optional<T>`, `Mutex<T>`, `Throwable`,
`SelectResult<T>`, the stream wrapper chain — these conceptually wrap a
value and hold it for their lifetime. The signature should *require*
transfer: `heap Optional(true, p)` without `#` should be an error, because
silently transferring would let a caller-side typo (using the local twice)
slip past the type system.

**Index / cache / view collections.** A `HashMap<EntityId, Entity>` used
as an *index* alongside a primary `EntityWorld` owner shouldn't force
transfer at `map.put(id, entity)`. The map indexes; the world owns. Same
`HashMap` type, same `put` signature, two ownership stories: caller
decides per-call. A callee-side `#V` here collapses one of the two
stories.

The current sweep correctly marks the owning-container ctors `#T`. It
must NOT mark `Pair`, `LinkedListNode`, `RedBlackNode`, `HashMap.put`,
etc., because those serve both stories.

## The model

> **Superseded in part (rev 2).** The two-sided model below was the original
> design: the callee declared the contract, the caller acknowledged it. It was
> revised once the consequence became clear — a signature that fixes the transfer
> mode asks the author to predict how every future caller will use the method,
> and for containers (where the same `put` is legitimately used both ways) the
> honest endpoint was to mark every parameter "either." **Transfer is now the
> caller's decision at every call site**, and signatures carry no ownership
> spelling by default. `#T` on a parameter survives as an *opt-in* must-own edge.
> Current semantics live in `MemoryModel.md` § Function signatures: the transfer
> ABI; the rejected shapes and their rationale are in the title-tracking spec
> §4.6. The rest of this document is retained for the design history and for the
> caller-side `#expr` story, which is unchanged.

- **Caller-side `#expr`** — *transfer at the use site*. The caller is saying: "I
  am surrendering this here; deactivate my drop entry." It's the symmetric partner
  of the storage-class annotations on construction (`heap` / `stack`): storage
  class on the *construction* expression, transfer class on the *use* expression.
  Both are expression-level decisions, both visible at the source line. **This is
  the whole model now** — the caller's spelling is what decides.

- **Callee-side `#T param`** — *opt-in must-own*. Retained only for a method that
  cannot function with a borrow: it stores the value somewhere that outlives the
  call and has no way to cope with the caller keeping the title. It refuses a lend
  (`CAJETA_ERROR_TRANSFER_REQUIRED`). It is no longer the way to *permit* a
  transfer — a plain formal already accepts one.

### Interaction matrix (rev 2)

| Callee formal | Caller writes | Behavior |
|---------------|---------------|----------|
| `T param`     | `x`           | **Lend** — the caller keeps the title and drops the value at its own scope exit. The callee may still store it (the store is a borrow store); if the receiving object escapes, the dangling-lend check fires. |
| `T param`     | `#x`          | **Transfer** — the caller's drop deactivates; the callee is a runtime owner. If it consumes the value (`#param` into a field / a forward / a return) the title moves on; if it doesn't, the value drops in the callee. |
| `#T param`    | `x`           | **Compile error** `CAJETA_ERROR_TRANSFER_REQUIRED` — the must-own edge refuses a lend. |
| `#T param`    | `#x`          | **Transfer** — same as row 2; the signature simply made it mandatory. |

Note what changed: row 1 no longer forbids the callee from storing the value, and
row 2 is reachable without any signature change. The callee learns which row it is
in at runtime, from a hidden per-call flag (`MemoryModel.md` § the transfer ABI).

### Why keep `#T` at all?

Discoverability, in the narrow case where it is genuinely load-bearing. A reader of
`public Optional(boolean present, #T value)` knows on the signature line that
calling this surrenders. But that is a *documentation* win, not an enforcement one,
and it is only worth the rigidity where a borrow is truly unusable — so it is opt-in
rather than the default.

## Borrow-checker rules

> **Title-tracking Unit 7 amendments.** (a) `#` is retired from every TYPE
> position — type arguments (`ArrayList<#T>`), local declarations
> (`#Type x`), and type parameters (`class C<#V>`) all error with
> `CAJETA_ERROR_TYPE_TRANSFER_RETIRED`; ownership is per-call, spelled at
> call/store sites. (b) An authored `#T` formal is a hard must-own edge in
> EVERY instantiation — the old borrow-mode dissolution is gone. (c) Rule 2
> below is retired for class-typed formals (see the rev-2 note further
> down): a plain formal is a RUNTIME owner, so `#param` forwards whatever
> flag it actually holds.

The new rule is paired:

1. **At the call site:** if formal is `#T` and the argument is not
   `#expr`-prefixed, throw `CAJETA_ERROR_TRANSFER_REQUIRED`. If formal is
   plain `T` and argument is `#expr`, deactivate the caller's drop entry
   (same machinery as today's `#T`-formal path).

2. **In the callee body:** if a formal is plain `T` (class-typed), the
   parameter is a *borrow*. The body cannot:
   - Store it into `this.field = param` (field-store).
   - Return it from a method whose return type isn't `&T` /
     borrow-shaped. (Borrow-returning multi-param check already exists;
     extends to single-param when the source is a plain-`T` param.)
   - Capture it in a returned closure / lambda that outlives the call.
   - Pass it as a `#`-marked argument to another call (that would
     transfer the borrow ownership, which the caller didn't authorize).

   Violation: `CAJETA_ERROR_BORROW_PARAM_ESCAPES` at the offending site
   in the body, with a fix-it: "mark the formal `#T` to require transfer,
   or restructure to not retain the borrow past the call."

3. **`#T` formal:** inside the body, `param` is owned. All four
   restrictions in (2) lift — store, return, capture, re-transfer
   freely. The parameter participates in the body's drop chain the same
   way an inline local does, so if the body neither stores nor transfers
   it, the parameter's drop fires at the body's scope exit.

## Grammar

The arg-expression rule extends to admit a leading `#`:

```
arguments      : '(' (argument (',' argument)*)? ')'
argument       : parameterLabel? REFERENCE? expression   # REFERENCE = '#'
```

v1 restriction: the expression after `#` must be a bare identifier
(matches today's call-site transfer logic that only fires for
`IdentifierExpression` args). `#someField`, `#arr[0]`, `#call().result`
are out of scope until the borrow checker can analyze the source of
those expressions. A clear error message points users at the named-local
shape.

## Implementation phasing

The work breaks into three independent commits, each shippable on its
own (each tightens the model without breaking valid prior code):

### Phase 1 — caller-side `#x` syntax

- Grammar: add `REFERENCE? expression` to the argument rule.
- Parser/AST: capture the REFERENCE flag on each `ParameterEntry` /
  argument node.
- Codegen: in `MethodCallExpression.cpp` (the `#`-transfer block, ~5881+) and the matching block
  in `CreatorRest.cpp` (the #67 ctor-call-site transfer), trigger the
  drop-deactivation when EITHER the formal is `#T`-marked OR the
  argument carries the caller-side `#`. Either is sufficient.
- Tests: caller-side `#x` against a plain-`T` formal deactivates the
  drop correctly (no double-free); without `#x` the source stays owned
  by the caller (the existing borrow lifetime applies).

Backwards-compatible: every existing call site (no `#` on args) keeps
working. New syntax is opt-in.

### Phase 2 — `(#T, x)` rejection at call sites

- Add the `CAJETA_ERROR_TRANSFER_REQUIRED` check at both call sites:
  if formal is `#T` and the argument isn't `#`-prefixed, throw with the
  fix-it message.
- Update every existing stdlib call site that constructs `Optional`,
  `Mutex`, `Throwable`, `SelectResult`, stream wrappers — they all need
  `#` added to the relevant argument. This is a sweep parallel in shape
  to #66.
- Tests: the matrix cases — `(T, x)` borrow; `(T, #x)` transfer;
  `(#T, x)` rejected; `(#T, #x)` transfer.

This is the breaking change. Land it as a single commit per region
(stdlib first, then user code) to keep blast radius contained.

### Phase 3a — body-side borrow-param escape check (return + transfer)

> **Retired in rev 2.** `CAJETA_ERROR_BORROW_PARAM_ESCAPES` no longer fires for
> class-typed formals. The check rested on "a plain formal is a borrow, so it has
> no title to give away" — and under caller discretion that premise is false: a
> plain formal is a *runtime* owner whose title is whatever the caller handed it.
> `#param` is now legal and forwards the flag it actually holds, so a lent value
> forwarded onward arrives lent, and nothing double-frees. The hazard the check
> was aimed at (a borrow escaping past its owner's scope) is now caught where it
> actually occurs — at the escape, by the dangling-lend check — rather than by
> banning the forward. Non-class formals keep the old rule.

- Extend the existing `BORROW_RETURN_*` checks to fire when a plain-`T`
  parameter flows into:
  - A return statement against a `#T`-return signature (callee promises
    ownership transfer but returns a borrow it doesn't own).
  - A `#`-prefixed argument to another call (callee claims to transfer
    a value they don't own).
- Tests: each escape shape reports
  `CAJETA_ERROR_BORROW_PARAM_ESCAPES` with the fix-it. The borrow
  pass-through shapes (plain `T` return + plain `T` param + `return p;`,
  and `#T` formal + `#x` transfer) continue to work.

Phase 3a catches the two escape shapes whose alternative — silently
accepting them — produces double-free or use-after-free at runtime far
from the offending source line. The remaining two (field-store of
borrow, closure-capture of borrow) are deliberately deferred to a
future phase pending the reference / lifetime feature that would let
the language reason about whether the storing object's lifetime is
bounded by the borrow's lifetime.

### Field-store of borrow: deliberately unchecked (deferred)

> **Partly addressed in rev 2.** The field store itself is still allowed — and the
> reasoning below (index / cache collections legitimately hold values owned
> elsewhere) is exactly why containers now carry no ownership spelling at all. What
> changed is that the *store* is now explicitly a borrow store (it does not silently
> take ownership), and the escape it enables is caught: if the storing object then
> leaves the method holding a lend of a dying local, that is
> `CAJETA_ERROR_DANGLING_LEND`. The general cross-procedure case remains deferred.

Field-store of a plain-`T` parameter — `this.f = param;` inside a
ctor or setter — is **not** rejected. The pattern is the load-bearing
form for index / cache / view collections: a `HashMap<K, V>` used as
an index alongside a primary owner stores references to values whose
lifetime lives elsewhere. Forcing every such collection's `put`,
`add`, and ctor to mark `#K`/`#V` would collapse that use case (the
same map type can't serve both owning-store and indexing).

The current rule is: the caller decides per call site. Calling
`map.put(k, v)` borrows (caller still owns `k`, `v`); calling
`map.put(#k, #v)` transfers. Phase 1's caller-side `#x` machinery
handles both correctly, and Phase 2's `(#T, x)` rejection still fires
for callees whose formals genuinely demand transfer (owning wrappers
like `Optional`, `Mutex`, exception ctors).

The unsoundness window — passing plain `k` while the map outlives
`k`'s primary owner — is the same one Rust closes with lifetime
annotations. Cajeta will close it the same way once reference types
land; until then, the safety burden sits with the caller exactly
where it does in C and pre-borrow-checker code.
See [`BorrowSoundness`](BorrowSoundness.md) for the interim static
lint and debug-build runtime checks that target this gap without
waiting on reference types.

### Closure-capture of borrow: deferred to the lifetime work

`return () -> useParam(borrowedParam);` — capturing a borrow in a
returned closure that outlives the call — is the same lifetime
question in closure form. The existing `CAJETA_ERROR_BORROW_ESCAPE`
already catches returned closures with borrow captures from
function-typed locals; the parallel for borrow PARAMETERS is part
of the same future work as field-store. [`BorrowSoundness`](BorrowSoundness.md)
covers the same lint + debug-runtime strategy for the closure shape.

## Backwards compatibility

Phase 1 alone is purely additive — no existing code breaks. Phase 2
breaks code that constructs `#T`-formal types without the corresponding
`#x` argument; the compile error is unambiguous and the fix is
mechanical (add `#` at the argument). Phase 3 catches new classes of
bugs that previously slipped through to runtime; it may break code that
was silently relying on a stored-borrow happening to live long enough.

The phasing lets a project upgrade incrementally:

1. Pull in Phase 1 — gain expressive transfer syntax with no breakage.
2. Add `#x` at known transfer sites (warnings can guide this).
3. Pull in Phase 2 — call-site contract becomes enforced.
4. Pull in Phase 3 — body-side promise becomes enforced.

## Recovery of the per-call ownership story

Restating the use case the design serves:

```cajeta
EntityWorld world = heap EntityWorld();        // primary owner
HashMap<EntityId, Entity> byTransform = ...;   // secondary index
HashMap<EntityId, Entity> byRenderable = ...;  // another secondary index

for (Entity e : world.spawned()) {
    // Index without transferring ownership — world still owns e:
    byTransform.put(e.id, e);
    byRenderable.put(e.id, e);
}

// Later, transferring the same entity into an `Optional<Entity>` slot
// genuinely takes ownership (the world surrenders this one):
Entity claimed = world.remove(id);
Optional<Entity> held = stack Optional<Entity>(true, #claimed);
```

`HashMap.put(K, V)` is declared with plain `K`, `V` — both index calls
above borrow. The `Optional` ctor is declared with `#T` — the `#claimed`
at the call site is both syntactically required (per the matrix) and
semantically correct (the world is handing this entity off; the
`Optional` is the new owner).

Without the two-sided model, you'd either lose the index story (callee
forces transfer) or lose the contract enforcement (callee can silently
keep a borrow). Both halves earn their place.

## Open design questions

1. **`#expr` for non-identifier sources.** v1 restricts caller-side `#`
   to bare identifiers, matching today's IdentifierExpression-only
   transfer machinery. Should `#arr[i]`, `#this.field`, `#someCall()`
   work? Probably yes for the field shape (it's a transfer FROM the
   field), with a corresponding deactivation of the field's owned-ref
   tracking; the array-element and call-result shapes need more
   thought. Deferred to a follow-up.

2. **`#T` on the return position.** The grammar already supports `#T`
   on returns (ownership transfer from callee to caller). Symmetrically,
   should there be a caller-side annotation at the receiving end? E.g.
   `#Foo x = somefn();` to acknowledge "I'm taking ownership of what
   somefn returned." Currently the receiving local just registers a
   fresh drop entry. Adding caller-side acknowledgement would be
   symmetric but probably not load-bearing unless a future feature
   benefits from it.

3. **`#T` and templates.** When the formal is `#T` where T is a
   template parameter, the requirement should apply only when the
   instantiated T is class-typed (transferring a primitive is a no-op).
   The codegen path naturally handles this — the deactivation is gated
   on the local having a drop entry, and primitive locals don't — but
   the rejection in Phase 2 needs the same gate so primitive
   instantiations don't get spurious `CAJETA_ERROR_TRANSFER_REQUIRED`.

## See also

- [`BorrowSoundness`](BorrowSoundness.md) — interim lint + debug-runtime
  detection for the deferred field-store and closure-capture shapes.
- [`MemoryModel`](MemoryModel.md) — the broader borrow / ownership /
  drop chain model this extends.
- [`FieldOwnership`](FieldOwnership.md) — how fields participate in
  the drop chain (relevant to the body-side escape check).
- `MethodCallExpression.cpp` (the `#`-transfer block, ~5881+) and `CreatorRest.cpp` — the
  existing call-site transfer machinery this builds on.
