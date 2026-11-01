# Field-Store Borrow Escape

Status: design draft, 2026-05-31. It was triggered by the observation that
storing a borrow of a stack local into a field of a longer-lived
object produces a dangling pointer that nothing currently catches.

Companion to [`FieldOwnership.md`](FieldOwnership.md) and
[`MemoryModel.md`](MemoryModel.md). This doc records the decision and
two non-obvious arguments before any code lands.

> **Status: not yet implemented.** `CAJETA_ERROR_FIELD_BORROW_ESCAPE` is
> proposed here, and it is not among the error constants the compiler currently
> emits. The existing return-edge checks it builds on
> (`CAJETA_ERROR_BORROW_ESCAPE`, `CAJETA_ERROR_VIEW_ESCAPE`) are real and
> shipped, and the field-store edge described below is design-stage.
>
> **Partly covered by the field-store rule.** A plain `=` store of an owned
> local (bound from `heap`, a call, `#=` or a `+`), or of a value read out of
> one, into a field of `this`, of a parameter, or of a local holder that
> escapes is now rejected with `CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE`, and a
> parameter kept with `=` must be spelled `^T`
> (`specs/field-store-ownership-spec.md`). A holder in an enclosing block
> that keeps an owned local from an inner block is rejected the same way. A `stack`
> local is outside that rule, because `#=` cannot fix it (argument (a) below),
> so the edge this document proposes is still open.

## Background — the gap left by the field-ownership relaxation

`MemoryModel.md` originally forbade borrows in fields outright:

> Old rule (~line 267): "Fields are owners. Borrows in fields are a
> static error."

`FieldOwnership.md` (2026-05-17) dropped that rule. Fields may now
hold borrows, because the stdlib needs them. `ArrayStream.data` aliases
`ArrayList.data`, `Optional.value` aliases its ctor argument, and
`Pair.first/second` likewise. Auto-drop discriminates owner from alias
at runtime via the per-fiber drop chain, so a borrowed field does not
double-free.

Note the shape of every legitimate case. The borrow's source
outlives the object that stores it. `ArrayList` outlives the
`ArrayStream` it hands out, and the `Optional`'s value is a heap instance
passed in from a caller. That is what makes them sound.

The hazard this doc addresses is the exact inverse, where the source is
shorter-lived than the storing object. When the blanket "no borrows in
fields" rule went away, the thing that incidentally rejected this case
went with it. Nothing replaced it.

## The hazard, precisely

Class instances are handled by pointer. UnifiedClasses.md says that "Class
instances always pass and return by pointer, never by value". So a
field store `obj.field = src` writes a pointer to `src` into the
object. A `stack T(...)` local lives in its frame and is reclaimed by
the drop chain at scope exit. Therefore:

> Storing a borrow of a stack local into a field of an object that
> outlives that local's scope leaves a dangling pointer in the field
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

This is the dual of a check the compiler already has.
`ReturnStatement::generateCode` in `Statement.cpp`, where the escape checks
land in the ~1660–1760 range, rejects a borrow escaping via the return edge:

```
CAJETA_ERROR_BORROW_ESCAPE     // return a closure/method-ref with borrow captures
CAJETA_ERROR_VIEW_ESCAPE       // return a view aliasing a function-local buffer
```

A field store has the same root cause, a borrow outliving its source,
escaping via a different door. That door is the field-store edge instead
of the return edge. The design intent is to extend the existing escape
analysis to this edge. No parallel system is built.

The call-edge checks report through this same escape/ownership family:

```
CAJETA_ERROR_TRANSFER_REQUIRED      // owned local into a `#T` formal without `#`
CAJETA_ERROR_TYPE_TRANSFER_RETIRED  // `#` in a TYPE position (type argument, `#Type` local,
                                    // `#V` type parameter) — ownership is per-call; spell it
                                    // at the call/store site (title-tracking spec §8.1)
```

> **Retired with title-tracking rev 2 (Unit 7).** The element-ownership
> type-argument layer no longer exists. That layer was the per-instantiation
> ownership modes, the 4B call-agreement and extractor gates
> (`ELEMENT_TRANSFER_MODE` / `ELEMENT_EXTRACT_MODE`), borrow-mode confinement
> (`BORROW_MODE_CONFINED` / `BORROW_MODE_OWNED`), and the §4.2 formal
> dissolution. Every instantiation is plain, an authored `#T` formal is a
> hard must-own edge in every instantiation, and per-value titles ride the
> hidden per-call transfer word. `BORROW_PARAM_ESCAPES` is likewise retired
> for class-typed formals, because they are runtime owners and `#x` forwards
> the flag they actually hold.

## What is statically knowable

At an assignment `target.field = src` we classify two things:

1. **Is the RHS a borrow of a stack local?** That is, `src` resolves to a
   `Field` whose storage class is stack, and the assignment is a plain
   borrow, with no `#`-transfer, no fresh `heap T(...)` temp and no
   `.clone()`. The `initIsBorrow` classifier in
   `LocalVariableDeclaration` already distinguishes borrow-shaped reads,
   and the same logic applies here.

2. **Does the target object outlive the source's scope?**

| Target shape | Outlives a function-local stack source? | Confidence | Response |
|---|---|---|---|
| `this.field` (receiver is a param) | **Always** | certain | **error** |
| `param.field` (param is a class instance) | **Always** | certain | **error** |
| heap local that is returned / transferred out | yes (escapes with it) | high | **error** |
| heap local that never escapes its scope | only within shared scope | medium | **warn** |
| stack local in an **enclosing** scope | yes | high | **error** |
| stack local, same scope, declared earlier | briefly (drop-order window) | low | **warn** / allow |

The receiver case, `this.field = stackBorrow`, is the highest-confidence
bug and likely the most common in real code. A method's receiver arrived
as a parameter, so the object's lifetime is the caller's, which is always ≥
the method call, while the stack local dies at method exit. That is a
guaranteed dangle.

The machinery to do this already exists. `Scope` chains via `parent`,
there is the `liveBorrows` path map and the storage-class on the source
`Field`, and the `BinaryOpExpression` `=` branch already performs a pre-write
borrow-overlap check, which is the natural hook point.

## Decision: tiered error / warn

1. **Error, `CAJETA_ERROR_FIELD_BORROW_ESCAPE`,** when the target's
   owning object provably outlives a stack-local borrow source, meaning
   `this.field` / `param.field`, enclosing-scope stack targets, and
   heap targets that escape. It has the same soundness bar as the
   return-escape check, and it does not touch the legitimate stdlib
   aliasing, whose sources are params and heap values, never stack locals.

2. **Warn** on the `CajetaLogger` WARNING channel in the genuinely ambiguous
   same-scope cases where we can prove neither "outlives" nor "safe",
   with a fix-it naming the two sanctioned fixes.

3. **Rejected: silent auto-copy.** Inserting a deep copy at the store
   contradicts "classes pass by pointer, no implicit copy / no slicing,"
   hides allocation cost, and breaks the explicit-allocation philosophy.
   Copy stays explicit, as `clone()` below describes.

## Two non-obvious arguments worth recording

### (a) `#`-transfer of a stack local does **not** fix it

Transfer moves ownership and leaves the storage location where it is. A
`stack Widget`'s bytes live in the frame and are gone at frame exit no
matter who owns them. So `this.w #= local` where `local` is stack-allocated
is also unsound, and `#` is not the fix here. The honest fixes are:

- **Allocate on the heap to begin with.** Write `Widget local = heap
  Widget(...)` then `this.w #= local`, or auto-promote a fresh
  `this.w = heap Widget(...)` straight into the field. That is independent
  storage that outlives the frame.
- **Clone into a fresh instance.** Write `this.w = local.clone()`, subject
  to the status caveat below.

The error message must lead with the heap-allocation fix, because it is
the one that works today and is the most honest, since the field then
genuinely owns its value.

### (b) Shallow clone is *sufficient* — because the check is compositional

`clone()` (Object.md) is planned Java-shallow, with value-typed fields
copied by `memcpy` and class-typed fields copied by reference. Shallow looks
too weak to fix an escape, but it is sufficient here:

- A shallow clone produces a fresh, independent top-level instance,
  which breaks the direct stack dangle, the case we catch.
- It only shares class-typed sub-fields by reference. But if any such
  sub-field held a stack-local borrow that outlives it, that store is
  independently flagged by this same check at its own site. By
  induction, a program that compiles has no field holding an outliving
  stack borrow, so everything shallow-clone shares is already proven to
  outlive both originals.

So we never need deep copy for soundness, and we never touch the
cyclic deep-copy problem, where cycles require an identity/visited map to
terminate. The project already chose this. The default is shallow, and a
manual `clone()` override goes deep where the author can reason about cycles.

## `clone()` status caveat — fix-it sequencing

`clone()` is deferred. The default returns `null` today, and the
synthesizer that walks field layouts is not built yet (Object.md §
"clone() — deferred", tracked in specs/Features.md). The consequences are:

- Until clone lands, the fix-it must not advertise `.clone()`, because it
  would trade a dangling pointer for a null deref. Lead with the
  heap-allocation / `#`-transfer fix only.
- When clone lands, add `.clone()` to the message. Per argument (b),
  shallow is enough, and no deep-copy work is required for soundness.

The two tracks are orthogonal. The escape check is independently
shippable now, and clone is a separate synthesizer task.

## Example matrix

Unsafe, where the receiver provably outlives, which is the high-confidence bug:

```cajeta
public class Holder {
    Widget w;
    public void capture() {
        Widget local = stack Widget(42);
        this.w = local;          // ✗ FIELD_BORROW_ESCAPE
    }
}
```

Unsafe, with an outer object and an inner stack source:

```cajeta
public void wire(Holder h) {     // h: caller-owned, outlives us
    Widget tmp = stack Widget(7);
    h.w = tmp;                   // ✗ FIELD_BORROW_ESCAPE
}
```

Unsafe, with a heap target that escapes, carrying a dead field:

```cajeta
public #Holder make() {
    Holder h = heap Holder();
    Widget tmp = stack Widget(1);
    h.w = tmp;                   // ✗ FIELD_BORROW_ESCAPE
    return #h;
}
```

Safe, and these must keep compiling, because they are why the old blanket
rule was dropped:

```cajeta
this.data = someParam;          // someParam is a `^T` formal, a borrow on every call
this.value = heap Widget(9);    // fresh heap, auto-promoted to ownership
this.w #= heapLocal;            // heap source, ownership transferred
this.w = src.clone();           // independent fresh instance  (once clone lands)
```

The discriminator is purely lifetime ordering. Reject iff the RHS is
a borrow of a stack local and the field's owning object provably
outlives that local's scope.

## Proposed error message (house style)

Model it on the return-escape messages (`ReturnStatement::generateCode` in
`Statement.cpp`). Name the dangle, then offer the working valve(s).

> cannot store stack local `local` into field `w` of `this` — `this`
> outlives this call, so the field would dangle the moment this frame's
> drop chain reclaims `local`. The value lives on the stack and cannot
> outlive the frame; transferring ownership with `#` does not help
> (ownership moves, storage does not). Fix: allocate the value on the
> heap and transfer it into the field — `this.w = heap Widget(...)` — so
> the field owns storage that outlives the call. See
> docs/specification/lang/FieldBorrowEscape.md.

Append "or store an independent copy with `local.clone()`" once
`clone()` is implemented.

## Implementation sketch

- Hook the `=` branch in `BinaryOpExpression`, which already does a pre-write
  `liveBorrows` overlap check, for the field-target shape, plus
  `DotExpression` assignment if `a.b = c` routes separately.
- Reuse `initIsBorrow`-style RHS classification and the storage-class on the
  source `Field`.
- Classify target lifetime per the table above using the `Scope` chain,
  and start with the certain tier, `this.field` / `param.field`.
- Tier 1, the error, ships first. The warn tier follows once the
  same-scope drop-order analysis is pinned down.

## Alternatives considered: auto-clone on escape

The proposal was that, instead of erroring, when a stack instance is
assigned to a longer-scoped slot, the compiler silently inserts a `clone()`
so the slot gets an independent heap copy. It is mechanically trivial,
because the same hook point that would error instead emits a clone. It was
rejected as a blanket default, and a per-type opt-in form is kept as a
future path.

The decisive issue is that cajeta classes are reference types, by-pointer,
identity-bearing, with no implicit copy. Auto-clone silently converts them to
value semantics, but only sometimes, conditioned on a scope analysis
invisible at the assignment site. The same `obj.f = local` aliases in one
scope and copies in another:

```cajeta
local.v = 5;
this.w = local;      // alias or clone? depends on scope analysis
local.v = 99;        // this.w.v is then 99 (alias) or 5 (clone)
```

This trades a loud, localized, fixable compile error for a silent
semantic divergence, the wrong answer instead of a crash, and that
divergence also flips when someone refactors a variable's scope.

| | |
|---|---|
| Benefits | "just works", no error to fix · always memory-safe · zero annotation burden · friendly to GC-language newcomers |
| Costs | silent reference→value flip conditioned on invisible analysis · shallow-clone trap: the copy still shares class-typed sub-fields by reference (independent on top, entangled underneath) · hidden heap alloc + memcpy inside a plain `=`, possibly per loop iteration · breaks three documented pillars — explicit allocation, true zero-copy, implicit-borrow/explicit-transfer · identity/resource objects (mutex, file handle, `@Inject` singletons) get silently duplicated — cloning a lock is a correctness bug · optimization-fragile (fires or not by escape-analysis precision / compiler version) |

Contrast this with the auto-promotion cajeta already does. A fresh anonymous
`heap T()` in transfer position promotes implicitly, which is safe because
the temp has no prior identity and no later use. A named stack local is the
opposite. It has identity and is used after the store, which is exactly
where the divergence bites.

The good version makes copy-ness a property of the type, declared
once through a `Copyable` / `@Value` marker or an author-written `clone()`,
and never a property of the assignment context. For a type that opts into
value semantics, auto-copy-on-escape is coherent and expected, as in the
Rust `Copy` vs `Clone` and Swift value-vs-reference splits. For everything
else, the escape stays an error. The decision then lives at the type
definition, where it is visible and intentional, instead of at every call
site, where it is invisible and contextual.

The decision is that the default is the tiered error above. Opt-in value
semantics (`Copyable`/`@Value`) is the sanctioned ergonomic path and the only
place auto-copy-on-escape belongs, and it is deferred to a separate proposal.

## Open questions

- Same-scope, target-declared-earlier: error, warn, or allow? Drop order
  is reverse declaration, so the target briefly outlives the source in
  the drop window. That is benign unless the target's drop reads the field,
  which it will not do for a borrow. The lean is allow, or warn, initially.
- Arrays-as-fields: does `obj.arr = stackArr` route through the same
  store path? Arrays have separate ownership handling, as the
  `Statement.cpp` note that array returns remain scope-owned shows, and
  they may need their own case.
- View fields: a view aliasing a stack buffer stored in a field is the
  same hazard one level down, and it likely folds into `CAJETA_ERROR_VIEW_ESCAPE`.
