# Unified Classes — v2 Design

This document specifies cajeta's v2 class model: a single `class` keyword whose instances may be allocated either on the stack or the heap, chosen at the use site rather than baked into the type declaration. It replaces the v1 split between `struct` (stack-only value aggregate) and `class` (heap-only reference). Views (`view`) and array primitives (`T[]`) are unaffected.

This is a foundational pivot. Everything in the standard library — Optional, Stream, the collection types — depends on it. It also retires several restrictions and bug-prone contortions that the v1 split forced (see § Background).

---

## Goals

- **One concept, two storage policies.** A class is a single type-design decision; the caller chooses stack or heap when they allocate. No "should this be a struct or a class?" gate at declaration time.
- **Uniform inheritance, dispatch, methods, fields.** Whether a class instance lives on the stack or the heap, it has a vtable, supports inheritance (single and multiple), and is callable through interface fat pointers. No second class of stack-only behaviors.
- **Memory safety preserved.** The borrow checker + drop chain catch escapes, double-frees, use-after-move, and (in a follow-on) iterator invalidation. C++'s slicing, dangling references, and forgotten destructors are not possible.
- **No surprise allocation.** Allocations are always explicit at the use site (`stack ClassName(...)` / `heap ClassName(...)`); no implicit boxing, no implicit copy constructors, no implicit heap traffic.
- **Most v1 implementation work survives.** The S6–S11 machinery (aggregate-init, drop chains, fat-pointer dispatch, escape checks, sret return semantics) generalizes from "struct-only" to "class with chosen storage." Sessions of past work fold into the unified model rather than getting discarded.

---

## Non-goals (v1 of the unified model)

- **Stack-allocated class returned by value through a base-typed signature.** Pointer-only pass/return semantics sidestep object slicing entirely; we don't try to handle the C++ case where copy-by-value-to-base might slice.
- **Implicit move / copy constructors.** Movement is explicit via `#`; copies are explicit via factory methods or aggregate init from another instance.
- **Stack-allocated class fields holding stack-allocated class values inline.** Class fields hold pointers to class instances (heap or stack — borrow checker enforces lifetimes). No inline embedding of class values in class layouts. (Views keep their own inline-layout rules; they're unaffected.)
- **Default methods on interfaces.** Multi-inheritance + abstract classes provide the DRY win (see § Inheritance); no new compiler feature needed for default methods.
- **Out-parameters.** Methods receive pointers but can't rebind caller locals. If out-params land later, the definite-assignment rules extend additively.

---

## Background — why move from v1's struct/class split

The v1 model had `struct` as a stack-only value aggregate (no vtable, no inheritance, no virtual dispatch) and `class` as a heap-only reference type (vtable, single + multiple inheritance, virtual dispatch). The two paths were grafted into the compiler through a shared `CajetaAggregate` base, two divergent codegen routines, and a sequence of contortions that worked around the type-system split:

- **S6.1** — `Foo(args)` ctor syntax on a struct segfaulted (no ctor path for struct receivers).
- **S6.4** — class-ref fields in structs could only be moves, not borrows (struct destruction was structural; class-instance ownership lived elsewhere).
- **S7.4** — move out of a struct field marked the path moved at compile time but didn't clear the runtime pointer, risking double-free.
- **S7.5 / S10.5** — arrays of class instances had an element-layout ambiguity; same gap blocked polymorphic interface arrays.
- **S8.4** — chained struct-returning method calls segfaulted (repackaging into a fresh body alloca didn't compose as a receiver).
- **S10.3** — struct-rooted interface values needed an escape check (struct body lives only as long as the frame; interfaces could be returned).
- **S11.2** — same-concrete-type return through dyn dispatch had to be rejected because the fat pointer didn't carry the struct's size.
- **Generic restriction** — `T[]` fields were struct-only because the generic-instantiation paths between struct and class diverged.

Each contortion was solvable in isolation. None went away. They were all symptoms of the same root cause — trying to make a value-typed thing and a reference-typed thing co-exist with shared interface dispatch but distinct storage policies. Lifting the split dissolves the symptoms.

---

## The model

### Single keyword

```cajeta
public class MyClass {
    int32 field;
    public MyClass(int32 v) { this.field = v; }
    public int32 method() { return this.field; }
}
```

There is no `struct` keyword in v2. The `struct` keyword retires (with a one-cycle deprecation alias during migration; see § Migration). `class` is the only type-declaration form for non-view non-primitive types.

### Allocation as expression prefix

Two prefix forms convert a constructor call (or aggregate-init expression) into an allocation:

- **`stack ClassName(args)`** — allocate on the current frame; lifetime tied to the enclosing scope.
- **`heap ClassName(args)`** — allocate on the heap (`malloc`); ownership transfers per `#` semantics; freed at drop time.

Both are **mandatory**. Bare `MyClass(args)` is a compile error. Every allocation explicitly says where it lives.

```cajeta
MyClass a = stack MyClass(42);                  // stack, ctor
MyClass b = heap MyClass(7);                    // heap, ctor
MyClass c = stack MyClass { field: 100 };       // stack, aggregate-init
MyClass d = heap MyClass { field: 200 };        // heap, aggregate-init
MyClass e = stack MyClass();                    // stack, default ctor
MyClass f;                                       // null reference (see § Definite assignment)
```

### `new` removed

The `new` keyword has been removed. `heap MyClass(args)` is the sole heap
allocator (`stack` and `shared` are the other placements). Writing `new`
produces a parse error.

### Storage-agnostic type system

`MyClass` is one type whether stack- or heap-allocated. A variable of type `MyClass` is a class reference (pointer to a class instance); the type system doesn't distinguish where the instance lives. Functions take `MyClass` and accept either:

```cajeta
public void inspect(MyClass m) { ... }   // accepts stack or heap

MyClass a = stack MyClass();
MyClass b = heap MyClass();
inspect(a);  // OK
inspect(b);  // OK
```

The borrow checker handles lifetime as metadata — stack instances can't outlive their frame, heap instances can.

### Pointer-only pass and return

**Class instances always pass and return by pointer, never by value.** This is the load-bearing safety decision; it sidesteps C++'s object slicing entirely.

- Pass `MyClass m` → pointer to the instance.
- Return `MyClass` → pointer to the instance.
- Assign `MyClass b = a` → pointer alias (a borrow).
- `MyClass b = #a` → ownership transfer (a's drop entry deactivated; b takes over).

There is no by-value copy of a class instance. If you want a copy, write an explicit copy factory or aggregate-init from another instance.

### Vtable slot at offset 0 on every class instance

Every class instance — stack or heap — has its vtable pointer at byte offset 0 of its body, followed by fields. Dispatch is uniform across storage modes. The 8-byte cost per stack instance is acceptable for unified dispatch; future optimization could elide it for classes with no virtual methods (e.g., via a `@NoVTable` annotation), but that's out of scope for v1.

### Return-by-value for stack-allocated factory returns (RVO / sret)

A static factory like `Optional<T>.Some(v)` that returns `stack Optional { ... }` would normally escape its frame. Cajeta's existing S6.7 / S9.5.5 sret machinery makes this safe: **the caller pre-allocates the return body in its own frame and passes the slot to the factory**, which writes into the slot. The bytes never live in the factory's frame, so no escape. No heap allocation.

```cajeta
public static Optional<int32> Some(int32 v) {
    return stack Optional { present: true, value: v };   // writes into caller's sret slot
}

void caller() {
    Optional<int32> opt = Optional<int32>.Some(42);      // opt is the sret slot
}
```

This works for any stack-allocated class returned from any function; not specific to Optional.

---

## Inheritance

### Single-inheritance for state, multiple-inheritance for behavior

Cajeta supports `class C extends A, B` (verified across grammar, visitor, type resolver, vtable construction, and tests). Multiple inheritance has been part of the language since before the v2 pivot; the v2 unification preserves it.

```cajeta
public abstract class AbstractStream<T> { ... }
public abstract class AbstractHashable<T> { ... }

public class Optional<T> extends AbstractStream<T>, AbstractHashable<T> {
    // Inherits from both bases.
}
```

### Stack and heap polymorphism are identical

Stack-allocated classes that participate in inheritance work the same as heap-allocated ones:

```cajeta
public class Shape { public int32 area() { return 0; } }
public class Square extends Shape {
    int32 side;
    public Square(int32 s) { this.side = s; }
    public int32 area() { return this.side * this.side; }
}

Shape s1 = stack Square(5);   // stack Square as a Shape reference
Shape s2 = heap  Square(7);   // heap Square as a Shape reference
int32 a1 = s1.area();         // 25 (Square.area via vtable)
int32 a2 = s2.area();         // 49
```

No slicing occurs because no bytes are copied. `s1` and `s2` are pointers; vtable slot 0 is Square's vtable; dispatch lands on the override.

### Diamond resolution via hash-based vtable

Cajeta's vtable lookup is hash-based by canonical method signature (`__cajeta_vtable_lookup` at runtime), chosen specifically to handle multi-inheritance without slot-index collisions. The same machinery makes diamond inheritance work:

```cajeta
class A { public int32 m() { return 1; } }
class B extends A { public int32 m() override { return 2; } }
class C extends A { public int32 m() override { return 3; } }
class D extends B, C { public int32 m() override { return 4; } }
```

D's vtable contains D::m (its override); calls through any reference type land correctly. Common-ancestor state (A's fields) is shared, not duplicated.

### Abstract classes for combinator DRY

Default methods on interfaces are **not** part of the v2 model. Instead, abstract classes provide concrete method bodies that subclasses inherit:

```cajeta
public abstract class AbstractStream<T> {
    public abstract Optional<T> next();
    public <U> Stream<U> map((T) -> U fn) { ... }        // concrete; inherited
    public Stream<T>     filter((T) -> boolean p) { ... }
    public int32         count() { ... }
}

public class ArrayStream<T> extends AbstractStream<T> {
    public Optional<T> next() { ... }
    // Inherits map / filter / count / etc.
}
```

Multi-inheritance lets a class extend multiple abstract bases for behavior from each (e.g., `class Optional<T> extends AbstractStream<T>, AbstractHashable<T>`).

### `@Override` annotation

Optional but recommended on methods that override a concrete inherited method (especially with covariant return types). Lint warning when overriding without it — catches typo'd signatures that silently create new methods instead of overriding.

```cajeta
public class Optional<T> extends AbstractStream<T> {
    @Override
    public <U> Optional<U> map((T) -> U fn) { ... }   // covariant: Optional<U> narrower than Stream<U>
}
```

### Covariant return types

An override may declare a narrower return type than the base method:

```cajeta
abstract class AbstractStream<T> {
    public <U> Stream<U> map((T) -> U fn) { ... }
}

class Optional<T> extends AbstractStream<T> {
    @Override
    public <U> Optional<U> map((T) -> U fn) { ... }   // Optional<U> is-a Stream<U>
}
```

At a concrete-receiver call site (`opt.map(...)`), the type system sees `Optional<U>`. At a base-typed call site (`Stream<T> s = opt; s.map(...)`), the type system sees `Stream<U>`. Standard Java feature.

---

## Memory model

### Drop chain

Same machinery as v1, unified across stack and heap:

- **Stack-allocated class:** at scope exit, the drop chain runs the class's destructor (any owned class refs are recursively dropped), then the stack frame teardown reclaims the body bytes. No `free()`.
- **Heap-allocated class:** at scope exit, the drop chain runs the destructor (recursive drops as above), then `__cajeta_free` releases the heap bytes.

A class's `getOrCreateDropFunction()` walks owned class-ref fields in reverse declaration order, just like v1 did for both struct and class. The only thing that changes between stack and heap drop is the final reclamation step.

### Ownership and `#` move

The `#` operator transfers ownership of a class instance:

```cajeta
MyClass a = heap MyClass();
MyClass b = #a;                   // b takes ownership; a's drop entry deactivated
// `a` is moved-from; subsequent reads of `a` are a compile error (CAJETA_ERROR_USE_AFTER_MOVE)
```

For stack instances, `#a` deactivates `a`'s drop entry — the instance still lives in the frame's stack region until function exit, but its drop fn doesn't run when `a` goes out of scope (because `b` now owns it). When `b` drops, the destructor fires; the stack bytes are reclaimed at frame teardown regardless.

For heap instances, `#a` deactivates a's drop entry — the heap pointer transfers to `b`; when `b` drops, the destructor fires + the heap is freed.

### Escape check

A function returning a stack-allocated class instance through its return type would dangle the caller a pointer into the dying frame. Caught at compile time:

```cajeta
MyClass leak() {
    MyClass x = stack MyClass();
    return x;          // COMPILE ERROR: CAJETA_ERROR_BORROW_ESCAPE
}

MyClass ok() {
    return heap MyClass();   // OK — heap pointer escapes freely
}
```

Same `Scope`-based escape check generalizes from v1's struct-rooted-interface case (S10.3) to all stack-allocated class returns.

### Interface fat pointers carry kind tags

Interface-typed values use the 24-byte fat pointer model from S9.5: `{ data_ptr, vtable_ptr, kind_tag }`. The v1 kind tags rename for clarity but the values stay the same:

- `BORROWED_STACK` (was `BORROWED_STRUCT`) — data points at a stack-allocated class instance; can't escape the frame.
- `BORROWED_HEAP` (was `BORROWED_CLASS`) — data points at a heap-allocated class instance; borrowed (no ownership).
- `OWNED_HEAP` (was `OWNED_CLASS`) — data points at a heap-allocated class instance; the iface value owns it; drop chain frees on scope exit.

The `__cajeta_iface_drop` helper dispatches by kind tag, same as S10.4.

---

## Definite-assignment analysis

`MyClass x;` declares a class reference variable but does not allocate or assign. The variable is **not-yet-assigned** (NYA) until something assigns it. Reading a NYA variable is a **compile-time error**, not a runtime null-deref.

```cajeta
MyClass x;
x.method();           // COMPILE ERROR: variable `x` may not have been initialized
```

The forward-flow analysis follows Java JLS §16 in shape. A variable transitions to **definitely-assigned** (DA) after an assignment; reads downstream require DA.

### What counts as assignment

- `x = expr;` direct assignment in any form (stack/heap construction, borrow, `#` move, expression result)
- `x = null;` explicit null binding (`x` is DA — its value is null, but the variable has been assigned)

### What does NOT count

- Passing `x` as a method argument (cajeta has no out-params; methods receive a pointer but can't rebind caller's local)
- Reading a field of `x` (`x.field`) — this REQUIRES `x` to be DA already
- Method call `x.m()` — also requires `x` to be DA

### Loops

A variable assigned only inside a loop body is **not** DA after the loop. The loop may not execute (`for(...; 0 < 0; ...)`, empty iterator, false `while` condition), or may exit via `break` before the assignment runs.

```cajeta
MyClass x;
for (int32 i = 0; i < n; i = i + 1) { x = stack MyClass(); }
x.method();           // COMPILE ERROR: loop may not have assigned x
```

Workaround: assign before the loop.

```cajeta
MyClass x = stack MyClass();
for (int32 i = 0; i < n; i = i + 1) { x = stack MyClass(); }
x.method();           // OK
```

(Provably-infinite-with-DA-at-every-exit loops would technically make a variable DA-after via Java's full rule, but v1 doesn't bother with the additional analysis. Users can assign before the loop.)

### Conditional (`if/else`, `switch`)

A variable is DA after an `if/else` iff it is DA in BOTH branches. Missing `else` → at most one branch assigns → not DA.

```cajeta
MyClass x;
if (cond) { x = stack MyClass(); } else { x = heap MyClass(); }
x.method();           // OK — DA in both branches

MyClass y;
if (cond) { y = stack MyClass(); }
y.method();           // COMPILE ERROR: else branch doesn't assign
```

A variable is DA after a `switch` iff it is DA in every case AND either there is a `default` case that assigns OR the switch is exhaustive over an enum.

### `try/catch`

A variable assigned in `try` is NYA in `catch` (the assignment may have thrown before completing). DA after `try/catch` iff DA in both the `try` body AND every `catch` block.

```cajeta
MyClass x;
try {
    x = factoryThatMayThrow();
} catch (Exception e) {
    x = stack MyClass();        // catch also assigns
}
x.method();           // OK — DA after both paths
```

### Unreachable code

After `return`, `throw`, `break`, `continue`, any subsequent reads are unreachable; trivially DA for everything (the analysis doesn't have to prove anything about unreachable paths).

### Constructor field initialization

Class fields follow **Java semantics**: all fields zero-initialized (null for class refs, 0 for primitives, false for booleans) when the instance is allocated. Constructors may leave fields untouched — they stay zero/null. **No static check** is performed on field init in constructors.

This is intentional asymmetry with locals: locals require explicit init (definite-assignment) because a NYA local has no meaningful value; class fields have a meaningful default (zero/null) and the construction protocol guarantees the default is in place before any constructor body runs.

### What's not covered

- Out-params — cajeta doesn't have them; if added, the rule extends additively (passing `out x` to an `out`-declared parameter makes `x` DA after the call).
- `goto` — cajeta doesn't have it.
- Coroutines / yielding — out of scope for v1 of the unified-class rollout.

---

## Migration from v1

The v1 `new` and `struct` keywords have both been removed — each is now a parse error.

### `struct` keyword

```cajeta
public struct Foo { int32 x; }       // v1 — parse error in v2
public class  Foo { int32 x; }       // v2
```

The `struct` keyword has been removed from the lexer and parser. v1 `struct` declarations must be rewritten as `class` declarations; the resulting class instance can be allocated with `stack ClassName(args)` (matching v1's lifetime model) or `heap ClassName(args)`.

### `new` keyword

The v1 `new` keyword has been removed from the lexer and parser; `new ClassName(args)` is now a parse error. Rewrite it with `heap` (or `stack`):

```cajeta
MyClass x = heap MyClass();
```

### Bare `MyClass(args)` was never legal

In v1, constructor calls always went through `heap ClassName(args)`. Bare `ClassName(args)` was either a parse error or routed to view-construction syntax. v2 keeps bare `ClassName(args)` as a compile error — every allocation says where (`stack` or `heap`).

### What changes for existing code

- Every `heap MyClass()` should be reviewed and migrated to either `heap MyClass()` (preserves behavior) or `stack MyClass()` (often better — avoids heap traffic).
- Every `struct Foo` declaration becomes `class Foo`. Existing struct fields, methods, drops, interface impls all carry forward.
- Tests using `MyClass x;` (Java-style null) continue to work, but `x.method()` immediately after will now be a compile error (was a runtime null-deref). Code paths that assumed lazy initialization may need an explicit `MyClass x = null;` or an `if (x != null)` guard.
- Code that used `Foo(bytes)` view-construction syntax stays unchanged — view construction is unrelated.

### What doesn't change

- `view` and `view`-construction syntax — fully unchanged.
- `T[]` primitive — unchanged. (`T[].stream()` becomes a compiler intrinsic; no existing code is affected.)
- Memory model operators (`#`, drop chain, path-borrow tracker) — unchanged.
- Method dispatch for direct calls vs interface dispatch — unchanged; the existing fat-pointer machinery generalizes.
- Hash-based vtable + multi-inheritance — unchanged.

---

## Implementation roadmap

The unified-class rollout is a multi-session effort. Sessions sized comparable to S6-S11. Status tracking lives in `ToDo.md`; this section gives the rough sequencing.

### Phase 1 — Syntax and keyword unification

- New `heap` / `stack` keywords; expression-prefix grammar; visitor; codegen.
- `new` keyword deprecation alias (warns; treated as `heap`).
- `struct` keyword deprecation alias (warns; treated as `class`).
- Bare `MyClass(args)` rejected at parse time.
- Existing tests stay green via the deprecation aliases.

### Phase 2 — CajetaStruct collapse into CajetaClass

- Merge `CajetaStruct`'s aggregate-init, drop chain, and method machinery into `CajetaClass`.
- Generalize stack-allocation path (was struct-only) to any class.
- Vtable slot at offset 0 on every class instance, regardless of allocation site.
- Migrate S6-S11-era tests from `Struct*Tests` files into `Class*Tests` (or merge into existing class test suites).
- Drop `isAggregate` skip branches in dispatch — every class uses the uniform vtable path.

### Phase 3 — Pointer-only pass/return + escape generalization

- Class returns by pointer (heap returns) or via sret slot (stack returns; RVO).
- Class parameters always by pointer (already true; confirm and pin).
- Escape check generalized from S10.3 (struct-rooted iface) to any stack-allocated class returned through a non-RVO path.
- Interface fat-pointer kind tags renamed: `BORROWED_STRUCT` → `BORROWED_STACK`, `BORROWED_CLASS` → `BORROWED_HEAP`, `OWNED_CLASS` → `OWNED_HEAP`.

### Phase 4 — Definite-assignment analysis

- New forward-flow pass tracking DA/NYA per local in each `Scope`.
- Rules: assignment forms, loops, conditional, switch, try/catch, unreachable code.
- Diagnostics: `CAJETA_ERROR_VARIABLE_NOT_ASSIGNED` with the variable name + the suspect read site.
- Tests covering the rule matrix.

### Phase 5 — Covariant return types

- Relax signature compatibility check from "exact return type match" to "implementer return type assignment-compatible with base."
- Test pin: subclass overrides a base method with a narrower return; concrete-receiver call sites see the narrower type, base-receiver call sites see the wider type.

### Phase 6 — Live-borrow tracker for iterator invalidation

- Extend `Scope`'s path-borrow infrastructure from "moved paths" to "paths currently live-borrowed by some still-in-scope owner."
- Registration at struct-field-binding-from-borrow sites (a stream borrowing from a collection registers the collection's path).
- Release at borrower's scope exit / move-out.
- Check at every write site (assignment, index-store, mutating method call).
- New diagnostic: `CAJETA_ERROR_MUTATION_DURING_BORROW`.

### Phase 7 — Retire deprecation aliases

- `struct` keyword → parse error.
- `new` keyword → parse error.
- Migration documentation updated; existing code expected to have completed the cycle.

After Phase 7, the unified-class rollout is complete. The stdlib (`Optional`, `Stream`, collections, `Collector`, etc.) is a separate rollout on top — see the corresponding stdlib design doc when it lands.

---

## Conventions and out-of-scope items

### Conventions

- Each phase ends with full regression passing. No half-finished commits.
- Tests written against the deprecated keywords (`struct`, `new`) stay in the existing test files during the migration cycle; they're migrated in the cleanup phase.
- Documentation updates land with the phase that implements them, not as a separate phase at the end.

### Out of scope for this rollout (deferred or never)

- **Implicit copy/move constructors.** Movement is explicit via `#`; copies are explicit via factories or aggregate init.
- **Stack-allocated class fields inline in another class's heap layout.** Class fields are pointers to class instances. Views keep their inline-layout rules.
- **Default methods on interfaces.** Abstract classes + multi-inheritance cover the DRY case.
- **Object slicing on by-value pass.** Pointer-only pass/return makes this impossible.
- **Out-params.** Not in v1; rules extend additively if added later.
- **Pin / unmovable types.** Not needed for v1; revisit if a real consumer requires it.

### Known follow-ons

- **Live-borrow tracker** lands as Phase 6 of this rollout, before the stdlib rollout begins (so iterator invalidation is a compile-time error from the first stdlib day).
- **Class-array element-layout** (S7.5 / S10.5 in the v1 deferreds) is still open. Under the unified model, the question becomes: does `MyClass[]` store inline class values or pointer references? My lean is pointers — matches the pointer-only pass/return rule. Settle during Phase 2 or Phase 3.
- **Move-out-of-class-field** (S7.4 in the v1 deferreds) — moving out of `c.field` marks the path moved at compile time but doesn't clear the runtime pointer; the class drop walks the now-stale pointer and double-frees. Solution: either runtime drop consults a per-instance ownership bitmap, or move-out point writes null into the slot. Lands in Phase 3 or Phase 4.

---

## Related documents

- `docs/stdlib/MemoryModel.md` — the borrow / move / drop doctrine. v2 preserves all rules; only the storage-choice mechanism (stack vs heap) is new.
- `docs/stdlib/Views.md` — typed overlays onto byte buffers. Unaffected.
- `docs/history/StructsViewsStatus.md` — v1 struct/view rollout (S1–S12). Already archived; preserved for diagnostic context.
- `docs/history/ImplementationStatus.md` — v1 memory-model rollout. Already archived.
- `ToDo.md` — working tracker; the committed-decisions section near the top documents the design state this doc formalizes.
