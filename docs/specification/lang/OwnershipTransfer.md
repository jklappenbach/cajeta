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
> `Statement.cpp`. Field-store of a borrow is no longer deferred: it is
> rejected by `CAJETA_ERROR_CAPTURED_BORROW_PARAM` (spec §4.2). One body-side
> shape remains deliberately deferred — **closure-capture of a borrow** (see
> the end of this doc and [`BorrowSoundness`](BorrowSoundness.md)).

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

**Owning containers.** `Pair<K, V>`, `JsonValue.setArray`/`setObject`,
`ActionResult.output` — these hold the value in storage that outlives the calling
frame, and cannot cope with the caller keeping the title. The signature
*requires* transfer: `heap Pair(k, v)` without `#` is an error, because
silently transferring would let a caller-side typo (using the local twice)
slip past the type system. Wrappers that merely hold a value for the
caller's convenience — `Optional<T>`, `Mutex<T>` — do not need that
rigidity, and their ctors take plain `T`.

**Index / cache / view collections.** A `HashMap<EntityId, Entity>` used
as an *index* alongside a primary `EntityWorld` owner shouldn't force
transfer at `map.put(id, entity)`. The map indexes; the world owns. Same
`HashMap` type, same `put` signature, two ownership stories: caller
decides per-call. A callee-side `#V` here collapses one of the two
stories.

> **The 0.15.0 uniform-`#K`/`#V` experiment was itself reversed.** For one
> release the stdlib declared every collection key and element parameter
> `#K`/`#V`; that is no longer the language. Collections are not containers —
> they can be *treated* as containers by a developer who elects to transfer in,
> but they are not designed to own by default. `HashMap.put`, `HashSet.add`,
> `Cache.put`, `BPlusTree.put`, `RedBlackTree.put`, `Heap.push`, the
> `ArrayList` mutators and the `LinkedList` mutators all carry plain `K`/`V`
> formals again, so `map.put(k, v)` lends and `map.put(#k, #v)` transfers —
> the caller decides per call, exactly as this section describes. Genuine
> containers, whose storage outlives the caller's frame, still declare `#`
> formals and are correct as they stand: `Pair`, `HashMap.operator[]=`,
> `ActionResult.output`, and `JsonValue.setArray`/`setObject` (the DOM owns
> its children). Code that uses a collection *as* such a container — the
> `DnsCache` entry store, `ActionResult.outputsMap` — spells the ownership
> at the call site instead: `this.store.put(#key, #entry)`.

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
`public Pair(#K first, #V second)` knows on the signature line that
calling this surrenders. But that is a *documentation* win, not an enforcement one,
and it is only worth the rigidity where a borrow is truly unusable — so it is opt-in
rather than the default.

## Borrow-checker rules

> **Title-tracking Unit 7 amendments.** (a) `#` is valid only on a FORMAL and
> on a RETURN type. It carries no meaning in any other type position — type
> arguments, local declarations and type parameters all error with
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

### Lifetime elision: why a multi-parameter free function cannot return a borrow

A borrow-returning function does not say whose memory the result points into —
it is *inferred* from the parameters. With exactly one reference parameter the
inference is forced: the result can only borrow from that one, so its lifetime
is the argument's and the caller already holds it. That is cajeta's whole
lifetime-elision rule, and it is why the single-parameter case needs no
annotation.

With more than one reference parameter the inference has no unique answer.
`Str pick(Str a, Str b)` might return `a`, might return `b`, and the compiler
cannot tell which — so it cannot tell the caller which argument must outlive
the result. Rather than guess, or demand explicit lifetime annotations, cajeta
rejects the shape (`CAJETA_ERROR_BORROW_RETURN_MULTI_PARAM`):

```cajeta
// error: no single parameter's lifetime is implied
public static Str pick(Str a, Str b) { ... }

// fine — one reference parameter, the lifetime is inferred from it
public static Str first(Str a) { ... }

// fine — the result is a TITLE, so it borrows from nothing
public static #Str join(Str a, Str b) { ... }
```

The fix is almost always the third form: a function combining two inputs is
producing a new value, not lending one of them back. The `#` goes on the
RETURN type; for a type parameter the spelling is `#T`. There is no `#?` —
`?` is the wildcard sentinel, not something you can write.

Value returns are exempt. An sret-constructed result (`return stack
Optional<R>(...)`) is copied into the caller's slot and borrows from nothing,
so `Tasks.withTimeout(Duration, Task<R>) -> Optional<R>` is legal.

## Grammar

The arg-expression rule extends to admit a leading `#`:

```
arguments      : '(' (argument (',' argument)*)? ')'
argument       : parameterLabel? REFERENCE? expression   # REFERENCE = '#'
```

`#` may prefix **any** argument expression — a bare identifier, a field read
(`#this.field`), an element read (`#arr[0]`), a call result (`#make()`), or
an unnamed temporary such as `#("k" + i)`. Nothing has to be hoisted into a
named local first. The drop-tracking follows the identifier, field and
temporary shapes precisely; `#arr[i]` where the element is itself a borrow
is a known defect — `#arr[i]` moves out of a borrowed element, which
double-frees — and
is still under work.

## Implementation phasing

The work breaks into three independent commits, each shippable on its
own (each tightens the model without breaking valid prior code):

### Phase 1 — caller-side `#x` syntax

- Grammar: add `REFERENCE? expression` to the argument rule.
- Parser/AST: capture the REFERENCE flag on each `ParameterEntry` /
  argument node.
- Codegen: in `MethodCallExpression.cpp` (the `#`-transfer block, ~5881+) and the matching block
  in `CreatorRest.cpp` (the #67 ctor-call-site transfer), trigger the
  drop-deactivation when the argument carries the caller-side `#`. A `#T`
  formal does not itself deactivate anything — it only rejects an argument
  that lacks `#` (`CAJETA_ERROR_TRANSFER_REQUIRED`), after which the
  caller's `#` performs the transfer. The rule is asymmetric: `#` at the
  call site is always sufficient, and a `#T` formal is never sufficient
  on its own.
- Tests: caller-side `#x` against a plain-`T` formal deactivates the
  drop correctly (no double-free); without `#x` the source stays owned
  by the caller (the existing borrow lifetime applies).

Backwards-compatible: every existing call site (no `#` on args) keeps
working. New syntax is opt-in.

### Phase 2 — `(#T, x)` rejection at call sites

- Add the `CAJETA_ERROR_TRANSFER_REQUIRED` check at both call sites:
  if formal is `#T` and the argument isn't `#`-prefixed, throw with the
  fix-it message.
- Update the stdlib call sites that construct a wrapper whose ctor
  actually requires transfer. As shipped that is `Throwable(#String
  message)` alone — `Optional(boolean present, T value)`,
  `Mutex(T initial)` and `SelectResult(int32 index, T value)` all take
  plain `T` and carry the caller's mode through `#=`, so they need no
  `#` at the call site (a string literal or other fresh rvalue passes
  plain even to a `#T` formal).
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

### Field-store of borrow: rejected (`CAJETA_ERROR_CAPTURED_BORROW_PARAM`)

> **Partly addressed in rev 2.** The field store itself is still allowed — and the
> reasoning below (index / cache collections legitimately hold values owned
> elsewhere) is exactly why containers now carry no ownership spelling at all. What
> changed is that the *store* is now explicitly a borrow store (it does not silently
> take ownership), and the escape it enables is caught: if the storing object then
> leaves the method holding a lend of a dying local, that is
> `CAJETA_ERROR_DANGLING_LEND`. The general cross-procedure case remains deferred.
>
> **Superseded by Unit 3's capture check.** For the common shape the store is
> now *rejected*, not merely loud: `this.f = param`, where `param` is a plain
> (non-`#`) class-typed formal and the destination is a direct `this.`-field or
> `this.`-element slot, is the hard error `CAJETA_ERROR_CAPTURED_BORROW_PARAM`
> (spec §4.2). `CAJETA_WARN_PLAIN_RETAIN_STORE` remains for the wider
> runtime-conditional-owner shapes that check does not reach — a non-`this`
> receiver, a nested path such as `this.head.prev = param`, or a flagged local
> rather than a formal as the source. That case is not the
> legitimate indexing pattern below — it is a store that borrows while the armed
> drop entry frees `param` at callee exit, leaving the field dangling on exactly
> the calls that transferred. It was the most recurring use-after-free family in
> the stdlib. The fix is `this.f #= param`, which records whatever title the caller
> actually handed over, so one method body is correct for both kinds of call; a
> deliberate borrow store spells `this.f = param.clone()` or keeps the plain store
> and stays quiet when the source cannot be owned. Element slots get the same
> treatment (`this.data[i] #= v`, per-slot bits) — see
> [`MemoryModel`](MemoryModel.md). The deferred case below is unchanged: a plain
> store of a *statically borrowed* value whose source outlives nothing in
> particular is still the caller's burden until reference types land.

Field-store of a plain-`T` parameter — `this.f = param;` inside a
ctor or setter — is **rejected**: `CAJETA_ERROR_CAPTURED_BORROW_PARAM`
(spec §2.4, §4.2), thrown from `Scope::rejectCapturedBorrowParam`. Three
spellings are correct instead: store with `#=`, which records whichever mode
the caller sent so one body serves a lend and a transfer (the sink model
§2.3 — how `Optional` and `ArrayStream` are written); declare the formal `#T`
to demand ownership outright (§2.4); or copy with `this.f = param.clone()`.
The check is deliberately narrow (§7.2): it fires only when the source is a
plain formal of a title-bearing class type — directly, or through a
straight-line local — stored by a direct `this.field = p` or
`this.slots[i] = p`. A nested path such as `this.head.prev = p` writes into
another object's field and stays legal.

> **How the stdlib collections spell it.** Collection key and element
> parameters are plain `K`/`V`: `HashMap.put`, `HashSet.add`, `Cache.put`,
> `BPlusTree.put`, `RedBlackTree.put`, `Heap.push`, the `ArrayList` mutators
> (`add`, `insert`, `appendAll`, `set`, `operator[]=`) and the `LinkedList`
> mutators (`add`, `addFirst`, `addTail`, `addHead`). `map.put(k, v)` lends;
> `map.put(#k, #v)` transfers; both are legal and the call site decides. A
> per-entry title bit records which one an entry got. Wrapper ctors are plain
> as well — `Optional(boolean present, T value)`, `Mutex(T initial)`.
>
> The alternative — uniform `#K`/`#V`, shipped for one release as
> uniform-transfer-semantics (0.15.0) — was weighed and reversed. The argument
> for it was that an entry whose ownership is decided at the call site has to
> be *recorded*, and then teardown, eviction, replace, rehash and remove all
> branch on that bit; the bit produced a double-free/leak family (`Cache`
> evicting a borrowed value; `HashMap.remove` displaced-releasing one) that no
> static check could sort out. Owning uniformly removes the branch, but it also
> removes the indexing story, and the language owner's ruling is that
> collections are not containers: they can be *treated* as containers by a
> developer who opts to transfer in, but they are not designed to own by
> default. The per-entry bit stays, and the branches with it.
>
> Genuine containers — storage that outlives the caller's frame — do still
> require `#`, and are correct as declared: `Pair`, `HashMap.operator[]=`,
> `ActionResult.output`, and `JsonValue.setArray`/`setObject`, where the DOM
> owns its children. A collection standing in for one (the `DnsCache` entry
> store) elects the same ownership at its call sites.
>
> The residual exposure is a lifetime one: a short-lived borrow parked in a
> longer-lived collection dangles once its owner's scope exits, and **no
> compiler check catches it today** ([`MemoryModel`](MemoryModel.md) §1.7).
> That is the lifetime tracker's job.

The unsoundness window for plain user-class field stores — parking
`k` in something that outlives `k`'s primary owner — is the same one
Rust closes with lifetime annotations. Cajeta will close it the same
way once reference types land; until then, the safety burden sits with
the caller exactly where it does in C and pre-borrow-checker code.
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
above borrow. The `Optional` ctor is plain `T` as well, so the `#claimed`
is the caller electing transfer rather than obeying the signature — and it
is the semantically correct election (the world is handing this entity off;
the `Optional` is the new owner). Against a `#T` formal such as
`Pair(#K, #V)` the same `#` would additionally be required.

The index above stores borrows, which is legal and intended — but it is
the caller's job to keep `world` alive for as long as the indexes are read.
A short-lived borrow parked in a longer-lived collection is an ordinary
lifetime error, and the compiler does not yet diagnose it.

Without the two-sided model, you'd either lose the index story (callee
forces transfer) or lose the contract enforcement (callee can silently
keep a borrow). Both halves earn their place.

## Open design questions

1. **`#expr` for non-identifier sources.** *Shipped.* Caller-side `#`
   prefixes an arbitrary argument expression — the grammar is
   `parameterEntry : parameterLabel? REFERENCE? expression`
   (`antlr4/CajetaParser.g4`), and `#this.field`, `#someCall()` and
   unnamed temporaries such as `#("k" + i)` all transfer correctly. The
   residual issue is narrower: `#arr[i]` where the element is a borrow
   rather than an owned slot is the `owned-array-element-move`
   defect, and the title tracking for that shape is still open.

2. **`#T` on the return position — CLOSED (spec §4.6, 2026-08-15).** The
   caller-side acknowledgement exists and is required: a `#T` result must be
   received with `#=` (`Foo x #= somefn();`); a plain `=` is
   `CAJETA_ERROR_OWNED_RESULT_NEEDS_TRANSFER`
   (`src/cajeta/ownership/OwnedBindCheck.cpp`), enforced at BOTH positions —
   the declaration (`T x = f()`) since plan 8.2.7 and the assignment
   (`x = f()`, `this.f = f()`, `arr[i] = f()`) since plan 8.2.12. It is
   load-bearing exactly because it makes an acquisition legible without
   opening the callee: `int8[] w #= s.toBytes()` frees, `int8[] w = s.root()`
   does not. (`root()`, not `trimView()` — `trimView` returns a fresh
   borrow-WINDOW wrapper and is truthfully `#String`; `root()` is the plain
   borrowed handle.) Note the marker goes on the BINDING, not the type —
   `#Foo x = somefn();` is `CAJETA_ERROR_TYPE_TRANSFER_RETIRED`.

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
