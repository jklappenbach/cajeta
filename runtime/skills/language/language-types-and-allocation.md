---
id: language-types-and-allocation
applies-to: [cajeta/language/types, cajeta/language/allocation]
title: Native types, literals, casts, type kinds, and heap/stack placement
description: Explicit-width primitives with no implicit widening; the six type kinds (class, interface, enum, record, view, annotation); explicit heap/stack construction and when to pick which.
---

# Types & allocation

## Primitives — explicit widths, explicit casts

`boolean`; `char` (a 32-bit Unicode codepoint — Go's rune, not C's byte);
`int8..int128`; `uint8..uint128`; `float16..float128`; `bfloat16`; the OCP
microscaling floats (`float8e4m3`, … — **storage-only** today, no arithmetic);
`pointer` (opaque interop address). There is no `int`, `long`, `float`,
`double`, or `byte`.

Literals: `_` separators anywhere; `L` suffix for 64-bit (`9_000_000_000L`);
`f`/`F` for `float32` (`3.14f`); **unsuffixed decimal literals are `float64`**;
`0x`/`0b`/leading-`0` octal. Conversions never happen silently — every
cross-width move is a cast, and float→int **truncates**: `(int32) 3.99` is `3`.
Boxed wrappers (`Int32`, `Float64`, …) exist in `cajeta.lang` for templated
collections.

## Type kinds (six, plus `@Kernel`)

- **`class`** — the one aggregate type. Reference vs value is decided at the
  construction site (`heap`/`stack`), not in the declaration. Depth:
  `cajeta/language/classes`.
- **`interface`** — method contracts only: no state, no default methods.
- **`enum`** — named constants; the ordinal is an explicit cast away:
  `(int32) Color.BLUE` is `2`.
- **`record`** — a value type: flat field bytes, no header, no vtable.
  Assignment copies; `==` is structural (field-by-field); fields are immutable
  unless declared `mut`; `a.with(price: 3.0)` returns a modified copy.
  Aggregate construction is labeled `Tick { price: 2.5, volume: 4 }` or
  positional `Tick { 2.5, 4 }` (never mixed); defaulted fields may be omitted.
  Records may `extends` a record (static, non-virtual). A record **cannot**
  hold reference fields — a class field, *including `String`*, is a compile
  error. A class may hold record fields; never the reverse.
- **`view`** — a typed zero-copy overlay onto a byte buffer, byte-exact
  declared layout, `@BigEndian`/`@LittleEndian`. No class references, no
  interfaces.
- **`annotation`** — declared with the `annotation` keyword (there is no
  `@interface`). Depth: `cajeta/language/annotations`.
- **`@Kernel`** on a static method compiles it for GPU/CPU-SIMD targets —
  see the `cajeta.xpu` skills.
- `structure` is reserved — using it is a parse error.

## Allocation — `heap` or `stack`, always spelled

There is no `new`; omitting the placement prefix is a compile error.

- **`stack`** — lives and dies inside the current method/block; drops at `}`.
- **`heap`** — must outlive the frame: returned, stored in a field or
  collection, or handed to another owner with `#`
  (`cajeta/language/ownership`). Freed automatically when its owner drops.

One type, either storage: a method taking `Point` accepts both; instances
always pass by reference (no slicing). Returning a stack local is rejected —
`CAJETA_ERROR_FRESH_RETURN_NEEDS_TRANSFER` steers you to `heap` + a `#` return.

## Worked example (verified: returns 200)

```cajeta
package dev.cajeta.skills;

public enum Color { RED, GREEN, BLUE }

public record Tick {
    mut float64 price;
    int32 volume = 1;
}

public class TypesDemo {
    public static int32 run() {
        int32   count = 1_000_000;
        int64   big   = 9_000_000_000L;
        float32 f     = 3.14f;
        float64 d     = 2.71828;          // unsuffixed decimal is float64
        int64   w     = (int64) count;    // widen explicitly
        int32   t     = (int32) 3.99;     // 3 — float→int truncates
        char    emoji = '😀';             // 32-bit codepoint
        int32   cp    = (int32) emoji;    // 128512

        int32 ord = (int32) Color.BLUE;   // 2

        Tick a = Tick { price: 2.5, volume: 4 };
        Tick b = Tick { 2.5, 4 };
        boolean same = a == b;            // true — structural equality
        Tick c = a.with(price: 3.0);      // a untouched
        a.price = 3.5;                    // mut field writes in place

        Point s = stack Point(3, 4);              // Point: see language-overview
        Point h = heap Point { x: 5, y: 12 };     // aggregate initializer

        int32 sameI = 0;
        if (same) { sameI = 1; }
        return t + ord + sameI + s.distSq() + h.distSq();  // 200
    }
}
```

## Sharp edges

- Numeric literals bind overloads by their literal type — an unsuffixed
  decimal is `float64`, an unsuffixed integer prefers `int32`; when a call is
  ambiguous or resolves oddly, cast the literal explicitly.
- The microscaling float formats declare and store but do not compute —
  convert through wider types (`docs/specification/lang/Primitives.md`).
- `char` literals accept any single codepoint (`'😀'`) — indexing text by
  bytes vs codepoints is a `String` concern (`cajeta.lang` skills).
