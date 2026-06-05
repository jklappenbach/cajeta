# Method-templated static operators (K-generic matmul `a * b`)

> **RETIRED (owner decision 2026-06-04).** We will **not** support templated operators.
> Operator overloads are concrete (a specific LHS type × a specific RHS type); templatizing an
> operator is the wrong shape. Shape-generic `Matrix`/`Tensor` **matmul `*` is compiler-intrinsic**
> instead — the compiler-known value type lowers `M1 * M2` to matmul by reading `R/K/C` straight
> off the (already concrete) operand types in codegen, exactly like `Vector`'s operators. No
> templates, no inference machinery, no instantiation. Element-wise multiply is the method
> `a.hadamard(b)`. This document is kept only as the record of why the templated-operator path was
> investigated and rejected; **it is not a workstream.** The live plan is
> [`value-type-overloading-plan.md`](value-type-overloading-plan.md).

## Context

Owner decision (2026-06-04): support real templated static operators so a K-generic matrix
multiply `a * b` works for arbitrary conforming shapes — e.g. on `Matrix<T,R,C>`:
```
public static Matrix<T,R,C> operator* <uint32 K> (Matrix<T,R,K> a, Matrix<T,K,C> b)
```
where `K` (and `R`, `C`, `T`) are **inferred** from the operand types at the call site, then the
operator is **monomorphized** per concrete `(T,R,K,C)`. Consumed by `Matrix`/`Tensor` in
[`ValueTypeCatalog.md`](../cajeta-docs/ValueTypeCatalog.md); composes with the value-type
operator-overloading effort ([`value-type-overloading-plan.md`](value-type-overloading-plan.md)).

## Headline finding — the machinery is ~90% shipped

Method-level templates are **shipped** (`MethodLevelTemplate.md:630-646`, 2026-05-18):
instantiation, two-layer caching + mangling, `AlwaysInline`, and re-parse-with-substitution
all work, reached through `resolveMethod`'s template fallback
(`CajetaClass.cpp:3757-3770`). Operator dispatch already routes through that exact
`resolveMethod` call with `thisInstance=nullptr` (`BinaryOpExpression.cpp:696-701`). The
`final-or-static` gate (`CajetaLlvmVisitor.h:1139-1167`) is already satisfied by static
operators. Class templates already parse + carry non-type params (`uint32 K`) as
`CajetaConstantType` through the same cache-key path (`TemplateInstantiator.cpp:43-51,197-219`).

So the effort is **narrow**: (1) admit templated static operators in grammar+visitor; (2) teach
the unifier to infer + conformance-check **non-type** params. Everything downstream is reused.

## The two real gaps

1. **Declaration:** the operator grammar has no `typeParameters?` slot, and
   `visitOperatorOverloadDeclaration` never captures method-type-params or `methodSource`
   (`CajetaParser.g4:188-234`; `CajetaLlvmVisitor.h:1177-1271`).
2. **Inference (the crux):** `unifyMethodTemplateFormal` (`CajetaClass.cpp:3445-3531`) binds only
   `CajetaClass` placeholder type-vars; it never extracts/conformance-checks `CajetaConstantType`
   non-type values. So `K` is never inferred and `a.cols == b.rows` is never checked.

## Required fixes from the adversarial review (verdict: sound-with-fixes) — MUST land

1. **CRITICAL — the placeholder short-circuit makes the designed inference dead code.** A formal
   `Matrix<T,R,K>` is resolved via `instantiate()` with `T/R/K` bound to t-var placeholders;
   the `isTVar` short-circuit (`TemplateInstantiator.cpp:117-137`) returns the **bare** `Matrix`
   with **empty** `typeArguments`. So the class-template-recursion arm in
   `unifyMethodTemplateFormal` (`CajetaClass.cpp:3488-3501`) — where the design wanted to add the
   non-type leaf binding — **never executes**; only the positional fallback (`3590-3616`) fires,
   binding `K` from `LHS.back()` and ignoring the RHS, with no conformance check. **Fix first:
   preserve the formal's structural type-args** (suppress/special-case the `isTVar` short-circuit
   for method-template formal resolution, *or* carry the formal's pre-instantiation type-arg
   structure separately into the unifier). Nothing else works until this lands.
2. **Strict-equality, separate path for non-type params.** Bind non-type params on a path
   **separate** from the type-param "first binding wins" tolerance (`CajetaClass.cpp:3459-3480`),
   and **disable the positional fallback** (`3590-3616`) for non-type params — otherwise the
   fallback satisfies `K` before any check runs. On a second binding, compare
   `CajetaConstantType::getValue()`; unequal ⇒ unification fails.
3. **Failure-reason plumbing for a precise error.** Thread an optional `std::string& reason` out
   of `tryInstantiateMethodTemplate` → `resolveMethod` → `BinaryOpExpression`, so a shape mismatch
   raises *"operator* — shared dimension mismatch: LHS cols K=3 ≠ RHS rows K=4"* instead of
   `nullptr` → silent fall-through to builtin (`BinaryOpExpression.cpp:672-673`) → opaque
   "no operator*".
4. **Fix the existing non-type capture gap in `visitMethodDeclaration`** (`CajetaLlvmVisitor.h:1287-1300`
   reads only the type-param arm, never `tp->primitiveType()` / `isNonType`), and apply the same in
   the new `visitOperatorOverloadDeclaration` capture. Shared code; enables non-type method
   templates generally.
5. **Add the `CONSTANT_FLAG` param-kind check to `instantiateMethodTemplate`**
   (`MethodTemplateInstantiator.cpp:82-124` checks arity+bounds only) mirroring
   `TemplateInstantiator.cpp:206-219`, so a malformed non-type instantiation fails loudly.
6. **Sequence behind prerequisites:** the `AlwaysInlinerPass` fix (value-type plan S1b / task #87)
   — else `AlwaysInline` on instantiations is a no-op on JIT/O0 and the by-value/register win is
   lost; and an actual `Matrix<T,R,C>` `@ValueType` declaration (value-type plan / task #86), which
   doesn't exist yet (only `CooperativeMatrix` does).

## Stages (each builds `./build.sh` + tests; TDD)
- **S0** — Failing-test scaffold: a templated `operator*<uint32 K>` parses; `a*b` over compatible
  shapes monomorphizes; mismatched shapes raise the precise error. (`test/parser/`, `test/codegen/`.)
- **S1** — Grammar + visitor: `typeParameters?` on operator alternatives (`CajetaParser.g4:188-234`),
  regenerate parser; `visitOperatorOverloadDeclaration` captures type-params + `methodSource` +
  `setMethodTypeParameters` (mirror `visitMethodDeclaration:1285-1339`); update grammar comment
  (`:163-165`). Type-params only.
- **S2** — Non-type capture (FIX #4) in both operator and method visitors (read `primitiveType
  identifier`, set `isNonType`/`nonTypePrimitive`).
- **S3** — **Inference core (FIX #1+#2+#3):** preserve formal structural type-args through the
  placeholder short-circuit; add the non-type leaf binding + strict-equality conformance on a
  separate path; disable the positional fallback for non-type; thread the failure-reason. Plus the
  `CONSTANT_FLAG` kind check (FIX #5). This is the bulk of the work.
- **S4** — Value-type integration: confirm instantiations inherit `AlwaysInline` (after S1b) and
  lower to flat IR; `K`-constant loops unroll. Host only.
- **S5** — Docs + code-size note: update `MethodLevelTemplate.md` (non-type params + inference +
  matmul) and `OperatorOverloading.md` §8 (lift the ban for *static* operators; keep it for
  instance); note the per-template instantiation-count open risk.
- **Device** — deferred to the value-type plan's S8 (`@Device` pure operator, aggregate-param
  `lowerDeviceFn`, shared inference cache-key so host/device don't drift).

## Open decisions (defaults baked; override on review)
- Grammar scope: `typeParameters?` on **all** operator alternatives (uniform; downstream gate
  rejects templated instance operators) — *recommended* — vs. static-eligible alternatives only.
- Explicit non-type call syntax (`a.operator*<3>(b)`): **inference-only for v1**, explicit deferred.
- Fix `visitMethodDeclaration`'s non-type gap in this change (enables non-type method templates
  generally): **yes, shared code**.
- Per-template instantiation budget/diagnostic: rely on the value-type size guard for now; a count
  cap is an open risk, not a blocker.
- Conformance: unify-time constant equality only (`a.cols==b.rows`); richer `where`-clause
  constraints are future work.

## Verification
- `./build.sh`; `CAJETA_SOURCE_ROOT="$PWD" ./build/test/cajeta_test --gtest_filter=...` per stage.
- Regression: existing `MethodTemplate*`/`OperatorOverload*`/template suites unchanged.
- Driving acceptance: `Matrix<f32,2,3> * Matrix<f32,3,4>` → `Matrix<f32,2,4>` bit-exact (host),
  and `Matrix<f32,2,3> * Matrix<f32,5,4>` → precise shared-dimension-mismatch compile error.
- **Constraints:** commit when verified (standing permission), brief messages, no attribution
  trailer, stage files explicitly; push ask-first.
