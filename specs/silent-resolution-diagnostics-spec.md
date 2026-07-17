# Spec: silent-resolution diagnostics

> Status: draft. Companion plan: `agents/silent-resolution-diagnostics-plan.md`.

## 1. Definition

The compiler **accepts unresolvable expressions and lowers them to `null`** instead of
rejecting them. A typo compiles; the failure surfaces at runtime (a zero, a null read, or
a SIGSEGV) far from its cause.

This is a *diagnostics* gap, not a type-system gap. The distinction is sharp and was
measured, not assumed:

- An unknown **declared type** IS diagnosed today. `NoSuchType x;` as a field raises
  `CAJETA_ERROR_UNKNOWN_TYPE`; as a local, `CAJETA_ERROR_UNRESOLVED_TYPE`.
- An unresolvable **expression or member** is NOT. It silently becomes `nullptr` in
  codegen, and `ReturnStatement` (`src/cajeta/asn/Statement.cpp:2022`) turns that into a
  `cerr` line — `[cajeta] return value lowered to null` — plus a `ret null`. **A warning on
  stderr, and a successful compile.**

The machinery to do this right already exists in-tree and is applied inconsistently:
`MethodCallExpression` throws a real located error for a missing method on a `Vector`
or `Matrix` receiver (`"Vector has no method '...'"`), but the ordinary class-receiver
path just returns `nullptr` — one of **28** silent `return nullptr` sites in that file.

### Why now
Three independent instances surfaced within a single day of work on `source-synthesis`
(Units 3 and 5), each confirmed against **hand-written** code with `cajeta jit-run` — none
was caused by source synthesis. The plan item `source-synthesis` 5.1.2 ("a call-site typo is
an ordinary member-not-found compile error") is **blocked** on this and is parked at `[~]`.

### Non-goals
- Not a new type system, inference pass, or borrow-check change.
- Not a rewrite of expression codegen. The fix is to **reject** what is already unresolvable,
  at the point it is already known to be unresolvable.
- Not the "no-op drop"/perf family, and not `nucleo`.

## 2. Feature: member-not-found is an error (SRD-1)

Today `p.volme()` on a plain, hand-written, non-generic class compiles.

**Use cases**
- **2.1** As a developer, when I misspell a method on a class receiver (`p.volme()`), then I
  get a located compile error naming the method and the receiver's type — the same shape the
  `Vector`/`Matrix` receivers already produce — not a silent `null`.
- **2.2** As a developer, when I misspell a *field* (`p.volme`), then I get the same located
  error, at the field's span.
- **2.3** As a developer, when a call is genuinely unresolvable because an **overload doesn't
  match** (right name, wrong argument types), then the error says so and lists the candidate
  signatures — a missing overload must not be reported as "no such member".
- **2.4** As a compiler maintainer, when any expression's `generateCode` yields `nullptr` in a
  value position, then the compile **fails** with a located diagnostic. `null` in a value
  position is never a legal lowering, so it is a compiler invariant, not a user error — the
  `cerr` warning at `Statement.cpp:2022` becomes an error, and it names the expression that
  produced it, not just the enclosing method.
- **2.5** As a developer, when I call a synthesized member (`source-synthesis` Unit 5's
  `Columns<Tick>.price()`), then a typo (`prce()`) is diagnosed exactly like a hand-written
  member's — synthesized members are ordinary members, so they inherit this for free. *(Closes
  `source-synthesis` 5.1.2.)*

## 3. Feature: initializers are checked (SRD-2)

**Use cases**
- **3.1** As a developer, when a static field's initializer calls an unresolvable type
  (`static int32 x = NoSuchType.nope();`), then I get a located compile error. Today it
  compiles clean.
- **3.2** As a developer, when an initializer's type doesn't match the declared type
  (`static int32 x = "hello";`), then I get a located type-mismatch error. Today it compiles
  clean.
- **3.3** As a developer, the same holds for an **instance** field initializer and for a local
  variable initializer — the check is on the initializer, not on where it happens to live.

## 4. Feature: the diagnostics are usable (SRD-3)

**Use cases**
- **4.1** As a developer, every error from §2 and §3 carries a **file, line, and column** at
  the offending expression (the located-`Exception` surface Cajeta already has), and survives
  into the JSON diagnostic format.
- **4.2** As a developer, when I misspell a member that exists on the receiver under a *near*
  name, the error suggests it ("did you mean `volume`?"). Cheap edit-distance over the
  receiver's member names; this is the single highest-value part for day-to-day use.
- **4.3** As a maintainer, no diagnostic added here names a compiler-internal artifact (a
  throwaway synthesis wrapper, an intrinsic's mangled symbol).

## 5. Safety property / acceptance

The regression risk is **the whole existing test suite**: any code that today relies —
knowingly or not — on an unresolvable expression quietly lowering to `null` will start
failing to compile. That is the point of the change, but it means the work is only done when
the full suite is green, and every newly-surfaced failure is triaged as either
(a) a real latent bug the new diagnostic just found, or
(b) a false positive in the new check.

Both outcomes are valuable; (a) must be fixed or filed, never suppressed by weakening the
check.
