# Records — Specification

> Status: draft for review (2026-06-23). A **language feature** (the reserved `record`
> keyword), driven by núcleo; may graduate to `docs/specification/lang/` when implemented.
> Layer-1a foundation. Companion design: `language-foundations.md` §2.
>
> This is a **spec** — requirements and use cases (the *why/what*). Build decisions (the
> *how*) are deferred as `> **TBD (plan-time):**` markers and collected in §9, to be resolved
> when this spec is turned into a plan. Outline-numbered for addressability.

## 1. Definition

### 1.1 Purpose
A **record** is a named, instantiable, **value-type** aggregate of typed fields — an
*instantiable value class with no vtable* (no virtual dispatch, no interfaces, no reference
identity), **immutable by default**. Records may compose and reuse one another **statically**
(vtable-free); anything that needs a vtable is a class, not a record. Records exist to give
Cajeta a first-class way to model *structured
data shapes* — typed multi-value returns, named tuples, and (crucially for núcleo)
**compile-time dataframe/tensor schemas**. They are the data-modeling primitive the rest of
núcleo's typed surface stands on.

**Lineage (why the name and shape benefit developers).** `record` deliberately mirrors the
established cross-language construct: Java `record`, C# `record`, F# records, Scala case
classes, Kotlin data classes — all *immutable, named-typed-field data carriers with value
equality and copy-update* (C#/F#'s `with` is exactly our copy-with, §3.3). It also matches the
**database** sense of a *record* as a typed row of a table — which reinforces the `Table<Tick>`
model directly. Both intuitions map onto Cajeta's two uses: a record *as a value* (a `Vec3`, a
`QrResult`, one materialized row) and a record *as a `Table<T>` schema* (type-level).

### 1.2 Scope
- A new declaration form: `record Name { Type field; ... }`.
- Value semantics, immutable-by-default, construction, **instantiation**, field access, and
  **pure methods / operator overloads** (direct dispatch, no vtable).
- Use as a **type argument** carrying a schema (`Table<Tick>`, a typed return bag).
- Reflection over a record's fields (reusing existing `Class<T>`/field enumeration).

### 1.3 Non-goals
- **Anonymous / structural record types** (`{ts: Instant, price: f64}` inline). Named records
  + reflection deliver the same power with a name; structural typing is explicitly out.
- **Sum types / tagged unions / variant-carrying enums** — separate feature, not records.
- **Mutation by default** — records are **immutable by default** (see §3); mutation is an
  explicit opt-in, never implicit.
- **Vtables / virtual dispatch / interfaces / runtime polymorphism** — a record has **no
  vtable**. It never implements an interface and is never dispatched polymorphically through a
  base type. If you need runtime polymorphism (interface dispatch, a heterogeneous base-typed
  collection), use a **class**. *(Static, vtable-free composition/reuse between records is
  allowed — §2.6.)*
- **Holding a reference-type class** — a record's fields are **value types only** (primitives,
  records, value-type aggregates); a record may not contain a heap/identity class. Composition
  is one-way: a class may contain records, never the reverse (§2.6).

### 1.4 Relationship to existing constructs
- `record` and `structure` are already **reserved words, currently unimplemented** — this spec
  fills that slot.
- Cajeta already has value-type classes (`@ValueType`), an aggregate initializer
  (`Foo { field: value }`), monomorphization, operator overloading, and reflection (`Class<T>`,
  field enumeration). A `record` **lowers to a `@ValueType` class with no virtual methods** —
  which is precisely a class with **no vtable** — reusing existing value-layout machinery rather
  than introducing a new type kind.
- The existing `cajeta.math` value types (`Vec3`, `Quat`, `Color`, `Matrix<T,R,C>`) are already
  *de facto* records — `@ValueType` classes with methods/operators and no vtable; `record`
  gives that pattern a named surface.

> **Resolved:** [F6] A `record` **lowers to a `@ValueType` class with no virtual methods** —
> i.e. a class with **no vtable** and no per-instance header. It is *not* `@Sealed`: records
> remain statically composable (§2.6), and sealing would block the vtable-free reuse we *do*
> want. **`record` and `@ValueType` are co-equal, first-class surfaces** (decided 2026-06-24):
> `record` is **convenience sugar over the `@ValueType` machinery with extra constraints**
> (immutable-by-default, value-only fields, no-override static inheritance, no interfaces/virtuals,
> synthesized `with`); plain `@ValueType` stays available for value types that need what records
> forbid (e.g. implementing an interface, or compiler-intercepted types like `Matrix`).

## 2. Declaration and structure

A record declares a name and an ordered set of typed fields:
```cajeta
record Tick { Instant ts; float64 price; float64 size; Symbol venue; }
```

**Use cases**
- **2.1** As a developer, when I declare `record Point { float64 x; float64 y; }`, then `Point`
  is a usable named type with two typed fields in declared order.
- **2.2** As a developer, when I give a record a field whose type is itself a record, then
  nesting is permitted and the field is a value-typed sub-aggregate.
- **2.3** As a developer, when I declare a record with a template parameter
  (`record Pair<A, B> { A first; B second; }`), then the record monomorphizes per type argument
  like any other generic.
- **2.4** As a developer, when I declare **pure methods and operator overloads** on a record
  (e.g. `Vec3.length()`, `Vec3 + Vec3`), then they are permitted and dispatch **directly**
  (monomorphized, no vtable) — a record carries behavior like a class, just without virtual
  dispatch (see §2.5). What is rejected: virtual/abstract methods, implementing an interface,
  mutable instance state beyond fields, and reference-type (class) fields (§2.6).

> **Resolved:** Records **may** carry pure methods and operator overloads (the `Vec3`/`Quat`
> pattern) and may compose other records **statically** (§2.6); they may **not** carry virtual
> methods, implement interfaces, hold reference-type class fields, or be dispatched
> polymorphically — anything needing a vtable is a class, not a record.

### 2.5 A record is an instantiable value class

A record is **used like a class — minus the polymorphism machinery.** You instantiate it, hold
it, pass it, return it, store it in arrays/columns; it may carry pure methods and operators.
What it drops is exactly what buys the flat value layout the columnar engine depends on.

**Use cases**
- **2.5.1** As a developer, when I instantiate a record and call its methods/operators
  (`var v = Vec3 { x: 1.0, y: 2.0, z: 3.0 }; v.length(); v + w`), then it behaves like a class
  value — construction, methods, operators — with every call dispatched **directly** (no vtable).
- **2.5.2** As a compiler author, when a record has no inheritance and no virtual dispatch, then
  it carries **no per-instance type header** — a record of plain numeric fields is *just the
  field bytes*, which is what makes a non-null record column bit-identical to a tensor buffer
  (§7.2).
- **2.5.3** As a developer, when I compare or copy records, then they use **value (structural)
  equality and by-value copy** — two records with equal fields are equal; there is no reference
  identity.
- **2.5.4** As a developer, when I attempt to declare a **virtual method**, **implement an
  interface**, or use a record **polymorphically through a base type**, then the compiler rejects
  it — those need a vtable, and a record has none (use a class). *(Static composition/reuse
  between records is a different thing, and it is allowed — §2.6.)*

> **Resolved:** Records do **not** implement interfaces. Interface dispatch needs a vtable/itable
> (boxing), which forfeits the flat-bytes layout the columnar engine relies on — *if you need a
> vtable, use a class.* This removes the boxing boundary outright rather than managing it.

### 2.6 Composition and the class boundary

Records compose in vtable-free, value-type-preserving ways. Inheritance follows the **C++
non-virtual model**: methods are statically dispatched (resolved at compile time on the static
type — `obj.foo()` lowers to a direct call `foo(&obj)`), so a record hierarchy carries no vtable
and no per-instance header. **Overriding — not having methods — is the vtable line.**

**Record ↔ record (static, non-virtual inheritance).**
- **2.6.1** As a developer, when I derive one record from another
  (`record TradeTick : Tick { float64 commission; }`), then `TradeTick` **is-a** `Tick` and
  inherits its fields and its **non-virtual** methods statically — no vtable, no header,
  flat-bytes layout preserved, and `Table<TradeTick>` has all of `Tick`'s columns plus
  `commission`.
- **2.6.2** As a developer, when a derived record tries to **override or shadow** an inherited
  method (redefine one the base already declares), then the compiler rejects it — overriding is
  the only thing that would need a vtable. A derived record may **add** new fields and new
  methods; it may not **redefine** inherited ones. (Need overriding with runtime dispatch → use
  a class.)
- **2.6.3** As a developer, when I expect to put mixed record subtypes in one collection and
  dispatch over them **polymorphically**, then that is **not** supported — it needs a vtable, so
  it is a class job. Record inheritance is for *reuse and is-a modelling at compile time*, not
  runtime polymorphism.

> **Resolved (provisional):** record hierarchies use **is-a static subtyping**
> (`record TradeTick : Tick`, non-virtual) — a derived record *is a* base record, statically,
> so it can be passed where the base is expected (upcast). Consequence to define: because records
> are **values**, upcasting to the base **slices** — a `Tick` copy of a `TradeTick` drops the
> derived fields. Dispatch is non-virtual, so there is *no* wrong-override surprise (the static
> type always decides the method) — the only effect is the value-slicing data drop. Plan-time
> detail: whether upcast-with-slicing is implicit or requires an **explicit** cast (lean:
> explicit, to keep the data drop visible).

**Record ↔ class (one-way).**
- **2.6.4** As a developer, when I put a **record field inside a class**, then it is allowed —
  the class holds the record's value data inline.
- **2.6.5** As a developer, when I try to put a **reference-type class field inside a record**,
  then the compiler rejects it — a record holds **only value types** (primitives, records,
  value-type aggregates). A heap/identity class field would break the flat-bytes layout (the
  field becomes a pointer, breaking the column == tensor-buffer invariant) and value-copy
  semantics (copying the record would alias the shared object). Composition is one-way: classes
  contain records, never the reverse.

## 3. Value semantics and immutability

**Use cases**
- **3.1** As a developer, when I assign a record value to another variable or pass it to a
  function, then it is copied by value (no shared heap reference), consistent with Cajeta's
  value-type and deterministic-memory model.
- **3.2** As a developer, when I attempt to assign to a field of a (default) immutable record
  instance (`tick.price = 1.0`), then the compiler rejects it — "changing" an immutable record
  means constructing a new one.
- **3.3** As a developer, when I want a modified copy, then a compiler-provided copy-with form
  (e.g. `tick.with(price: 1.0)`) yields a new record with one field replaced and the rest
  copied (mirrors C#/F# `with`).
- **3.4** As a developer, when I explicitly opt a record into mutability, then its fields may be
  written in place — the exception, not the default; this is what a write-through `Table` view
  uses to update columns (`nucleo-frame-spec.md`).

> **Resolved:** [F5] Records are **immutable by default** (matching the Java/C#/F#/Scala record
> convention and value-semantics cleanliness), with **explicit opt-in mutation**. Copy-with
> (`with(...)`) serves the immutable case. Plan-time detail: the exact opt-in *surface* (a `mut`
> qualifier on the declaration vs. per-field) and the precise `with` spelling — but
> immutable-default-with-opt-in is fixed.

## 4. Construction

**Use cases**
- **4.1** As a developer, when I construct `Tick { ts: t, price: p, size: s, venue: v }`, then
  the named aggregate initializer binds each field by name (order-free), reusing Cajeta's
  existing aggregate-init syntax.
- **4.2** As a developer, when I construct a record positionally (`Point { 1.0, 2.0 }`), then
  fields bind in declaration order.
- **4.3** As a developer, when I omit a field at construction, then the compiler errors unless
  that field has a declared default.
- **4.4** As a developer, when a field has a declared default value, then omitting it at
  construction uses the default (consistent with default parameters elsewhere in the language).

> **TBD (plan-time):** Whether positional construction is supported in addition to named, and
> whether field defaults are in v1.

## 5. Field access and typed multiple-return

**Use cases**
- **5.1** As a developer, when I read `point.x`, then I get the typed field value; an unknown
  field name (`point.z`) is a compile error (not a runtime lookup).
- **5.2** As a library author, when I return a record from a function
  (`QrResult qr(Matrix a)` returning `QrResult { q, r }`), then the caller accesses
  `res.q`/`res.r` with full types — replacing scipy-style positional tuple bags.
- **5.3** As a developer, when destructuring sugar exists, then `var (q, r) = qr(a)` binds the
  record's fields to locals. *(Separable ➕ sugar — records work via field access without it.)*

> **TBD (plan-time):** [F8] Destructuring syntax, if/when added — positional `(q, r)` vs.
> by-name `{q, r}`. Not required for the capability; field access (5.1/5.2) suffices. This
> bracket choice is **shared with `syntax-sugar-spec.md` [S8]** and must be decided jointly.

## 6. Records as schemas (reflection + núcleo integration)

This is the load-bearing núcleo use: a record describes a **schema** that a generic consumes.

**Use cases**
- **6.1** As a núcleo author, when I write `Table<Tick>`, then the table's column set is derived
  from `Tick`'s fields — column `price` has type `float64`, etc. — known at compile time via
  reflection over the record.
- **6.2** As a developer, when I access a typed column (`ticks.price`), then it resolves to a
  `float64` column accessor and a typo (`ticks.prce`) is a compile error. *(The accessor
  generation rides the source-synthesis facility — see `source-synthesis-spec.md`.)*
- **6.3** As a núcleo author, when a record is used as a schema, then it is a **type-level
  descriptor only** — the physical storage is struct-of-arrays (one column buffer per field),
  not an array of record instances (see §7).
- **6.4** As a developer, when I enumerate a record's fields reflectively at compile time, then
  I get each field's name and type, enabling schema-driven code generation.

## 7. Physical model — record is the schema, storage is columnar

**Use cases**
- **7.1** As a núcleo author, when I build a `Table<Tick>`, then each field becomes a separate
  contiguous column buffer (SoA); a single `Tick` instance (one row) is the AoS view.
- **7.2** As a núcleo author, when a record field is a non-null numeric type, then its column is
  bit-identical to a tensor buffer (the column == tensor-buffer invariant; see
  `nucleo-column-spec.md`).

## 8. Acceptance criteria (spec-level)
- A record can be declared, constructed (named init), instantiated, and read with full
  compile-time typing.
- A record may carry pure methods and operator overloads, dispatched directly (no vtable); it
  does **not** implement interfaces and is never dispatched polymorphically.
- Field typos are compile errors; field mutation of a (default) immutable record is a compile
  error; an explicitly-mutable record permits in-place writes.
- A record lowers to a `@ValueType` class with **no virtual methods** (no vtable, no per-instance
  header); it may compose other records via **static (C++ non-virtual) inheritance** — inherited
  methods are statically dispatched and a derived record may **not** override or shadow them
  (overriding is the vtable line), never polymorphic dispatch.
- A record's fields are **value types only** (primitives, records, value-type aggregates); a
  record may not hold a reference-type class. A class may hold records — composition is one-way.
- A record can be used as a type argument whose fields are reflectable at compile time
  (enabling `Table<Tick>`).
- Value-copy and value (structural) equality semantics hold; no reference identity, no hidden
  heap aliasing.

> **Provisional (2026-06-23):** the no-vtable / static-composition / one-way-class-boundary
> rules in §1.3, §2.4–§2.6 are a deliberate *starting position*, not a settled invariant. The
> long-tail impacts are not yet known, and the exact mechanism for vtable-free composition is
> open (§2.6). We proceed with these to see where they lead and may revise as consequences
> surface. Treat them as the current working contract, not a guarantee.

## 9. Open questions (resolve at plan time)
- **[F5] — RESOLVED:** immutable by default, explicit opt-in mutation; copy-with for the
  immutable case (§3). Remaining plan-time detail: the opt-in *surface* and `with` spelling.
- **[F6] — RESOLVED:** lowers to a `@ValueType` class with no virtual methods (no vtable),
  **not** `@Sealed` (statically composable — §1.4/§2.6). Remaining detail: keyword-as-pure-sugar
  vs. recognized-distinct-surface (semantics fixed either way).
- **Methods / interfaces / inheritance — RESOLVED:** records may carry non-virtual methods and
  operators (§2.4/§2.5); inheritance is the **C++ non-virtual model** — static dispatch, derived
  may add but not override/shadow (§2.6). No interfaces, no virtual dispatch (no vtable — use a
  class for those).
- **Record composition mechanism — RESOLVED (provisional):** is-a **static subtyping**
  (`record TradeTick : Tick`, non-virtual, §2.6). Remaining detail: implicit vs. explicit upcast
  given value-**slicing** (lean explicit).
- **[F7] — RESOLVED:** **both**, distinct roles — `col.price` (expression builder, late-bound to
  an operation's input schema) and `ticks.price` (member access bound to a specific table), over
  the same typed columns, one synthesizer (`nucleo-frame-spec.md` §3/[F7]).
- **[F8]** Destructuring syntax, if/when added (§5.3) — shared with `syntax-sugar-spec.md` [S8].
- Positional construction and field defaults in v1 (§4).
