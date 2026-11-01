# Reified template capture — `Tensor<?>` → `Tensor<float32>` (spec)

> **Status: spec (requirements + the load-bearing design decisions).** This spec defines
> how a value whose static type is a template wildcard (`Foo<?>` / `Foo<? extends B>`) or a
> template supertype is recovered to a concrete instantiation (`Foo<float32>`) at
> runtime. The recovery is sound because cajeta monomorphizes. Its companions are
> `numeric-bounds-spec.md` (the bound side) and `docs/specification/math/tensor-spec.md`
> §2.1 (the airlock that needs this). It supersedes the deferred Phase-2.1+ items in
> `docs/CaptureConversion.md` for the explicit capture forms.

## 1. Scope & role
cajeta's templates are reified and monomorphized (`docs/Embedded.md`, `TemplateArgument`).
`Foo<float32>` and `Foo<int32>` are distinct concrete types, each carrying its template
arguments in its RTTI. A `Foo<?>` value is therefore never type-erased. At runtime it is
some concrete `Foo<X>`. This spec defines the language surface to recover that concrete
type, with a checked test, a checked cast, and a null-clean optional form.

This is the "airlock" the numerical stack needs. A `Tensor` loaded from a `.npy`, a
checkpoint, the interop protocol, or dynamic-dtype FFI arrives as `Tensor<?>`, with its
dtype known only at runtime. To operate on it with static types and monomorphized speed,
code captures it back to the concrete `Tensor<float32>`. It is also the general mechanism
for downcasting any reified template (`Stream<?>` → `Stream<int32>`, etc.), and `Tensor` is
just one user of it.

Implicit capture conversion is not in scope. That is Java's `capture#1` inference, where the
type-checker silently introduces a fresh capture variable at a method call. The explicit
forms below are the primitive. Implicit inference (`docs/CaptureConversion.md` Phases 2.1–2.3)
remains a separate, optional convenience layered on top and is not required by the airlock.

## 2. The load-bearing decision — capture is a *checked downcast*, sound by monomorphization
Java cannot do this safely. Erasure leaves nothing to check, so `(List<String>) listOfQ` is
an unchecked cast and a soundness hole (heap pollution). cajeta is the opposite. Because
each instantiation is monomorphized with distinct RTTI, recovering `Foo<float32>` from
`Foo<?>` is the exact analogue of `(Dog) animal`:

- **Representation is identical.** Every `Foo<X>` is a heap object (pointer) with its RTTI at
  slot 0, and `Foo<?>` is the same pointer at a wider static type. Capture is a pointer
  reinterpret, with no layout change and no copy.
- **The check is RTTI identity.** "Is this object's instantiation `Foo<float32>`?" is answered
  by comparing the object's reified template arguments, already recorded in
  `__cajeta_rtti_template_arg_*`, against the target instantiation. Primitives are included.
  The RTTI carries template-argument names for primitive args too (`TemplateArgument.getTypeName()`
  works for `int32`/`float32`), so the check does not depend on the unlanded REFL-8
  `Class.forName` path.

Capture is thus a conservative extension of the existing downcast/`instanceof` machinery to
instantiation-typed targets. It adds no new, destabilizing feature.

## 3. Surface (the three forms)
Let `w` have static type `Foo<?>` (or `Foo<? extends B>`, or a template supertype of
`Foo<float32>`).

1. **Checked test.** `w instanceof Foo<float32>` → `boolean`. True iff `w`'s reified
   instantiation is `Foo<float32>` (or a subtype, per §5). Null is `false`.
2. **Pattern-binding test.** `if (w instanceof Foo<float32> f) { … f … }` binds `f` with
   static type `Foo<float32>` in the true branch. This is the ergonomic primary form. It
   mirrors modern Java pattern instanceof, and here it is sound.
3. **Checked cast.** `(Foo<float32>) w` → `Foo<float32>`. It succeeds with the same pointer
   on a match. On a mismatch it throws a `RecoverableException` (a `ClassCastException`-family
   type), the same failure model as a bad reference downcast. Null casts to null.
4. **Optional form.** `w.tryAs<Foo<float32>>()` → `#Optional<Foo<float32>>`. It is present
   on a match and empty on a mismatch or null. This is the null-clean airlock path, specified
   as sugar over "test then cast".

Targets may be nested (`Foo<Bar<int32>>`), wildcard (`(Foo<?>) x` is a widening with no
runtime check), or bounded-wildcard (`(Foo<? extends Floating>) x` checks that the element
conforms to the bound, per `numeric-bounds-spec.md`).

## 4. Semantics
- **Match rule.** The runtime instantiation of `w` equals the target instantiation, compared by
  reified template-argument identity (element-wise, recursively for nested args). A non-`?`
  target with `?` args is illegal because it is ambiguous. Targets are concrete or bounded,
  never raw.
- **On success.** The value is the same object pointer, re-typed. There is no allocation and
  no field copy, only an O(1) RTTI compare. Storage, aliasing and ownership are unchanged, so
  capturing a `Tensor<?>` view yields a `Tensor<float32>` over the same `Storage`.
- **On failure.** Form (1)/(2) → `false` (no binding), form (3) → throw, and form (4) → empty.
- **Null.** `instanceof` → false, cast(null) → null, and `tryAs(null)` → empty. There is no NPE.
- **Subtype targets** (§5). The check is "is-a" rather than "exactly", so capturing to a
  concrete type through an intermediate template supertype is allowed.

## 5. Interaction with template subtyping
The match is an is-a test against the reified type, so it composes with:
- **Wildcard widening** already in the language (`Foo<float32>` ⊑ `Foo<?>`).
- **Bounded wildcards** (`numeric-bounds-spec.md`). `(Foo<? extends Floating>) w` succeeds iff
  `w`'s element type conforms to `Floating`. That is a bound check on the reified arg rather
  than an exact match. `Tensor<? extends Floating>` admits a captured `Tensor<bfloat16>` and
  rejects `Tensor<int32>`.
- **Declared template supertypes** (if `Bar<T>` extends `Foo<T>`). A `Foo<?>` that is really a
  `Bar<float32>` captures to `Bar<float32>` and to `Foo<float32>`.

## 6. Goals / Non-goals
**Goals:** explicit `instanceof` (+ binding), checked cast, and `Optional` `tryAs` over
instantiation-typed and bounded-wildcard targets, backed by reified RTTI identity; O(1),
allocation-free, no representation change; correct for primitive and class template args;
null-safe; the `Tensor<?>` → `Tensor<T>` airlock (`tensor-spec.md` §2.1) expressible and sound.

**Non-goals (v1):** implicit capture-conversion inference at call sites
(`docs/CaptureConversion.md` 2.1–2.3, separate and optional); a reflective `Class`-object cast
(needs REFL-8 `Class.forName`); capture that changes representation (there is none, since
monomorphized layouts are identical); `super`-bounded capture targets.

## 7. Acceptance criteria
1. `Foo<?>` (and `Foo<? extends B>`) values produced by widening, and recovered by all three
   forms, for primitive (`Tensor<float32>`) and class (`Box<String>`) args.
2. `instanceof` / binding-`instanceof` / checked cast / `tryAs` each behave per §3–§4:
   match → same pointer (verified: a write through the captured handle shows through the
   original); mismatch → false / throw / empty; null-safe.
3. Nested args (`Foo<Bar<int32>>`) and bounded-wildcard targets (`(Foo<? extends Floating>) w`)
   match per §5. `Tensor<? extends Floating>` admits float dtypes and rejects integer dtypes.
4. The `Tensor<?>` airlock round-trips: a `Tensor<?>` of runtime-unknown dtype captures to
   `Tensor<float32>` when (and only when) the reified dtype matches, sharing storage; mismatch
   fails cleanly (empty/throw) and is never UB.
5. Zero-cost: capture emits an RTTI compare + pointer reuse (no malloc, no element copy),
   verified in the lowered IR.
