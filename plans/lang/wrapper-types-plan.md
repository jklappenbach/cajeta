# Wrapper-type family (`cajeta.lang` boxed primitives)

**Status:** design — not started. Unblocks reflection primitive boxing
(REFL-4.1 tail) and value-in-`Object` flow generally.

**Decided (2026-06-09, via design forks):**
- **Naming** mirrors the primitive keyword, capitalized: `Int32`, `Int64`,
  `Float32`, `Float64`, `Boolean`, … (NOT Java's `Integer`/`Long`/`Float`/
  `Double` — cajeta has no `int`/`long`/`float`/`double` keywords, so the JVM's
  32/64 framing would mismatch the language).
- **Scope** = the *full* primitive set (signed + unsigned ints, `char`, every
  float incl. the ML sub-byte/fp8 formats), shipped in phases (canonical first).
- **`Number` base** — an overridable base class the numeric wrappers extend,
  exposing `asInt64()` / `asFloat64()`. `Boolean` and `Char` extend `Object`
  directly (a codepoint and a truth value aren't `Number`s — matches Java).

## Why

1. **Reflection boxing (the trigger).** `Method.invokeObject` already returns a
   reference-typed result as a real `#Object`. The missing piece is invoking a
   method whose return is a *primitive* and handing back a boxed `#Object`
   instead of int64-widened bits — i.e. `invokeBoxed`. That needs a heap,
   introspectable object per primitive. No wrapper types exist today, so this
   is blocked. (See `plans/reflection/reflection-plan.md` REFL-4.1.)
2. **Primitives through `Object`-typed slots.** `HashMap<String, Object>`,
   `ArrayList<Object>`, `Optional<Object>`, and any generic API instantiated at
   `Object` currently can't hold a primitive. Boxing fixes that.
3. **Uniform `toString`/`hash` for primitives** in debug/printing paths.

This is a `cajeta.lang` stdlib effort that *unblocks* reflection; the reflection
wiring (Phase W5 below) is the actual REFL-4.1 deliverable and lands last.

## Language constraints this design respects

- **No `abstract`.** Cajeta has no `abstract` class/method modifier (the
  `Modifier` enum has none; `Stream.cajeta` notes "Cajeta doesn't have
  abstract-method enforcement … Future work"). The idiom is a **concrete base
  class with a default method body that subclasses override** (`Stream<T>` is a
  plain `class`; `ArrayStream<T> extends Stream<T> implements Splittable<T>`).
  So `Number` is concrete, not abstract.
- **Equality flows through `hash()`.** `Object.operator==(a, b)` is
  `a.hash() == b.hash()` (virtual). A wrapper gets value equality purely by
  overriding `hash()` — no `operator==` override needed. (`String` does exactly
  this.) Hash-collision caveat from `Object` applies (≈2⁻⁶⁴).
- **Storage model.** Unified-class model: `stack Int32(5)` for a scope-local
  box (no heap traffic) or `heap Int32(5)` when it must escape. Reflection
  boxing always uses `heap` (the result escapes as `#Object`).
- **Field index 0 = value.** The boxed value must be the first declared field so
  `Class.of(box).getInt32(box, 0)` reaches it (the `invokeObjectReturnsReference`
  test relies on field-0 access of a returned object). Keep `Number`
  **field-less** so the subclass's `value` field is always reflect-index 0.
- **`char` is i32** (a Unicode codepoint, distinct from `int8`); `Char` wraps it
  and extends `Object`, not `Number`.
- **ML sub-byte/fp8 floats are opaque iN storage** (no LLVM fp type; conversions
  are "future work"). Their wrappers store raw bits; `asFloat64()` is deferred
  until the fp-conversion runtime helpers exist (Phase W4).

## The family

```
Object
├── Number                       (concrete base; field-less; default-bodied virtuals)
│   ├── Int8   Int16  Int32  Int64  Int128
│   ├── UInt8  UInt16 UInt32 UInt64 UInt128
│   ├── Float16 BFloat16 Float32 Float64 Float128
│   └── Float4E2M1 Float6E2M3 Float6E3M2 Float8E4M3 Float8E5M2
│       Float8E4M3Fnuz Float8E5M2Fnuz          (raw-bits; conversions deferred)
├── Boolean
└── Char
```

24 wrapper classes total. The name is the capitalized primitive keyword
(`float8e4m3` → `Float8E4M3`).

### `Number` base

```cajeta
package cajeta.lang;

/**
 * Base of every boxed numeric primitive. Concrete (cajeta has no `abstract`
 * yet); the default bodies return 0 and are overridden by every subclass — a
 * Number is never meant to be instantiated directly. Field-less, so each
 * subclass's `value` field is reflect-index 0.
 */
public class Number extends Object {
    public int64   asInt64()   { return 0; }     // overridden per wrapper
    public float64 asFloat64() { return 0.0; }   // overridden per wrapper
}
```

### A representative wrapper (`Int32`)

```cajeta
package cajeta.lang;

public class Int32 extends Number {
    int32 value;                                   // field index 0

    public Int32(int32 value) { this.value = value; return; }

    /** Java-style valueOf: heap box. The boxing factory reflection calls. */
    public static #Int32 of(int32 value) { return heap Int32(value); }

    public int32 value() { return this.value; }

    public int64   asInt64()   { return (int64) this.value; }
    public float64 asFloat64() { return (float64) this.value; }

    /** Value-based hash → two Int32(5) compare equal via Object.operator==. */
    public int64 hash() { return (int64) this.value; }

    // toString(): decimal text — see "toString / parsing" below.
}
```

Every numeric wrapper follows this shape with its own primitive type and the
matching `asInt64`/`asFloat64` casts. `Boolean` and `Char`:

```cajeta
public class Boolean extends Object {
    boolean value;
    public Boolean(boolean value) { this.value = value; return; }
    public static #Boolean of(boolean value) { return heap Boolean(value); }
    public boolean value() { return this.value; }
    public int64 hash() { return this.value ? 1 : 0; }
}

public class Char extends Object {              // 32-bit Unicode codepoint
    char value;
    public Char(char value) { this.value = value; return; }
    public static #Char of(char value) { return heap Char(value); }
    public char value() { return this.value; }
    public int64 hash() { return (int64) this.value; }
}
```

## API surface (per wrapper)

- `WrapperT(prim value)` constructor.
- `static #WrapperT of(prim value)` — heap factory (the boxing entry point).
- `prim value()` — unbox.
- `int64 hash()` override — value-based equality.
- `Number.asInt64()` / `asFloat64()` overrides (numeric wrappers).
- `String toString()` override — see below.

**Immutability.** The `value` field is set once in the constructor and never
mutated; no setters. Wrappers are immutable value boxes.

**No implicit autoboxing in v1.** The compiler will *not* auto-convert
`int32 ↔ Int32` at assignment/call sites. Boxing is explicit (`Int32.of(x)`,
`b.value()`) or via reflection. Compiler-level auto(un)boxing is a separate,
larger feature (own plan when wanted) — keeping it out of v1 avoids overload-
resolution and ownership surprises.

**Small-value caching (deferred, optional).** Java caches `Boolean.TRUE/FALSE`
and small `Integer`s. `Boolean` is the obvious cheap win (two singletons).
Deferred to a later optimization pass; `of()` is the seam where a cache would
slot in without changing callers.

**`Comparable<T>` (deferred).** Numeric wrappers are natural `Comparable`s, but
templated-interface vtable dispatch "is still maturing" (per `Comparable.cajeta`).
Add `compareTo` as a concrete method per wrapper when ordered-collection demand
appears; not in the initial cut.

## toString / parsing

`Object.toString()` is currently a `null` placeholder; there's no stable
number→`String` surface yet. Options, cheapest first:

1. **Defer** — leave `toString()` inherited (null) in W1; ship it once a
   `String.fromInt64`/`fromFloat64` (or a `__cajeta_int64_to_string` native)
   exists. Lowest scope; boxing for reflection does **not** need toString.
2. **Native formatter** — add `__cajeta_int64_to_string` / `_float64_to_string`
   natives and route every numeric wrapper's `toString()` through `asInt64()` /
   `asFloat64()`. One pair of natives covers all numerics.

Recommendation: **defer to option 2 as its own small step** after the wrappers
exist; it isn't on the reflection-unblock critical path.

## Reflection boxing wiring (Phase W5 — the actual unblock)

Once the wrappers exist, REFL-4.1 boxing is:

1. **Return-type kind from RTTI.** Add a native
   `__cajeta_rtti_method_return_kind(rtti, idx) -> int32` exposing the method's
   return-type id (the `TYPE_ID` from `CajetaType.h`, or a compact enum). Today
   the method RTTI carries param types (REFL-4.3 reads them) but the *return*
   type isn't surfaced — this is the one prerequisite. (Field boxing reuses the
   REFL-2A `typeFlags` already stored per field.)
2. **`Method.invokeBoxed(Object o[, args]) -> #Object`.** Read the return kind;
   `switch` on it: call the matching existing adapter path
   (`invokeScalar`/`invokeInt32`/`invokeFloat32`/`invokeFloat64`) and wrap the
   result with the corresponding `WrapperT.of(...)`; a reference return delegates
   to `invokeObject`; `void` returns `null`.
3. **`Field.getBoxed(o) -> #Object`** and **`Class.getBoxed(o, index)`** —
   same switch over the field's `typeFlags`.
4. **`Number` round-trip** — callers that want a number generically take the
   result as `Number` (cast from `#Object`) and call `asInt64()`/`asFloat64()`.

This keeps boxing a thin layer over the typed-invoke surface already shipped in
REFL-4.1/4.4; the only new compiler/runtime surface is the return-kind native.

## Phased plan (TDD)

- [x] **W1 — core boxes + `Number`.** DONE 2026-06-09. `Number`, `Boolean`,
      `Int32`, `Int64`, `Float32`, `Float64` in `runtime/src/cajeta/lang/`.
      Float wrappers hash via the bit-reinterpreting `__cajeta_hash_float32/64`
      natives (so `Float64.of(1.5) != Float64.of(1.2)`); int/bool hash by raw
      value. Tests (`test/parser/WrapperTypesTests.cpp`, 5/5 green): `of`/
      `value()` round-trip incl. >2³² in `Int64`; value-based `==`; `Number`
      upcast + virtual `asInt64`/`asFloat64`; float bitwise equality;
      `Class.of(box).getInt32(box, 0)` == boxed value (field-index-0 layout).
      Regression after the bigger prelude: 19/19 Object-eq/inheritance/reflection.
      **Compiler bug found:** `boolean ? 1 : 0` in an `int64`-return method
      miscompiles to a malformed `ICmp` (asserts at IR build) and took down the
      whole embedded prelude — worked around with an `if` in `Boolean.hash()`
      and documented there. Worth a real compiler fix later (own ticket).
      NOTE: wrappers exist ONLY for type-erased reflection (`Object`-typed
      slots). cajeta monomorphizes generics, so primitives flow through
      `HashMap<int32,V>`/`ArrayList<int32>` natively — no boxing needed there
      (unlike Java's erased generics). See [[templates-not-generics]].
- [ ] **W2 — remaining integers + `Char`.** `Int8`/`Int16`/`Int128`,
      `UInt8`/`UInt16`/`UInt32`/`UInt64`/`UInt128`, `Char`. Tests mirror W1;
      unsigned `asInt64` zero-extends, signed sign-extends; `Char` round-trips a
      codepoint (`'😀'` → 0x1F600).
- [ ] **W3 — wide/half IEEE floats.** `Float16`, `BFloat16`, `Float128`.
      `asFloat64` via the existing fp casts where LLVM supports them.
- [ ] **W4 — ML sub-byte/fp8 floats.** `Float4E2M1` … `Float8E5M2Fnuz`. Store
      raw bits (iN); `value()` returns the opaque primitive. `asFloat64`/
      `toString` **deferred** until the fp-conversion runtime helpers land
      (documented stub that returns raw-bits-widened or throws — decide when the
      helpers exist). Log the limitation; don't silently mis-convert.
- [~] **W5 — reflection boxing (REFL-4.1).** `Method.invokeBoxed` DONE
      2026-06-09. No compiler change needed: `CajetaMethodDesc.returnType` is
      already a canonical type-name string; new native
      `__cajeta_rtti_method_return_kind` maps it to a compact kind
      (CAJETA_RK_* / mirrored in Method.cajeta), and `invokeBoxed(o[, args])`
      switches: primitive→`WrapperT.of(...)`, reference→`invokeObject`,
      `void`→`null`. A primitive with no W1 wrapper (int8/16/128, unsigned,
      char, half/quad/ML floats, `pointer`) throws the new
      `cajeta.reflect.UnsupportedReflectionException` (honest, not a widening
      lie). Tests (ReflectionTests, 3/3): invokeBoxedPrimitiveReturns
      (Int32/Float64/Boolean), invokeBoxedReferenceAndVoid (#Cell + void→null
      w/ side effect), invokeBoxedUnsupportedThrows (int8→exception).
      **Remaining W5b (TODO):** `Field.getBoxed` + `Class.getBoxed` (field type
      via the REFL-2A `typeFlags`/the field-desc `type` string — same kind
      switch); the unsupported set shrinks automatically as W2-W4 wrappers land.
- [ ] **W6 (optional) — `toString` + small-value cache.** Number→`String`
      natives wired through `asInt64`/`asFloat64`; `Boolean`/small-`Int*` cache
      behind `of()`.

## Open questions (not blocking W1)

- **`char` literal vs `Char` box** interplay once auto(un)boxing is considered.
- **Caching policy** (which ranges, thread-safety of the cache under the fiber
  model) — revisit at W6.
- **`Comparable` / ordered collections of boxes** — revisit when demand appears
  and templated-interface dispatch matures.
