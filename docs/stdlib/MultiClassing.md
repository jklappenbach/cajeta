# Multi-Classing — Collision Handling & Selection

This document specifies how Cajeta resolves the ambiguities that arise when a
class extends more than one parent: which inherited method runs, which field a
bare name binds to, what `super` means, and how the diamond shape is laid out.
It surveys how other languages handle the same questions and proposes the rules
Cajeta should adopt.

Scope: methods, fields, ctors, and the diamond. Out of scope: traits-style
composition (Cajeta has only `class` and `interface`), CLOS-style method
combination, and method dispatch through interface fat pointers — covered in
`UnifiedClasses.md` § Interfaces.

---

## Why multi-classing

A class `C extends A, B` lets `C` inherit state and behavior from both `A` and
`B`. The standard library uses this for orthogonal mixins — `Optional<T>
extends Stream<T>, AbstractHashable<T>` composes "yields zero or one element"
with "hashable" without forcing one to subordinate the other. The same shape
shows up wherever two unrelated capabilities should compose:

- `ArrayList<T> extends Stream<T>` so `for (v : list)` works without `.stream()`.
- `Optional<T> extends Stream<T>` so the empty/single-element stream pattern
  drops out of one declaration.
- `HashMap<K,V> extends Stream<Pair<K,V>>` so map iteration is uniform with
  collections.

The price of letting two parents contribute is the chance of name collisions.
The next sections name them precisely and then survey existing remedies.

---

## Collision taxonomy

Every multi-classing language eventually has to answer these six questions. The
language's character — strict / flexible / cooperative — shows up in how it
answers them.

### C-1. Method name + same signature

Both parents declare `int32 kind()`. What does `c.kind()` call?

```cajeta
class A { public int32 kind() { return 1; } }
class B { public int32 kind() { return 2; } }
class C extends A, B { }       // C inherits both `kind`s; which wins?
```

### C-2. Method name + different signature (overload)

Not a true collision — overload resolution picks the right one. Included for
completeness so the rules don't accidentally reject valid overloads.

```cajeta
class A { public int32 emit(int32 x) { return x; } }
class B { public int32 emit(String s) { return s.size(); } }
class C extends A, B { }       // c.emit(42) and c.emit("hi") both valid
```

### C-3. Field name collision

Both parents declare `int32 total`. The sub-object layout gives them distinct
slots (`A`'s `total` lives in `C`'s A-sub-object; `B`'s in C's B-sub-object).
But what does an unqualified `c.total` mean?

```cajeta
class A { public int32 total; }
class B { public int32 total; }
class C extends A, B { }       // c.total — which one?
```

### C-4. Diamond inheritance — shared vs replicated ancestor

`A` is base of both `B` and `C`; `D extends B, C`. Does `D` have one `A`
sub-object (shared) or two (replicated)? The choice changes the meaning of
`d.A::field` and the cost of conversion.

```cajeta
class A { public int32 x; }
class B extends A { }
class C extends A { }
class D extends B, C { }       // one A in D, or two?
```

### C-5. Abstract obligation satisfied multiple ways

An abstract method declared on `A` is satisfied by `B`'s concrete impl. `C
extends A, B` — does B's impl count? Does C need to redeclare?

### C-6. Constructor ordering

`C extends A, B` — when `C()` runs, do `A()` and `B()` both run? In what
order? Does the explicit `super(...)` form get to pick one parent or all?

---

## Survey of existing approaches

### C++

**Method/field collision (C-1, C-3).** Hard compile error. The user must
disambiguate with `A::foo` or `B::foo`. There is no implicit "first wins"
rule. A subclass can override the inherited method to pick one behavior, or
qualify each call site:

```cpp
class C : public A, public B {
public:
    int kind() override { return A::kind(); }   // override picks A's
    int otherCall() { return B::field; }        // call-site qualifies
};
```

**Diamond (C-4).** Replicated by default. To get one shared `A`, every
intermediate must inherit `virtual`-ly:

```cpp
class B : public virtual A { };
class C : public virtual A { };
class D : public B, public C { };              // one A in D
```

Virtual bases pay a vptr-per-virtual-base, vbtable lookups, and constructor-
ordering complexity (the most-derived constructor is responsible for calling
the virtual base's ctor). The cost is real but bounded; correctness is
explicit.

**Abstract satisfaction (C-5).** Inherited concrete method satisfies an
inherited abstract one only if it's reachable through the dispatch order —
in C++ that usually means an explicit `using B::method;` declaration to
pull B's name into C's scope.

**Constructor ordering (C-6).** All non-virtual base ctors run, in
left-to-right declaration order. Virtual base ctors run first, in
inheritance-DAG order, called by the most-derived ctor.

**Super-call analogue.** `B::method()` is just a qualified call; there's no
distinguished `super` keyword. The qualification can target any reachable
ancestor.

**Verdict.** Strict and explicit, but verbose. Diamond + virtual base is a
correctness landmine — silent slicing across replicated bases is a famous
gotcha.

### Python (C3 linearization / MRO)

**Method/field collision (C-1, C-3).** No error. The compiler runs C3
linearization on `class C(A, B)` to compute a Method Resolution Order
(MRO) — a list of classes in lookup priority. Attribute access walks the
MRO top-down and returns the first match.

```python
class A: def kind(self): return 1
class B: def kind(self): return 2
class C(A, B): pass
C().kind()  # 1 — A is earlier in MRO
```

**Diamond (C-4).** Always shared. The MRO algorithm guarantees each ancestor
appears exactly once in the linearization, regardless of how many paths
reach it.

**Super (C-6).** `super().method()` calls the *next* class in the MRO from
the current class's position — not necessarily the immediate parent. This
enables cooperative multiple inheritance, where each class calls
`super().method()` and the chain walks the MRO automatically.

```python
class Logger:
    def save(self):
        print("logging")
        super().save()
class Auditor:
    def save(self):
        print("auditing")
        super().save()
class Storage:
    def save(self): print("writing")
class C(Logger, Auditor, Storage): pass
C().save()  # logging, auditing, writing (MRO: C → Logger → Auditor → Storage)
```

**Verdict.** Convenient — no boilerplate at the use site — but the MRO
linearization is invisible. Users who don't know C3 are surprised; users who
do still trip over diamond cases where MRO refuses to compute a consistent
order ("MRO conflict"). Cooperative MI demands every contributor follow the
`super()` discipline; mixing cooperative with non-cooperative breaks calls
silently.

### Scala (trait linearization)

**Method/field collision (C-1, C-3).** Trait composition with right-to-most
linearization (`with` clauses). When two parents define the same method, the
compiler picks the latest in linearization order. Subclasses that want to
disambiguate must override and call `super[Trait].method` to reach the
specific parent.

```scala
trait A { def kind = 1 }
trait B { def kind = 2 }
class C extends A with B {
  override def kind = super[A].kind   // explicit pick
}
```

**Diamond (C-4).** Linearization shares (one instance per ancestor).
Traits can't declare instance state without a constructor; that side-steps
the replicated-state problem.

**Verdict.** Linearization is the same MRO machinery as Python's, but
Scala's `super[Trait]` makes parent-selection explicit at the call site.
A nice middle ground — convenient default, explicit when you need it.

### Eiffel (rename / redefine / undefine / select)

The most explicit MI language. Inheritance declarations carry clauses that
spell out how to handle every collision:

```eiffel
class C
inherit
    A
        rename kind as a_kind
        redefine make
    B
        rename kind as b_kind
        undefine other
    select kind from A   -- in diamond, pick A's view
end
```

**Method/field collision (C-1, C-3).** Must be resolved at the inheritance
declaration via `rename` (give the inherited name a new local name) or
`undefine` (drop the inherited name). Calls can then use unambiguous local
names.

**Diamond (C-4).** Replicated by default; `select` clause forces sharing
for one ancestor view.

**Verdict.** Maximally explicit — collisions are statically detected and
the source of resolution is right there in the class header. Verbose, but
nothing happens by accident. Few mainstream languages adopted this style
because the boilerplate is heavy in the common case where there's no
collision.

### Kotlin (single class + default-method interfaces)

Kotlin has only single class inheritance + multiple interfaces. Default
methods on interfaces give a subset of the MI problem. The disambiguation
syntax is `super<Interface>.method`:

```kotlin
interface A { fun kind() = 1 }
interface B { fun kind() = 2 }
class C : A, B {
    override fun kind() = super<A>.kind()
}
```

**Verdict.** Avoids full MI's problems by restricting state to a single
class chain. Default methods are the only collision surface; the
`super<I>.m()` syntax is the model for explicit parent-selection that
Cajeta should follow.

### CLOS (Common Lisp Object System)

Method combination, not just lookup. A generic function gathers contributions
from every applicable class along the MRO ("class precedence list") and runs
them in declared order:

```lisp
(defmethod save :before ((x logger)) (log-it x))
(defmethod save :before ((x auditor)) (audit-it x))
(defmethod save ((x storage)) (write-it x))
```

A `:before` method runs before the primary; `:after` after; `:around` wraps
the chain with explicit `call-next-method`. Subclasses don't override — they
*add* contributions.

**Verdict.** Far beyond what any C-family language attempts. The expressive
power is real (think aspect-oriented programming as a first-class feature)
but the cost is that every method call is a runtime walk through the CPL,
gathering applicable contributions. Cajeta's `@Before`/`@After` aspect
annotations (`AspectModel.md`) cover this territory without elevating it
to the core language.

### Summary table

| Aspect                  | C++              | Python           | Scala            | Eiffel           | Kotlin           |
|-------------------------|------------------|------------------|------------------|------------------|------------------|
| Method conflict         | error → qualify  | MRO first wins   | linearization    | rename/undefine  | error → `super<I>` |
| Field conflict          | error → qualify  | MRO first wins   | n/a (traits)     | rename           | n/a              |
| Diamond default         | replicated       | shared (MRO)     | shared           | replicated       | n/a              |
| Diamond opt-in          | `virtual`        | always           | always           | `select`         | n/a              |
| Parent-selection syntax | `Base::method`   | `super()` MRO-relative | `super[Trait].m` | rename, then local name | `super<I>.m`     |
| Ctor order              | left-to-right + virtual bases first | MRO | linearization | declared | n/a |

---

## Cajeta today (state-of-the-tree)

Pinned by `test/parser/MultipleInheritanceGapTests.cpp` (15 active
tests; the file's own header comment claiming they are still `DISABLED_`
is itself stale — the `TEST(...)` names carry no `DISABLED_` prefix):

- **Layout.** `class C extends A, B` lays out as `{ vtable_primary,
  A-sub-content-shares-primary, vtable_secondary_for_B, B-sub-content,
  C-own-fields }`. Per-parent sub-objects with their own vptr slots; first
  parent shares the primary vptr (C++ single-inheritance fast path).
- **Vtable.** Hash-keyed (FNV-1a of canonical signature). Override
  resolution aliases each ancestor's hash to the most-derived impl, so a
  call on `C` dispatches to `C`'s override regardless of whether the
  receiver is typed as `A`, `B`, or `C`.
- **Polymorphic dispatch.** `B b = c;` adjusts the pointer to C's B
  sub-object at the assignment site; `b.foo()` reads the secondary vtable
  at `b[0]`. Cross-class overrides (e.g., C overrides B's method) are
  reached via synthesized offset thunks that restore the most-derived
  pointer before tail-calling the override.
- **Super-call.** `super.method()` resolves to the **first declared
  parent**'s method. `super<Base>.method()` — picking a non-first
  parent — is **now implemented** (angle-bracket selector; grammar
  `primary: SUPER '<' typeType '>'`, lowered in
  `src/cajeta/asn/expression/Expression.cpp` `SuperExpression`). The
  sibling `this<Base>.field` selector is implemented the same way.
- **Ctor.** Implicit super-ctor invocation runs for **every** parent in
  declared order (Gap 1). Explicit `super(args)` reaches the first
  parent only.
- **Override aliasing.** `buildVirtualTable` walks the hierarchy
  parent-first and indexes by suffix; a child method with the same
  name+params overrides every ancestor's same-suffix method
  simultaneously (see `CajetaClass.cpp:buildVirtualTable`). This is
  Python-style "one override covers all paths," not Eiffel-style
  "rename to keep both."

The four scenarios that are **not yet handled**:

1. **Bare method-name collision** (C-1). Both A and B declare `int32
   kind()`. C inherits both. `c.kind()` today: the compiler picks
   whichever entry the hash-keyed lookup happens to find first. No
   error, no warning. **This is a bug** — silent ambiguity.
2. **Bare field-name collision** (C-3). Both A and B declare `int32
   total`. `getFieldLlvmIndex` returns whichever slot the DFS walker
   reaches first. **Same bug** — silent ambiguity.
3. **Receiver-expression parent view** (`expr[Base]` / the `c[A]`
   form used in the worked examples below). The angle-bracket
   selectors on `this` / `super` inside a method body
   (`this<Base>.field`, `super<Base>.method()`) **have shipped** — see
   `test/parser/OverrideFromTests.cpp`, which exercises
   `super<B>.stride()`. What is **not** implemented is selecting a
   parent view off an arbitrary receiver expression (`c[A].kind()`):
   a square bracket on an expression still parses as indexing. The
   `c[A]` / `d[B]` / `e[B]` spellings in the worked examples and error
   messages below are therefore aspirational shorthand.
4. **Diamond — shared vs replicated.** When `D extends B, C` and both
   B and C extend a non-`Object` A, A's sub-object appears twice in
   D's layout (replicated, C++-style default). Cajeta has no `virtual`
   / `shared` keyword to force sharing. Today this happens to work for
   `Object` (the implicit root — `getNonFirstSubObjects` walks the
   layout deterministically so the root collapses), but breaks for any
   user-declared diamond.

---

## Proposed Cajeta design — best-of-breed

The shape of the recommendation: **strict by default, explicit when ambiguous,
ergonomic when not.** Borrows Kotlin's `super<I>.m()` model for parent
selection, Eiffel's "decide at the declaration site" instinct for class-level
disambiguation, and Python's MRO-style sharing for the diamond default.
Rejects MRO-implicit method picking — when two parents collide, the user
should know.

### P-1. Reject ambiguous unqualified access at compile time

When `c.kind()` would resolve to two distinct inherited methods with
incompatible bodies, the compiler raises an error:

```
error[CAJETA_ERROR_AMBIGUOUS_METHOD_DISPATCH]:
  call `c.kind()` is ambiguous; both `test.A.kind()` and `test.B.kind()`
  match. Resolve by either:
    1. overriding `kind` in `C` (the override becomes C's single
       canonical impl; previous ancestors are reachable via
       `super<A>.kind()` / `super<B>.kind()`).
    2. qualifying the call: `c[A].kind()` to view as A, `c[B].kind()`
       to view as B.
```

Symmetric rule for field reads/writes: `c.total` → `CAJETA_ERROR_AMBIGUOUS_FIELD_ACCESS`
with the same two remediation options.

This is C++'s default but with the error message doing the teaching that
C++'s leaves to the user. Compare to Python (silent surprise) and Eiffel
(boilerplate before any collision exists) — the Cajeta rule pays zero
ergonomic cost when there's no collision and a clear single-message cost
when there is.

### P-2. `super<Base>.method()` and `this<Base>.field` for explicit selection

The bracketed type qualifier extends what `super.method()` and `this.field`
already mean today, narrowing the scope to one specific ancestor's view:

```cajeta
class A { public int32 kind() { return 1; } }
class B { public int32 kind() { return 2; } }

class C extends A, B {
    public int32 kind() {                          // override picks one
        return super<A>.kind() + super<B>.kind();  // 1 + 2 = 3
    }
}

class A { public int32 total; }
class B { public int32 total; }
class C extends A, B {
    public C() {
        this<A>.total = 10;   // writes A's slot
        this<B>.total = 20;   // writes B's slot
    }
    public int32 sum() {
        return this<A>.total + this<B>.total;
    }
}
```

Inside a method body of `C`, `this<A>` is an l-value of static type `A*` whose
runtime address is `(char*) this + offsetof_A_in_C` — exactly the upcast that
the polymorphism work already performs for assignment sites. The `super<A>`
form is the same mechanism applied to the dispatch site (already half-built —
see `CajetaClass::adjustForUpcast` and `getSubObjectByteOffset`).

The `[Base]` qualifier rejects non-ancestor types at compile time:

```
error[CAJETA_ERROR_NOT_AN_ANCESTOR]:
  `this<Date>.field`: `Date` is not a base of `C`.
```

**Why angle brackets and not `A.super.m()` (Java) or `super[A].m()` (Scala)?**
`A.foo` is already an identifier-DOT-identifier (static field / static method
on A), so the Java form would compete with that path. The angle form parallels
template-instantiation syntax (`Foo<T>`, `method<T>(args)`) — the parser
commits to the parent-view alternative only when it can match `<typeType>`
followed by the trailing `.member`, so plain `<` comparisons against THIS /
SUPER (which wouldn't typecheck as numerics anyway) continue to fall through
to the bare alternatives. Earlier drafts used square brackets (`super[A]` /
`this[A]`); the angle form reads more naturally at the call site and pays
the small grammar cost in exchange.

### P-3. Class-level override redirect

A frequent collision pattern is "both parents define this method; I want
B's version always." The override path of P-1 handles this:

```cajeta
class C extends A, B {
    public int32 kind() { return super<B>.kind(); }
}
```

That's three lines and a perfectly fine answer. For users who want it
even shorter, a sugar form:

```cajeta
class C extends A, B {
    public int32 kind() = super<B>.kind();   // expression-bodied override
}
```

This is just the existing Java 25 expression-bodied method syntax — no new
parser work. The compiler emits an override whose body is `return super<B>.kind();`.
The same shape generalizes to "pick A's field as my `total`":

```cajeta
class C extends A, B {
    public int32 get_total() = this<A>.total;
}
```

Field-level renaming (Eiffel's clause) isn't needed — exposing
`this<A>.total` directly is enough to make `c.A_total` unnecessary.

### P-4. Diamond — shared by default

Two distinct scenarios get conflated under "MI field collision"; they
have different answers and need to be kept separate.

#### Scenario 1 — true diamond (common ancestor)

A field declared once in a shared ancestor:

```cajeta
class A { public int32 x; }
class B extends A { }
class C extends A { }
class D extends B, C { }
```

D has **one** A sub-object. Both B's view and C's view of A point at the
same memory. `d[B].x = 1; assert d[C].x == 1;` always holds. Coalescing
makes semantic sense because A.x was declared *once* — there is genuinely
a single field in the source, and B and C just inherit access to it.

#### Scenario 2 — unrelated parents, accidental same name

Two unrelated classes that happen to declare a field with the same name:

```cajeta
class A { public int32 x; }
class B { public int32 x; }      // unrelated to A; coincidence
class C extends A, B { }
```

C has **two** distinct `x` slots. They're separate declarations with
possibly different types, different invariants, different intended
semantics — coalescing would be semantically wrong. `c.x` is ambiguous
and rejected (P-1); `c[A].x` and `c[B].x` reach the independent storage.

The shared-by-default rule applies **only to Scenario 1**. Scenario 2
always keeps separate slots; no keyword controls this and no keyword
ever should.

#### Implementation note

Cajeta's bookkeeping already supports shared diamond for the implicit
`Object` root — `getNonFirstSubObjects` walks the layout deterministically
and the C3-like walk collapses both paths to Object onto a single
sub-object. Promoting the same behavior to user-declared diamonds
requires the vbase machinery outlined in § Phase 3.

#### Deferred — replicated diamond opt-in

For users who genuinely want two independent A sub-objects in D (the
rare "modeling double-employment via inheritance" case), a keyword
opt-in was considered:

```cajeta
class D extends <kw> B, <kw> C { }
```

Candidate names: `replicated` (technical, vague), `distinct` (cleaner
but still indirect — the keyword sits on the parents B and C but the
thing it controls is the ancestor A). A more precise form would place
the keyword on the ancestor itself, in a `with` clause:

```cajeta
class D extends B, C with distinct A { }
```

**Status: deferred.** No stdlib row needs this, and composition is the
principled answer for the rare case:

```cajeta
class D {
    public B asB;     // contains its own A
    public C asC;     // contains its own A
    // Compose access methods as needed.
}
```

If a real use case files a bug, the keyword form (likely `distinct A`
in a `with` clause) is additive — adding it later doesn't paint the
design into a corner.

### P-5. Abstract obligation — inherited concrete satisfies abstract

When A declares `abstract int32 step();` and B (a sibling, not derived from
A) declares concrete `int32 step() { ... }`, a class `C extends A, B`
inherits B's `step` as the concrete impl that satisfies A's obligation.
Today's `buildVirtualTable` already does this — the suffix-match aliasing
treats B's `step` as the most-derived impl reachable from A's hash. No
language change needed; document the behavior here so users can rely on it
without reading source.

If both A and B declare `abstract step()`, C must provide a concrete impl
itself (or remain abstract); P-1's enforcement already covers this via
`CAJETA_ERROR_ABSTRACT_NOT_IMPLEMENTED`.

### P-6. Ctors — implicit-all, explicit-one-pick

Current behavior (Gap 1): an implicit `super()` call runs for every parent
in declared order before the subclass ctor body executes. An explicit
`super(args)` invokes the first parent's matching ctor only.

The proposed extension parallels P-2: `super<Parent>(args)` invokes the
named parent's ctor. When any explicit `super<Parent>(...)` is present,
the implicit pass skips that parent (a parent gets at most one ctor call
per construction).

```cajeta
class A { public A(int32 x) { ... } }
class B { public B(String s) { ... } }
class C extends A, B {
    public C() {
        super<A>(42);
        super<B>("hi");
        // no implicit super() pass — both parents are explicitly initialized
    }
}
```

If A has only an args-ctor and `C` omits `super<A>(...)`, the compiler
reports the missing parent ctor (matching the existing
`CAJETA_ERROR_MISSING_PARENT_CTOR` shape).

---

## Worked examples

### E-1. Stream + Hashable composition (no collisions)

The motivating case for L-03. No methods collide between `Stream<T>` and
`AbstractHashable<T>`, so nothing in P-1 / P-2 fires. The layout, dispatch,
upcasts, secondary vtables, and offset thunks all use the machinery that
landed in the polymorphism work; the user writes the natural form:

```cajeta
public class Optional<T> extends Stream<T>, AbstractHashable<T> {
    private T value;
    private boolean present;
    @Override public Optional<T> next() { ... }   // Stream's obligation
    @Override public int64 hash() { ... }         // AbstractHashable's
}

Optional<int32> opt = Optional.of(42);
Stream<int32> s = opt;          // upcast — first parent, no offset
int64 h = opt.hash();           // direct dispatch
```

### E-2. Two parents both define `count()`

```cajeta
class Cache extends ArrayList<int32>, Histogram {
    // ArrayList.count() returns element count
    // Histogram.count() returns bucket count
}
Cache c = heap Cache();
int32 n = c.count();    // ERROR — CAJETA_ERROR_AMBIGUOUS_METHOD_DISPATCH
```

Remediation:

```cajeta
class Cache extends ArrayList<int32>, Histogram {
    // Make ArrayList's count() the canonical Cache.count().
    public int32 count() = super<ArrayList>.count();
}
int32 n = c.count();              // ArrayList's count
int32 b = c[Histogram].count();   // Histogram's view, still callable
```

### E-3. Diamond with shared ancestor

```cajeta
class Hashable { public int64 hash() { return 0; } public int64 seed; }
class A extends Hashable { ... }
class B extends Hashable { ... }
class C extends A, B { }       // Hashable is shared (P-4 default)

C c = heap C();
c[A].seed = 99;
int64 viaA = c[A].seed;        // 99
int64 viaB = c[B].seed;        // 99 — shared Hashable
```

If the user wanted independent `Hashable` state per arm, the v1
answer is composition rather than a `distinct` keyword (see P-4
§ Deferred — replicated diamond opt-in):

```cajeta
class C {
    public A asA;     // its own Hashable inside
    public B asB;     // its own Hashable inside
}
c.asA.seed = 99;
c.asB.seed = 77;
```

### E-4. Field collision via different parents — Eiffel-style rename, modern syntax

```cajeta
class Logger { public String name; }
class Auditor { public String name; }
class Audited extends Logger, Auditor {
    // Avoid the unqualified `name` collision by exposing both as
    // distinct properties on the subclass.
    public String loggerName()  = this<Logger>.name;
    public String auditorName() = this<Auditor>.name;
}
```

No new "rename" syntax — the expression-bodied accessor + the `this<Base>`
selector together cover what Eiffel's `rename` clause does, in Cajeta's
existing grammar.

---

## Implementation outline

The proposal lands in three phases. Each is independently testable; nothing
in a later phase blocks an earlier one's value.

### Phase 1 — collision rejection (P-1)

Extend `CajetaClass::buildVirtualTable` to detect when two ancestors
contribute methods with the same suffix but different implementations
(neither overrides the other). Today the suffix-aliasing silently picks
one; the fix is to raise `CAJETA_ERROR_AMBIGUOUS_METHOD_DISPATCH` when
the count of impls reaching a suffix is > 1 and the current class doesn't
provide its own override.

Same logic for fields: `getFieldLlvmIndex` walks the layout DFS and
returns the first slot whose property matches by name. Detect duplicates
and raise `CAJETA_ERROR_AMBIGUOUS_FIELD_ACCESS` for unqualified access;
allow qualified access through P-2.

**Estimated effort.** ~1 session. The detection is local to existing
walks; the error-message tooling already exists (see Gap 4 / Gap 7
enforcement landings).

### Phase 2 — `super<Base>.m()` and `this<Base>.field` (P-2) — DONE

**Shipped.** The form landed as the **angle-bracket** selector, not the
square-bracket sketch below: the `primary` rule accepts
`THIS '<' typeType '>'` and `SUPER '<' typeType '>'`, and
`SuperExpression` carries the chosen ancestor
(`src/cajeta/asn/expression/Expression.cpp`). The historical sketch
below (square brackets) is retained only to show the path; the angle
form was chosen to reuse template-instantiation syntax.

Grammar change (historical sketch): extend the primary-expression rule
to accept `super '[' typeName ']'` and `this '[' typeName ']'` as
receivers. Wire `SuperExpression` to carry the chosen ancestor (it
already exists; Gap 5 added it for the unbracketed form). Add a
`ThisAtBaseExpression` or reuse `SuperExpression` with a flag.

Code-gen: both forms reduce to the existing `adjustForUpcast` +
`forceDirectCall` machinery. `super<B>.kind()` calls `B`'s method
function (no virtual dispatch), with `this` adjusted by `offsetof_B_in_self`.
`this<B>.field` returns a GEP into the B sub-object at the property's
slot inside B's standalone layout.

**Estimated effort.** ~1 session. Grammar + 2 AST nodes + reuse of
existing helpers.

### Phase 3 — shared diamond (P-4)

The bookkeeping rework. Each non-first sub-object whose ancestor chain
includes a class that's also reachable from another sub-object needs an
extra indirection: instead of embedding A's sub-object inline, embed a
pointer to the shared A. Both views land on the same memory.

The layout becomes `{ vtable_primary, A-shared-content, vtable_B,
B-content-minus-A, vptr_to_shared_A, vtable_C, C-content-minus-A,
vptr_to_shared_A, D-own-fields }`.

Method dispatch through the offset thunks already does the right thing
for non-shared-ancestor cases; for the shared one, the thunk needs to
load the `vptr_to_shared_A` indirection before adjusting `this`.

No `replicated` / `distinct` branch lands in this phase — see § P-4's
deferred subsection. If the keyword form ever ships, it adds a
per-extends-clause flag and a branch in the layout walker; the
secondary vtable + thunk paths stay unchanged.

**Estimated effort.** ~2 sessions. The hardest piece because the layout
walker, offset map, secondary vtable builder, and thunk emitter all
need a "this ancestor is shared with another path" check. Phase 1 and
2 ship without needing this.

---

## Resolved decisions

### R-1. Parent-view selection — angle-bracket on `this` / `super`

**As shipped**, the selector is the angle-bracket form on `this` and
`super` inside a method body: `this<A>.field` and `super<A>.foo()`
(grammar `primary: THIS '<' typeType '>' | SUPER '<' typeType '>'`).
The angle form reuses template-instantiation syntax and stays
grammatically distinct — ANTLR commits to it only when `<typeType>` is
followed by a trailing `.member`, so plain `<` comparisons fall
through. (The `c[A].foo()` square-bracket form on an arbitrary
receiver — used in the worked examples below — is **not** implemented;
a square bracket on an expression parses as indexing.)

Considered but rejected: dotted alias (`c.A.foo()`) — Java-friendlier
but forces parent-vs-static disambiguation in the parser.

### R-2. Implicit-ctor-skip warning — narrow ambiguity only

Emit a warning **only** when:

1. an explicit `super<A>(args)` ctor call appears in this body, AND
2. another parent has both a no-arg AND an args-ctor, AND
3. the no-arg got picked implicitly.

That's the actual footgun (user wrote one explicit pick and didn't
realize a sibling parent had an args ctor they should have picked
too). Other cases stay silent: pure-implicit on every parent is fine;
args-only parent with no explicit pick already errors via
`CAJETA_ERROR_MISSING_PARENT_CTOR`.

Considered but rejected: no warning (misses the footgun); warn
aggressively on every implicit-pass skip (lint noise on intentional
skips).

### R-3. `@Override(from=Parent)` — optional, documentation + verification

`@Override(from=B)` is accepted on any override; compiler verifies the
named parent actually declares a matching method (same suffix). Omitting
is fine — the override body already carries the intent in
`= super<B>.kind()` form. Adds zero ergonomic cost when omitted, adds a
verified-by-compiler reader cue when present. **Implemented** — see
`test/parser/OverrideFromTests.cpp` (bare `@Override` is also accepted
as a no-op; there is no `override` keyword).

Considered but rejected: no annotation at all (loses the optional
verification step); required when ambiguous (forces boilerplate on
every disambiguation site even when the body is self-documenting).

---

### R-4. Aspects only — no MI-driven method combination

Cross-cutting concerns live in `@Aspect` classes (`AspectModel.md`),
decoupled from the inheritance graph. No `combine` keyword, no
CLOS-style automatic composition of parent contributions, no qualifier
syntax on the method itself.

The mental model the child author works with:

- **Polymorphism picks one method to execute** — the most-derived
  override, found via the hash-keyed vtable (`__cajeta_vtable_lookup`).
- **The body author chains explicitly** via `super.method()` or
  `super<Base>.method()` when they want parent behavior. Omitting the
  `super` call is how a child *replaces* parent behavior rather than
  extending it.
- **Aspects bind to body execution** — `@Before` / `@After` / `@Around`
  on a pointcut targeting `A.method()` fires when A.method's body is
  about to run, including when reached transitively through
  `super.method()` from a subclass. If the subclass never calls
  `super.method()`, the parent's body never runs and the parent's
  aspects never fire — exactly matching the "full replacement"
  semantics the child intended.

#### Worked example — aspect on parent, child chains or replaces

```cajeta
class A {
    public void doSomething() { Log.info("A doing"); }
}
class B extends A {                // chains
    public void doSomething() {
        prepare();
        super.doSomething();       // A's body runs, A's aspects fire
        cleanup();
    }
}
class C extends A {                // replaces
    public void doSomething() {
        somethingTotallyDifferent();
        // No super call. A's body doesn't run; A's aspects don't fire.
    }
}

@Aspect public class ParentAspect {
    @Before("execution(* A.doSomething())")
    public void preAudit() { Log.info("about to enter A.doSomething"); }
}
```

| Call | A.body runs? | ParentAspect fires? |
|---|---|---|
| `a.doSomething()` on actual A | yes | yes |
| `b.doSomething()` on B (chains via super) | yes (via chain) | yes (during chain) |
| `c.doSomething()` on C (full replacement) | no | no |

This is the property CLOS-style combination gives up. In CLOS,
`ParentAspect`-style contributions would fire on every dispatch
regardless of whether the child wanted parent behavior. Cajeta's model
trades the conciseness of automatic composition for predictability
("what the body says is what runs") and for the child author's full
control over inheritance participation.

#### Reasoning recap

- Aspects + explicit `super` already cover the cross-cutting + chaining
  surface. Adding `combine` (or replacing aspects with MI-driven
  combination) would be two mechanisms doing the same job.
- "What runs when you call this method" should be answerable by reading
  the body and its known aspects, not by walking the entire inheritance
  chain looking for contributions. Predictability > conciseness.
- A child that overrides to *replace* parent behavior must be able to
  do so cleanly. CLOS-style auto-composition fights against this; the
  explicit-chain model supports it directly.

Considered but rejected: hybrid (aspects + `combine` keyword) — two
mechanisms with overlapping reach; choice paralysis without expressive
gain. Full replacement of aspects with MI combination — would discard
a shipped, working feature and force every cross-cut to live somewhere
in the inheritance graph (painful for cross-cuts that span unrelated
classes).

---

## Future directions

Items that aren't open in the same sense — they're orthogonal extensions
to think about later.

- **`mixin` keyword as a state-less restriction.** A `mixin` would be a
  class that can't declare fields, only methods. Multi-inheriting mixins
  is cheap (no sub-object layout needed) and dodges field-collision
  questions entirely. If the stdlib accumulates many state-less helpers
  the keyword may be worth its weight; until then, abstract classes
  cover the case.
- **Virtual destructor chaining across MI.** Single-inheritance destructor
  dispatch lands through Gap 1's virtual-drop machinery (vtable's drop_fn
  slot). The MI question — should each parent's destructor run, in what
  order — parallels the implicit-ctor rule (run all parents in declared
  order, post-body). Not implemented; tracked in `MemoryModel.md` § Known
  gaps.
- **Numeric tower / arithmetic promotion via MI.** Some languages (Scheme,
  Racket) use multi-inheritance hierarchies to implement number-tower
  promotion. Cajeta uses typed primitive widening (`int8` → `int32` etc.)
  with explicit casts, so MI isn't the right tool here. Not planned.

---

## See also

- `docs/stdlib/UnifiedClasses.md` § Inheritance — the rest of the L-03
  surface.
- `docs/Features.md` L-03 — current implementation status of the
  multi-inheritance feature row.
- `ToDo.md` Priority 2 § 5 — running gap list (Gap 9 =
  `super<Base>.method()`, now shipped; diamond / virtual base = Phase 3
  above, still open).
- `test/parser/MultipleInheritanceGapTests.cpp` — 15 tests pinning the
  current behavior; the doc's "open" items here are the natural extensions.
- `test/parser/OverrideFromTests.cpp` — `@Override(from=Parent)` and
  `super<Base>.method()` behavior (R-3, P-2).

---

## Templates × multiple inheritance — composable generic mixins

The two features compose into something neither has alone. Generics give you one
implementation reused across types; multiple inheritance lets a class assemble
concrete behavior from several bases. Together they give **reusable, generic
behavior mixins**: a type inherits a mixin's *implementation* once and shares it
across every instantiation — without the per-type boilerplate that interfaces
(which have no default methods in Cajeta) would force.

```cajeta
// Reusable behavior mixins — concrete, written once.
public class Identified { public int64 id;  public Identified() { this.id = 0; }  public String idTag()  { return "#" + this.id; } }
public class Versioned  { public int32 ver; public Versioned()  { this.ver = 1; } public int32  version() { return this.ver; } }

// A generic container that inherits BOTH mixins.
public class Crate<T> extends Identified, Versioned {
    public T value;
    public Box() { return; }
    public T get() { return this.value; }
}

Crate<int32>  bi = stack Crate<int32>();   // idTag()/version() for free
Crate<String> bs = stack Crate<String>();  // ...and so for every other T
```

Without templated MI you'd either re-implement `idTag()`/`version()` in every
`Box`-like type (an interface gives you the contract, not the body), or collapse
the mixins into one base and lose the freedom to combine them. With it,
`Identified` and `Versioned` are authored once and mix into any generic type —
the same way one generic type can be "streamable AND hashable AND comparable" by
extending three small bases instead of carrying N copies of the same code.

Collisions follow the same rule as non-generic MI (P-1): if two mixins declare
the same method, the deriving type overrides it and selects a parent with
`super<Base>.method()`. Verified compiling via `Crate<int32> extends Identified,
Versioned`; runnable in the multiple-inheritance tour demo.
