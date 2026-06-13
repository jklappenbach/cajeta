# Plan: Multiple inheritance — collision safety + remaining gaps

Status: **Planned.** Spec: [`docs/stdlib/MultiClassing.md`](../../docs/stdlib/MultiClassing.md)
(proposals P-1…P-x + the "Cajeta today" gap list). This plan tracks the
*remaining* MI work; the core (below) already ships.

## Context

Multiple inheritance of **behavior** is implemented and tested: `class C extends
A, B`, per-parent sub-objects, a hash-keyed vtable, polymorphic dispatch through
any base, the `super<Base>.method()` / `this<Base>.field` parent selectors, and
the `@Override(from=Base)` annotation. The four scenarios in MultiClassing.md
"§ Cajeta today" are **not yet handled** — this plan closes them, headlined by
**collision safety**.

**Decided rule (P-1, confirmed):** an *unqualified* call to a method (or access
to a field) that is **ambiguous** — inherited from two parents with the same
signature/name and not overridden by the child — is a **compile error**, not a
silent arbitrary pick. The developer must resolve it explicitly: either override
in the child (and reach a specific parent via `super<Base>`), or **qualify the
call by the declaring class**. Today the compiler silently picks whichever the
hash lookup finds first — a latent bug this plan fixes.

> No compiler changes have been made yet — this is the tracked plan.

## 1. TDD

a. **Method-collision is an error (P-1)**

   1. [ ] `A` and `B` both declare `int32 kind()`; `class C extends A, B` does
      **not** override; an unqualified `c.kind()` is rejected with
      `CAJETA_ERROR_AMBIGUOUS_METHOD_DISPATCH`, the message naming both
      candidates and the two remediations. → `MultiClassingCollisionTests.unqualifiedAmbiguousCallIsError`.
   2. [ ] Remediation A — `C` overrides `kind()` and selects a parent via
      `super<A>.kind()` / `super<B>.kind()`: compiles, dispatches to C's
      override. → already pinned by `OverrideFromTests.fromMatchingAncestorAcrossMultipleInheritance`.
   3. [ ] Remediation B — a call-site qualified by the declaring class picks
      that parent's method: compiles and calls it. →
      `MultiClassingCollisionTests.canonicalClassQualifiedCallResolves`.

b. **Field-collision is an error (P-1, C-3)**

   1. [ ] `A` and `B` both declare `int32 total`; `c.total` →
      `CAJETA_ERROR_AMBIGUOUS_FIELD_ACCESS`; resolved by `this<A>.total` (in a
      method body) or a declaring-class-qualified access. →
      `MultiClassingCollisionTests.unqualifiedAmbiguousFieldIsError`.

c. **Diamond — defined behavior, not breakage**

   1. [ ] `D extends B, C` where user `B`, `C` both extend user `A`: the layout
      and dispatch are defined (replicated by default; sharing is opt-in, syntax
      TBD), not the current silent breakage. → `DiamondLayoutTests.*`.

## 2. Deliverables

a. [ ] **Ambiguous-method detection** — at the unqualified call site, if the
   resolved set has ≥2 distinct inherited impls and the receiver class declares
   no override, raise `CAJETA_ERROR_AMBIGUOUS_METHOD_DISPATCH` with a teaching
   message (candidates + both remediations). (`src/cajeta/` dispatch/vtable
   lookup; today it picks the first hash hit.)
b. [ ] **Call-site canonical-class qualification** — let the developer name the
   declaring class to select a parent's method/field at an arbitrary call site
   (the "full canonical class name" resolution). **Open syntax decision:**
   cast-to-parent `((A) c).kind()` (reuses the existing cast machinery) vs a
   receiver parent-view selector mirroring `super<Base>`/`this<Base>`
   (MultiClassing.md P-1 sketched `c[A]`, but a square bracket already parses as
   indexing — must pick a non-colliding form).
c. [ ] **Ambiguous-field detection** — `CAJETA_ERROR_AMBIGUOUS_FIELD_ACCESS`,
   same remediation set (and `this<Base>.field` already works inside bodies).
d. [ ] **Diamond shared-vs-replicated policy** — define + document the default
   (replicated, C++-style) and an opt-in share marker if wanted; fix the
   user-declared-diamond breakage. (MultiClassing.md gap #4.)

(Already shipped, the building blocks this plan completes: `super<Base>` /
`this<Base>` selectors, `@Override(from=Base)`, hash-keyed override aliasing —
see `OverrideFromTests`, `MultiClassingPhase{1,2,3}Tests`.)

## 3. Acceptance Criteria

a. [ ] An unqualified colliding method call or field access is a **compile
   error** with a message that teaches the fix — never a silent arbitrary pick.
b. [ ] Both remediations work: override + `super<Base>`, and call-site
   qualification by the declaring class.
c. [ ] A user-declared diamond has defined, documented layout/dispatch.
d. [ ] No regression in the existing MI suites.

## Open decisions

1. [ ] Call-site qualification syntax — cast-to-parent vs a parent-view selector
   (§2.b).
2. [ ] Diamond sharing — replicated-only, or an opt-in `shared`/`virtual` marker
   (§2.d).
