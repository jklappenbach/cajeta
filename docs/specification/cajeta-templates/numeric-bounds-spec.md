# Numeric template bounds — primitives satisfy `Numeric` / `Floating` / … (spec)

> **Status: spec (requirements + the load-bearing design decisions).** Defines a built-in
> **numeric marker hierarchy** (`Numeric ⊃ Floating / Integral / Complex`; `bool` standalone)
> that **primitive types satisfy intrinsically**, usable as template-parameter and wildcard
> **bounds**. Companion: `reified-capture-spec.md` (recover concreteness to operate) and
> `docs/specification/cajeta-math/tensor-spec.md` §2.1/§3 (the dtype-generic surface that needs this).

## 1. Scope & role
A bound like `Number` / `Floating` exists to range over **numeric types** — and the numeric
types *are* the primitives (`float32`, `int32`, `bfloat16`, …). A `Floating` bound that
excludes `float32` is a bound no float satisfies; it inverts its own purpose. So this spec
makes the numeric markers **first-class bounds that primitives conform to**, enabling
dtype-generic code without runtime dispatch:

```cajeta
public static T relu<T extends Floating>(T x) { … }     // any float type, monomorphized
public class Tensor<T extends Numeric> { … }            // numeric element types only
public static float64 meanOf(Tensor<? extends Floating> t) { … }   // any float tensor
```

This is **load-bearing for the whole numerical stack**: the numpy/torch port is fundamentally
generic over numeric primitives (`<T extends Floating>`, `Tensor<? extends Numeric>`). Without
primitive-satisfiable bounds those signatures are inexpressible and the library collapses to
runtime `DType` dispatch — losing static typing *and* monomorphized performance.

## 2. The load-bearing decision — trait-style conformance, not Java boxing
Two language philosophies; cajeta must pick the trait side:

- **Java's model:** primitives sit *outside* the bound graph; to "be a `Number`" you box to
  `Integer`. For a GPU/numerics-first language, boxing a billion floats is a non-starter.
- **C++/Rust/Swift model:** `i32: Num`, `Double: FloatingPoint`, `Float: Floating` — primitives
  conform to numeric traits directly, monomorphized, zero-cost. cajeta is already monomorphized
  and already classifies primitives by these axes, so this is the natural fit.

**Why it is not new information.** `CajetaType` already computes exactly this classification:
`getTypeFlags()` carries `FLOAT_FLAG` / `INT_FLAG` / `SIGNED_FLAG` / `BIT_*_FLAG` and the
`*_ID` table (used by the `DType.codeOf<T>` intercept). So:
`float32 : Floating` ≡ `flags & FLOAT_FLAG`; `int32 : Integral` ≡ `INT_FLAG`;
`… : Numeric` ≡ `FLOAT_FLAG | INT_FLAG | (complex)`. The markers are **names for a lattice the
compiler already has** — we expose it as a bound, we don't invent a capability.

**Why "bounds only work over classes" today is an artifact.** Bound satisfaction is currently a
walk of the **nominal class graph** (`extends`/`implements`), in which primitives are not nodes.
But satisfaction is a *conformance relation*, and nominal subtyping is only one way to answer
it. The fix is a **dual conformance** check (§4), not a redefinition of bounds.

## 3. The hierarchy
Built-in marker interfaces in **`cajeta.lang`** (fundamental, reusable beyond `cajeta.math`):

```
Numeric                          (any number)
├── Floating   — float16/32/64/128, bfloat16, the fp8/fp6/fp4 variants
├── Integral   — int8/16/32/64/128 (signed) + uint8/16/32/64/128 (unsigned)
└── Complex    — complex64/128  (RESERVED — no concrete complex primitive yet)
bool                             (stands alone — NOT Numeric)
```

These mirror the runtime `cajeta.math.DType` predicates (`isFloating`/`isIntegral`/`isNumeric`)
— the **DType descriptor is the runtime reflection of the same lattice; the markers are its
compile-time, type-system face.** (Finer markers — `Signed`/`Unsigned` — are a possible later
refinement; v1 ships the four above.)

## 4. Conformance — dual relation
A type `X` **satisfies** a numeric marker `M` iff **either**:
- **(intrinsic)** `X` is a primitive and the flag lattice places it under `M`
  (`float32 ⊨ Floating`, `uint8 ⊨ Integral`, both ⊨ `Numeric`); **or**
- **(nominal)** `X` is a class/interface that declares `implements M` (or transitively does)
  — so a user `BigDecimal`, or a future `complex64` class, is `Numeric` the ordinary way.

The bound-satisfaction predicate consults **both** backings. `bool` satisfies neither numeric
marker (it is not `Numeric`). The intrinsic table is **closed** (compiler-owned) in v1 — users
extend the hierarchy via nominal `implements`, not by re-tagging primitives.

## 5. Surface & semantics
- **Bounded template parameters:** `class Tensor<T extends Numeric>`, `static T f<T extends
  Floating>(…)`. A type argument is admitted iff it satisfies the bound (§4); otherwise a
  **compile-time error** (`Tensor<bool>` rejected where `Numeric` required; `f<int32>` rejected
  where `Floating` required).
- **Bounded wildcards:** `Tensor<? extends Floating>` as a parameter/field/return type.
- **Covariant subtyping / admission:** `Tensor<float32>` ⊑ `Tensor<? extends Floating>` ⊑
  `Tensor<? extends Numeric>` ⊑ `Tensor<?>`. So `meanOf(Tensor<? extends Floating>)` **accepts**
  a `Tensor<float32>`/`Tensor<bfloat16>` and **rejects** a `Tensor<int32>` at compile time.
- **Member exposure (v1 = marker-only):** the bound gates *admission* and enables *covariance*;
  it exposes **no operations**. To actually compute on a `? extends Floating`, code **captures
  back to the concrete type** (`reified-capture-spec.md`) or dispatches on `DType`. Operator
  members on bounds (`+`/`*` directly on a `<T extends Numeric>`) are a deliberate follow-on
  (operator-traits), not v1.
- **Soundness / cost:** admission is entirely compile-time → **zero runtime cost, zero
  soundness risk** for marker bounds. When operator-traits later let a bound expose `+`,
  monomorphization lowers it to the concrete primitive's op per instantiation — still sound,
  still zero-cost (no boxing, ever).

## 6. Goals / Non-goals
**Goals:** `Numeric`/`Floating`/`Integral`/`Complex` marker interfaces in `cajeta.lang`;
**intrinsic primitive conformance** via the existing type-flag lattice + **nominal class
conformance** via `implements` (dual relation); bounded **template parameters** and **bounded
wildcards** over them; covariant subtyping + compile-time admission (floats in, `int`/`bool`
out where `Floating`); composition with reified capture so a bounded wildcard can be operated
on by recovering the concrete type; the `Tensor<? extends Numeric/Floating>` surface
(`tensor-spec.md`) expressible.

**Non-goals (v1):** operator members on bounds (operator-traits — later); user-defined
*intrinsic* conformance (adding new primitives to the closed table); `super`-bounded numeric
wildcards; F-bounded / recursive numeric bounds; `Signed`/`Unsigned`/width-parametric markers;
making `bool` numeric.

## 7. Acceptance criteria
1. `Numeric`/`Floating`/`Integral`/`Complex` declared in `cajeta.lang`; a bounded declaration
   (`<T extends Floating>`, `class C<T extends Numeric>`, `Tensor<? extends Floating>`) parses
   and resolves.
2. **Intrinsic admission:** `float32`/`float64`/`bfloat16`/fp8 satisfy `Floating` (and
   `Numeric`); `int32`/`uint8` satisfy `Integral` (and `Numeric`); `bool` satisfies neither —
   verified by accept/reject at compile time (a rejected case is a clean diagnostic, not a
   crash).
3. **Nominal admission:** a class declaring `implements Numeric` is admitted where `Numeric` is
   the bound.
4. **Covariant assignment/admission:** `Tensor<float32>` assignable to `Tensor<? extends
   Floating>`; a routine over `Tensor<? extends Floating>` accepts a float tensor and rejects
   `Tensor<int32>`.
5. **Composition:** inside a `Tensor<? extends Floating>` routine, capturing to a concrete
   `Tensor<float32>` (`reified-capture-spec.md`) succeeds for an admitted float tensor and lets
   the body operate with static types.
6. Zero runtime cost: marker-bound admission emits no runtime check (compile-time only),
   verified in the lowered IR.
