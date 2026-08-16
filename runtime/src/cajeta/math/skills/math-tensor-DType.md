---
id: math-tensor-DType
applies-to: [cajeta/math/DType]
title: DType — runtime dtype descriptor & NEP-50 promotion
description: The (kind,bits,variant) dtype value driving NEP-50 promotion and runtime-dtype paths; the Numeric/Floating/Integral/Complex predicate hierarchy, the v1 native dtype set, and the reserved complex types.
---

# DType

A **runtime dtype descriptor** in the `cajeta.math` package — the value that
identifies an element type's `(kind, bits, variant)`. A `Tensor<T>` carries its dtype
in the reified type parameter `T`; a `DType` value is what the **NEP-50 promotion**
table (`promote`) and the dtype-generic / runtime-dtype code paths reason over when
the static type isn't available. Bridge from a static type to a descriptor with
`DType.of<T>()`.

**Access-point flag:** value/descriptor type. You obtain one from a **static factory**
(`DType.f32()`, `DType.i32()`, …) or the type bridge `DType.of<T>()` / `DType.promote(...)`
— you rarely call the raw `(kind, bits, variant)` constructor directly.

## Predicate hierarchy (the bounds `Tensor<? extends …>` binds on)

`Numeric ⊃ Floating / Integral / Complex`; `bool` stands alone (NOT numeric).
Mirror of the hierarchy, all `boolean`-returning, no args:

- `isNumeric()` = `isIntegral() || isFloating() || isComplex()` — **excludes bool**.
- `isFloating()` — `KIND_FLOAT` (covers f16/32/64/128, bf16, and every fp8/fp6/fp4 variant).
- `isIntegral()` = `isSignedInt() || isUnsignedInt()`.
- `isSignedInt()` / `isUnsignedInt()` — signed vs unsigned integer specifically.
- `isComplex()` — reserved kind; see below.
- `isBool()` — the standalone boolean dtype.
- `isSigned()` — sign-bit sense: signed int, float, **or** complex (NOT bool, NOT uint).
  Distinct from `isSignedInt()` (`f32.isSigned()` is true; `f32.isSignedInt()` is false).

Width/identity accessors: `kind()`, `bits()`, `variant()` (all `int32`), and
`bytes()` = `(bits + 7) / 8` (so `f4`/`f6`/`f8` all report `1` byte). Identity is the
full triple `(kind, bits, variant)`; use `equals(DType other)`, not reference identity
— `f16()` and `bf16()` share width 16 but differ by `variant` and are not equal.

## Native dtype set (v1) — the factories

Factory names are **short forms** because the full primitive names (`int32`,
`float32`, `float8e5m2`, …) are reserved keywords and cannot name a method:

- `boolType()` (the standalone bool; stored as 8 bits).
- `i8 i16 i32 i64 i128` (signed) · `u8 u16 u32 u64 u128` (unsigned).
- `f16 f32 f64 f128` (standard IEEE, `VAR_STD`) · `bf16` (16-bit, `VAR_BF16`).
- `f8e4m3 f8e5m2 f8e4m3fnuz f8e5m2fnuz` (8-bit) · `f6e2m3 f6e3m2` (6-bit) · `f4e2m1` (4-bit).

**`complex64/128` are reserved and unsupported.** The `KIND_COMPLEX` kind, the
`isComplex()` predicate, and the complex branch of `promote` exist, but there is **no
concrete complex primitive and no complex factory** — do not expect `DType.complex64()`.
The complex promotion path is unreachable until complex types land.

## Construction & ownership

Every factory, `of<T>()`, and `promote(...)` return **`#DType` — an owned heap value**
(`heap DType(...)` internally; the `#` marks transfer to the caller). The caller owns
it and is responsible for its lifetime; bind it to a `#DType` local. There is no
`close()`/dispose step — it is a plain heap value dropped on scope exit per cajeta's
ownership model. `equals` takes a borrowed `DType` (no `#`); it does not consume either
operand.

## The type → DType bridge

`DType.of<T>()` recovers the descriptor of a **static** type parameter — `of<float32>()`
== `f32()`, `of<float8e4m3>()` carries `variant() == 2`, `of<boolean>()` is the bool
dtype. It is reified: it folds `codeOf<T>()` (a compiler intrinsic packing
`(kind << 16) | (bits << 4) | variant`) to a constant, then decodes it. Use `of<T>()`;
`codeOf<T>()` is the low-level intercept, not for app code (its body is a never-executed
placeholder).

## NEP-50 promotion

`DType.promote(DType a, DType b)` returns the result `#DType` of a binary op, decided
from the **types, never the values**. For the numpy-standard dtypes it matches
`numpy.result_type` under NEP-50; cajeta-extension dtypes (int128/uint128, bf16, the
fp8/fp6/fp4 variants) follow documented extension rules numpy has no opinion on. Key
rules that trip people up:

- **bool is absorbed:** `bool` with X → X.
- **int + float keeps the float's width** — `promote(f16, i64)` → `float16`, NOT
  `float64`. This is the NEP-50 surprise.
- two floats → wider; same width + different variant → next standard float up.
- two ints same signedness → wider.
- **signed + unsigned** → a signed int wide enough for both; if `2 × unsignedWidth`
  would exceed 64 it falls back to **`float64`** (numpy's `int64 + uint64 → float64`).
- promotion is commutative.

## Idiomatic use (mirrors `test/math/DTypeTests.cpp`)

```cajeta
package test;
import cajeta.math.DType;

public final class D {
    public static int32 run() {
        DType f32 #= DType.f32();
        if (!f32.isFloating()) { return -1; }
        if (f32.isIntegral())  { return -2; }
        if (!f32.isSigned())   { return -3; }   // float is signed-by-sign-bit
        if (f32.bytes() != 4)  { return -4; }

        DType b #= DType.boolType();
        if (b.isNumeric())     { return -5; }    // bool stands alone

        // NEP-50: int + float keeps the float's width (not float64).
        DType r #= DType.promote(DType.f16(), DType.i64());
        if (!(r.kind() == 3 && r.bits() == 16)) { return -6; }   // float16

        // Reified type → descriptor; round-trips against the factory.
        if (!DType.of<float32>().equals(DType.f32())) { return -7; }
        return 1;
    }
}
```

Note: the boolean type keyword in cajeta source is `boolean` (e.g. `DType.of<boolean>()`),
not `bool` — `bool` is only an informal label.

## Related

- `cajeta/math/Tensor` — carries dtype in `T`; `Tensor<T>.dtype()` is built on `of<T>()`.
- `cajeta/math/TensorProtocol` — the dtype-bound generic surface.
