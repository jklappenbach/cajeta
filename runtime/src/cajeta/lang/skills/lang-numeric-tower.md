---
id: lang-numeric-tower
applies-to: [cajeta/lang/Number, cajeta/lang/Numeric, cajeta/lang/Integral, cajeta/lang/Floating, cajeta/lang/Complex]
title: Numeric tower — boxed Number vs the Numeric/Integral/Floating/Complex bound markers
description: Two parallel numeric hierarchies — the boxed Number wrapper tower and the Numeric marker bounds — and how primitives, boxes, and <T extends Numeric> cooperate.
---

# Numeric tower

There are **two distinct hierarchies here, and they do not intersect** — pick by what you are doing:

- Need to **constrain a template/wildcard to numeric types** (`<T extends Numeric>`, `Tensor<? extends Floating>`) → use the **marker interfaces** `Numeric`/`Integral`/`Floating`/`Complex`. These range over **primitives** (`int32`, `float32`, `bfloat16`, …), zero-cost, no boxing.
- Need to **carry a number through an `Object`-typed slot** (a generic container, reflective `invokeBoxed`) → use the **boxed wrapper tower** rooted at `Number` (`Int32`, `Float64`, `UInt64`, …). These are heap objects.

The trap: **a primitive satisfies the markers; its box does not, and vice-versa.** `int32 ⊨ Integral` but `Int32` (the box) does **not** `implements Integral`. `Number` is **not** a `Numeric`. Crossing the two towers is what trips agents up.

## Members and roles

Marker tower (interfaces, `cajeta.lang`, empty — pure markers):
- `Numeric` — root; "any number." Sub-markers below.
- `Integral extends Numeric` — `int8/16/32/64/128`, `uint8/16/32/64/128`.
- `Floating extends Numeric` — `float16/32/64/128`, `bfloat16`, the `float8`/`float6`/`float4` formats.
- `Complex extends Numeric` — **RESERVED**: no concrete complex primitive exists yet, so today *nothing* satisfies it intrinsically. The bound exists only so future `complex64`/`complex128` (or a class `implements Complex`) compose into `Tensor<? extends Numeric>` code.

Box tower (concrete classes):
- `Number extends Object` — root of every boxed primitive. Field-less by design (so a subclass's `value` is always reflect-index 0: `Class.of(box).getInt32(box, 0)`). Carries `asInt64()`/`asFloat64()` (trivial defaults every wrapper overrides) and the `formatSigned`/`formatUnsigned`/`formatFloat` text helpers.
- `Int8…Int128`, `UInt8…UInt128`, `Float16…Float128`, `BFloat16`, etc. — each `extends Number`, holds one `value` field, immutable.
- **NOT in either tower:** `Boolean` and `Char` are deliberately not `Number`s, and `bool` satisfies **no** numeric marker.

## Conformance — dual relation (the core mechanic)

A type `X` satisfies a marker `M` iff **either**:
- **intrinsic** — `X` is a primitive and the compiler's type-flag lattice (`FLOAT_FLAG`/`INT_FLAG`/`SIGNED_FLAG`) places it under `M`. No `implements`, no boxing. This table is **closed** (compiler-owned); you cannot re-tag primitives.
- **nominal** — `X` is a class/interface declaring `implements M` (or transitively). This is how a user `BigDecimal`, or a future complex class, joins the hierarchy.

Bound admission is **entirely compile-time → zero runtime cost**. `Tensor<bool>` and `f<int32>()` where `Floating` is required are clean compile errors, not crashes. The markers are the compile-time face of the same lattice the runtime `cajeta.math.DType` predicates (`isFloating`/`isIntegral`) reflect.

**v1 markers gate admission + covariance only — they expose no operations.** You cannot `+`/`*` directly on a `<T extends Numeric> x`. To compute on a bounded wildcard you must recover the concrete type (reified capture) or dispatch on `DType`. Operator-traits on bounds are a deliberate follow-on, not here.

## How the two towers cooperate

The bridge is the **primitive value**, not a type relationship:
- box a primitive → `Int32.of(v)` (static `of`, Java-style `valueOf`); unbox → `box.value()` or `box.asInt64()`/`asFloat64()`.
- A `<T extends Numeric> T` is a primitive `T`; to store it in an `Object[]` you box it (`Int32.of(...)`), and the box is an `Object` (via `Number`) — but it is no longer admissible to a `Numeric` bound. Round-trip through `value()` to get back a bound-satisfying primitive.

## Ownership / lifecycle

- Markers are types only — nothing to own.
- Boxes are heap objects. `of(...)`, `toString()`, and the `formatSigned/Unsigned/Float` helpers all return **owned** values (`#Int32`, `#String`) — caller owns and drops them. The wrappers are **immutable**.
- Equality is via the `hash()` override (`Object.operator==` is `a.hash() == b.hash()`): integer boxes hash by value, `Float64` hashes **bitwise** (`-0.0` canonicalizes to `+0.0`; distinct NaN bit patterns hash distinctly), `UInt64.asInt64()` reinterprets bits (values ≥ 2⁶³ come back negative) while `asFloat64()` is the true magnitude.

## Worked example

```cajeta
import cajeta.lang.Number;
import cajeta.lang.Int32;
import cajeta.lang.String;
import cajeta.math.Tensor;

// 1. Marker bound: ranges over the PRIMITIVE int32 (no box involved).
public class Histogram<T extends Integral> { /* monomorphized per int type */ }
Tensor<float32> a = Tensor.zeros<float32>(shape);   // float32 ⊨ Floating ⊨ Numeric
// Tensor<bool> b;                                   // compile error: bool ⊭ Numeric

// 2. Box tower: carry an int32 through an Object slot, then recover it.
#Int32 boxed = Int32.of(42);          // owned heap box; boxed is Number is Object
int32  prim  = boxed.value();          // unbox back to a Numeric-satisfying primitive
#String text = boxed.toString();       // owned "42" (via Number.formatSigned)
```

## What this does NOT do

- No primitive arithmetic on a bound (`<T extends Numeric>` exposes no `+`/`*` in v1).
- `Number` does **not** implement `Numeric`; a box is not admissible to a numeric bound — unbox first.
- No `Complex` primitive exists; `<T extends Complex>` admits nothing today (nominal classes aside).
- You do not add primitives to the intrinsic table — extend the hierarchy only via nominal `implements`.
- `Boolean`/`Char`/`bool` are not numbers anywhere in this tower.

See `docs/specification/cajeta-templates/numeric-bounds-spec.md`, `cajeta.math.DType` (runtime face), and `plans/lang/wrapper-types-plan.md` (the box tower).
