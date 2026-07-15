# Field-Store Borrow Escape

Status: design draft, 2026-05-31. Triggered by the observation that
storing a borrow of a **stack** local into a field of a longer-lived
object produces a dangling pointer that nothing currently catches.

Companion to [`FieldOwnership.md`](FieldOwnership.md) and
[`MemoryModel.md`](MemoryModel.md). This doc records the decision and
two non-obvious arguments before any code lands.

> **Status: not yet implemented.** `CAJETA_ERROR_FIELD_BORROW_ESCAPE` is
> proposed here; it is not among the error constants the compiler currently
> emits. The existing return-edge checks it builds on
> (`CAJETA_ERROR_BORROW_ESCAPE`, `CAJETA_ERROR_VIEW_ESCAPE`) are real and
> shipped; the field-store edge described below is design-stage.

## Background — the gap left by the field-ownership relaxation

`MemoryModel.md` originally forbade borrows in fields outright:

> Old rule (~line 267): "Fields are owners. Borrows in fields are a
> static error."

`FieldOwnership.md` (2026-05-17) **dropped that rule**. Fields may now
hold borrows, because the stdlib needs them: `ArrayStream.data` aliases
`ArrayList.data`, `Optional.value` aliases its ctor argument,
`Pair.first/second` likewise. Auto-drop discriminates owner from alias
at runtime via the per-fiber drop chain, so a borrowed field doesn't
double-free.

Note the shape of **every** legitimate case: the borrow's source
**outlives** the object that stores it. `ArrayList` outlives the
`ArrayStream` it hands out; the `Optional`'s value is a heap instance
passed in from a caller. That is what makes them sound.

The hazard this doc addresses is the **exact inverse**: the source is
*shorter*-lived than the storing object. When the blanket "no borrows in
fields" rule went away, the thing that incidentally rejected this case
went with it. Nothing replaced it.

## The hazard, precisely

Class instances are handled **by pointer** (UnifiedClasses.md: "Class
instances always pass and return by pointer, never by value"). So a
field store `obj.field = src` writes a *pointer* to `src` into the
object. A `stack T(...)` local lives in its frame and is reclaimed by
the drop chain at scope exit. Therefore:

> Storing a **borrow of a stack local** into a field of an object that
> **outlives that local's scope** leaves a dangling pointer in the field
> the moment the frame unwinds.

```cajeta
public class Holder {
    Widget w;
    public void capture() {
        Widget local = stack Widget(42);
        this.w = local;          // ✗ `this` outlives this call; w dangles on return
    }
}
```

## Relationship to the existing borrow-escape check

This is the **dual** of a check the compiler already has.
`ReturnStatement::generateCode` in `Statement.cpp` (the escape checks land
in the ~1660–1760 range) rejects a borrow escaping via the **return edge**:

```
CAJETA_ERROR_BORROW_ESCAPE     // return a closure/method-ref with borrow captures
CAJETA_ERROR_VIEW_ESCAPE       // return a view aliasing a function-local buffer
```

A field store is the same root cause — a borrow outliving its source —
escaping via a different door: the **field-store edge** instead of the
return edge. The design intent is to extend the *existing* escape
analysis to this edge, not to build a parallel system.

The call-edge checks report through this same escape/ownership family:

```
CAJETA_ERROR_TRANSFER_REQUIRED      // owned local into a `#T` formal without `#`
CAJETA_ERROR_TYPE_TRANSFER_RETIRED  // `#` in a TYPE position (type argument, `#Type` local,
                                    // `#V` type parameter) — ownership is per-call; spell it
                                    // at the call/store site (title-tracking spec §8.1)
```

> **Retired with title-tracking rev 2 (Unit 7).** The element-ownership
> type-argument layer — owning/borrow-mode instantiations, the 4B
> call-agreement and extractor gates (`ELEMENT_TRANSFER_MODE` /
> `ELEMENT_EXTRACT_MODE`), borrow-mode confinement
> (`BORROW_MODE_CONFINED` / `BORROW_MODE_OWNED`), and the §4.2 formal
> dissolution — no longer exists. Every instantiation is plain; an
> authored `#T` formal is a hard must-own edge in every instantiation;
> per-value titles ride the hidden per-call transfer word.
> `BORROW_PARAM_ESCAPES` is likewise retired for class-typed formals
> (they are runtime owners; `#x` forwards the flag they actually hold).

## What is statically knowable

At an assignment `target.field = src` we classify two things:

1. **Is the RHS a borrow of a stack local?** I.e. `src` resolves to a
   `Field` whose storage class is stack, and the assignment is a plain
   borrow — not `#`-transfer, not a fresh `heap T(...)` temp, not a
   `.clone()`. (The `initIsBorrow` classifier in
   `LocalVariableDeclaration` already distinguishes borrow-shaped reads;
   the same logic applies here.)

2. **Does the target object outlive the source's scope?**

| Target shape | Outlives a function-local stack source? | Confidence | Response |
|---|---|---|---|
| `this.field` (receiver is a param) | **Always** | certain | **error** |
| `param.field` (param is a class instance) | **Always** | certain | **error** |
| heap local that is returned / transferred out | yes (escapes with it) | high | **error** |
| heap local that never escapes its scope | only within shared scope | medium | **warn** |
| stack local in an **enclosing** scope | yes | high | **error** |
| stack local, same scope, declared earlier | briefly (drop-order window) | low | **warn** / allow |

The receiver case (`this.field = stackBorrow`) is the highest-confidence
bug and likely the most common in real code: a method's receiver arrived
as a parameter, so the object's lifetime is the *caller's* — always ≥
the method call, while the stack local dies at method exit. Guaranteed
dangle.

The machinery to do this already exists: `Scope` chains via `parent`,
the `liveBorrows` path map, storage-class on the source `Field`, and the
`BinaryOpExpression` `=` branch already performs a pre-write
borrow-overlap check — that is the natural hook point.

## Decision: tiered error / warn

1. **Error — `CAJETA_ERROR_FIELD_BORROW_ESCAPE`** when the target's
   owning object **provably outlives** a stack-local borrow source
   (`this.field` / `param.field`, enclosing-scope stack targets, and
   heap targets that escape). Same soundness bar as the return-escape
   check; does **not** touch the legitimate stdlib aliasing, whose
   sources are params/heap, not stack locals.

2. **Warn** (`CajetaLogger` WARNING channel) in the genuinely ambiguous
   same-scope cases where we can prove neither "outlives" nor "safe",
   with a fix-it naming the two sanctioned fixes.

3. **Rejected: silent auto-copy.** Inserting a deep copy at the store
   contradicts "classes pass by pointer, no implicit copy / no slicing,"
   hides allocation cost, and breaks the explicit-allocation philosophy.
   Copy stays explicit (see `clone()` below).

## Two non-obvious arguments worth recording

### (a) `#`-transfer of a stack local does **not** fix it

Transfer moves *ownership*, not *storage location*. A `stack Widget`'s
bytes live in the frame and are gone at frame exit no matter who owns
them. So `this.w = #local` where `local` is stack-allocated is **also
unsound** — `#` is not the fix here. The honest fixes are:

- **Allocate on the heap to begin with** — `Widget local = heap
  Widget(...)` then `this.w = #local`, or auto-promote a fresh
  `this.w = heap Widget(...)` straight into the field. Independent
  storage that outlives the frame.
- **Clone into a fresh instance** — `this.w = local.clone()` (see
  status caveat below).

The error message must lead with the heap-allocation fix, because it is
the one that works today and is the most honest (the field then genuinely
owns its value).

### (b) Shallow clone is *sufficient* — because the check is compositional

`clone()` (Object.md) is planned **Java-shallow**: value-typed fields
copied by `memcpy`, class-typed fields copied by reference. Shallow looks
too weak to fix an escape — but it is sufficient here:

- A shallow clone produces a fresh, independent **top-level** instance,
  which breaks the direct stack dangle (the case we catch).
- It only shares *class-typed sub-fields* by reference. But if any such
  sub-field held a stack-local borrow that outlives it, **that store is
  independently flagged by this same check at its own site.** By
  induction, a program that compiles has no field holding an outliving
  stack borrow, so everything shallow-clone shares is already proven to
  outlive both originals.

So we never need deep copy for soundness — which means we never touch the
**cyclic deep-copy** problem (cycles require an identity/visited map to
terminate). The project already chose this: shallow default, manual
`clone()` override for deep where the author can reason about cycles.

## `clone()` status caveat — fix-it sequencing

`clone()` is **deferred**. The default returns `null` today; the
synthesizer that walks field layouts isn't built yet (Object.md §
"clone() — deferred", tracked in specs/Features.md). Consequences:

- Until clone lands, the fix-it must **not** advertise `.clone()` — it
  would trade a dangling pointer for a null deref. Lead with the
  heap-allocation / `#`-transfer fix only.
- When clone lands, add `.clone()` to the message; per argument (b),
  shallow is enough — no deep-copy work required for soundness.

The two tracks are orthogonal: the escape check is independently
shippable now; clone is a separate synthesizer task.

## Example matrix

Unsafe — receiver provably outlives (the high-confidence bug):

```cajeta
public class Holder {
    Widget w;
    public void capture() {
        Widget local = stack Widget(42);
        this.w = local;          // ✗ FIELD_BORROW_ESCAPE
    }
}
```

Unsafe — outer object, inner stack source:

```cajeta
public void wire(Holder h) {     // h: caller-owned, outlives us
    Widget tmp = stack Widget(7);
    h.w = tmp;                   // ✗ FIELD_BORROW_ESCAPE
}
```

Unsafe — heap target that escapes, carrying a dead field:

```cajeta
public #Holder make() {
    Holder h = heap Holder();
    Widget tmp = stack Widget(1);
    h.w = tmp;                   // ✗ FIELD_BORROW_ESCAPE
    return #h;
}
```

Safe — must keep compiling (these are why the old blanket rule was
dropped):

```cajeta
this.data = someParam;          // source is caller-owned, outlives `this`
this.value = heap Widget(9);    // fresh heap, auto-promoted to ownership
this.w = #heapLocal;            // heap source, ownership transferred
this.w = src.clone();           // independent fresh instance  (once clone lands)
```

The discriminator is purely **lifetime ordering**: reject iff the RHS is
a borrow of a stack local *and* the field's owning object provably
outlives that local's scope.

## Proposed error message (house style)

Model on the return-escape messages (`ReturnStatement::generateCode` in
`Statement.cpp`): name the dangle, then offer the working valve(s).

> cannot store stack local `local` into field `w` of `this` — `this`
> outlives this call, so the field would dangle the moment this frame's
> drop chain reclaims `local`. The value lives on the stack and cannot
> outlive the frame; transferring ownership with `#` does not help
> (ownership moves, storage does not). Fix: allocate the value on the
> heap and transfer it into the field — `this.w = heap Widget(...)` — so
> the field owns storage that outlives the call. See
> docs/specification/lang/FieldBorrowEscape.md.

(Append "or store an independent copy with `local.clone()`" once
`clone()` is implemented.)

## Implementation sketch

- Hook the `=` branch in `BinaryOpExpression` (already does a pre-write
  `liveBorrows` overlap check) for the field-target shape, plus
  `DotExpression` assignment if `a.b = c` routes separately.
- Reuse `initIsBorrow`-style RHS classification + storage-class on the
  source `Field`.
- Classify target lifetime per the table above using the `Scope` chain;
  start with the certain tier (`this.field` / `param.field`).
- Tier 1 (error) ships first; the warn tier follows once the
  same-scope drop-order analysis is pinned down.

## Alternatives considered: auto-clone on escape

Proposal: instead of erroring, when a stack instance is assigned to a
longer-scoped slot, silently insert a `clone()` so the slot gets an
independent heap copy. Mechanically trivial (the same hook point that
would error instead emits a clone). Rejected as a **blanket default**;
a **per-type opt-in** form is kept as a future path.

Decisive issue: cajeta classes are reference types (by-pointer,
identity-bearing, no implicit copy). Auto-clone silently converts them to
value semantics — but only sometimes, conditioned on a scope analysis
invisible at the assignment site. The same `obj.f = local` aliases in one
scope and copies in another:

```cajeta
local.v = 5;
this.w = local;      // alias or clone? depends on scope analysis
local.v = 99;        // this.w.v is then 99 (alias) or 5 (clone)
```

This trades a loud, localized, fixable compile error for a silent
*semantic* divergence — the wrong answer instead of a crash — that also
flips when someone refactors a variable's scope.

| | |
|---|---|
| Benefits | "just works", no error to fix · always memory-safe · zero annotation burden · friendly to GC-language newcomers |
| Costs | silent reference→value flip conditioned on invisible analysis · shallow-clone trap: the copy still shares class-typed sub-fields by reference (independent on top, entangled underneath) · hidden heap alloc + memcpy inside a plain `=`, possibly per loop iteration · breaks three documented pillars — explicit allocation, true zero-copy, implicit-borrow/explicit-transfer · identity/resource objects (mutex, file handle, `@Inject` singletons) get silently duplicated — cloning a lock is a correctness bug · optimization-fragile (fires or not by escape-analysis precision / compiler version) |

Contrast with the auto-promotion cajeta already does: a fresh anonymous
`heap T()` in transfer position promotes implicitly — safe because the
temp has no prior identity and no later use. A *named* stack local is the
opposite: it has identity and is used after the store, which is exactly
where the divergence bites.

The good version: make copy-ness a property of the **type**, declared
once (a `Copyable` / `@Value` marker, or an author-written `clone()`),
not a property of the **assignment context**. For a type that opts into
value semantics, auto-copy-on-escape is coherent and expected — the
Rust `Copy` vs `Clone` / Swift value-vs-reference split. For everything
else, the escape stays an error. The decision then lives at the type
definition (visible, intentional) instead of at every call site
(invisible, contextual).

Decision: default = tiered error (above). Opt-in value semantics
(`Copyable`/`@Value`) is the sanctioned ergonomic path and the only place
auto-copy-on-escape belongs; deferred to a separate proposal.

## Open questions

- Same-scope, target-declared-earlier: error, warn, or allow? Drop order
  is reverse declaration, so the target briefly outlives the source in
  the drop window — benign unless the target's drop *reads* the field
  (it won't, for a borrow). Leaning **allow** (or warn) initially.
- Arrays-as-fields: does `obj.arr = stackArr` route through the same
  store path? Arrays have separate ownership handling (see the
  `Statement.cpp` note that array returns remain scope-owned) and may
  need their own case.
- View fields: a view aliasing a stack buffer stored in a field is the
  same hazard one level down; likely folds into `CAJETA_ERROR_VIEW_ESCAPE`.
