# Primitives — boxed wrappers and the unboxed types they wrap

Cajeta has a fixed set of **explicit-width** primitive types (there is no
`int`/`long`/`float`/`double`). The authoritative list is the native-type
bootstrap in `src/cajeta/type/CajetaType.cpp`:

| Family | Types | Backing |
|--------|-------|---------|
| Boolean | `boolean` | i1 |
| Character | `char` | a **32-bit Unicode codepoint** (i32) |
| Signed integer | `int8` `int16` `int32` `int64` `int128` | iN |
| Unsigned integer | `uint8` `uint16` `uint32` `uint64` `uint128` | iN |
| Floating-point (IEEE) | `float16` `float32` `float64` `float128` | half / float / double / fp128 |
| Brain float | `bfloat16` | LLVM `bfloat` (ML training dtype) |
| Low-precision floats | `float4e2m1` `float6e2m3` `float6e3m2` `float8e4m3` `float8e5m2` `float8e4m3fnuz` `float8e5m2fnuz` | OCP Microscaling — storage-only today (opaque iN; conversion/arith helpers are future work) |
| Raw pointer | `pointer` | opaque address; low-level/interop |

There is no implicit numeric widening — every cross-width conversion goes
through an explicit cast. **There is no `byte` type**: the canonical byte buffer
is `int8[]` (or `uint8[]`). `uchar` is a deprecated alias for `uint8`. `char` is
a codepoint, not a byte.

This doc covers the boxed wrapper types and the static parse/format intrinsics
that operate on primitives. For the broader language tour, see
[`../LanguageGuide.md`](../../guide/drafts/LanguageGuide.md).

## Status

| Feature | Status |
|---------|--------|
| Primitive arithmetic, bitwise ops, comparisons | shipped |
| `(int32) v` cast syntax | shipped |
| `intLiteral.hash()`, `floatLiteral.hash()`, etc. | shipped (HashTests) |
| `Integer.parseInt(String)` etc. intrinsic | shipped (intrinsic dispatcher) |
| `String.valueOf(int32 v)` etc. intrinsic | shipped |
| Boxed wrapper classes (`Int32` / `Int64` / `Float32` / `Float64` / `Boolean` / `UInt8`…`UInt64`) | shipped (`runtime/src/cajeta/lang/`) |
| `Math` namespace — `abs`/`min`/`max`/`sqrt`/`pow` + transcendentals (`sin`/`cos`/`tan`/`exp`/`log`…) | shipped (`runtime/src/cajeta/lang/Math.cajeta`) |

## Boxed wrappers — shipped

Thin wrappers that exist so primitives can be stored in `Collection<T>`
and related templated types when the element-type slot is class-typed.
Boxing happens at the boundary; the unboxed types stay separate.

The shipped classes are named after the primitive they wrap — `Int32`,
`Int64`, `Float32`, `Float64`, `Boolean`, and `UInt8`/`UInt16`/`UInt32`/`UInt64`
(in `runtime/src/cajeta/lang/`) — **not** Java's `Integer`/`Long`/`Double`. The
sketch below shows the shape; consult the source for the exact surface.

```cajeta
public final class Integer {
    public int32 value;
    public Integer(int32 v);
    public int32 unwrap();

    // Static parse / format
    public static int32 parseInt(String s);
    public static int32 parseInt(String s, int32 radix);
    public static String toString(int32 v);
    public static String toString(int32 v, int32 radix);
    public static String toBinaryString(int32 v);
    public static String toHexString(int32 v);

    // Range
    public static int32 MIN_VALUE;
    public static int32 MAX_VALUE;
}

public final class Long  { ... }              // int64 equivalent
public final class Double { ... }              // float64 equivalent
public final class Boolean { ... }
```

The static parse/format methods are intrinsic in `MethodCallExpression`'s
namespace dispatcher; the boxed wrapper classes are declared in
`runtime/src/cajeta/lang/` (`Int32.cajeta`, `Int64.cajeta`, `Float32.cajeta`,
`Float64.cajeta`, `Boolean.cajeta`, `UInt8.cajeta` … `UInt64.cajeta`).

## `String.valueOf` and `<primitive>.toString`

```cajeta
String s1 = String.valueOf(42);                // "42"
String s2 = String.valueOf(3.14);              // "3.14"
String s3 = String.valueOf(true);              // "true"
String s4 = String.valueOf('a');               // "a"

String s5 = "count = " + 42;                   // "count = 42"
                                                // (concatenation autostringifies)
```

`+` over a `pointer` + non-pointer pair automatically routes through
the appropriate stringifier (`__cajeta_i64_to_str`,
`__cajeta_f64_to_str`, `__cajeta_bool_to_str`). Pinned by
`test/expression/StringMethodsTests.cpp`.

## `Math` namespace

`Math` (`runtime/src/cajeta/lang/Math.cajeta`) is a mix of monomorphized generic
methods and compiler intrinsics dispatched by name. Shipped:

| Group | Methods |
|-------|---------|
| Basic | `abs`, `min`, `max`, `sqrt`, `pow`, `floor`, `ceil`, `round` |
| Transcendentals | `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2(y,x)`, `exp`, `exp2`, `log`, `log2`, `log10` |

Pinned by `test/expression/MathIntrinsicTests.cpp`. Consult `Math.cajeta` for the
exact per-width signatures and any constants.

## Primitive `.hash()`

Compiler intrinsics over each primitive. See Hashing.md § "Per-
primitive `hash()`" for the table and semantics. Pinned by
`test/expression/HashTests.cpp`.

## Conversion intrinsics

```cajeta
int32 narrow = (int32) someInt64;             // explicit truncation
int64 wide = (int64) someInt32;               // explicit sign-extend
float64 fp = (float64) someInt32;             // int→float
int32 back = (int32) someFloat;               // float→int (truncation)
```

Pinned by `test/expression/ConversionIntrinsicTests.cpp`,
`test/expression/UnaryAndCastTests.cpp`.

## Open items

Tracked in Features.md:

- Arithmetic/conversion helpers for the low-precision float formats
  (`float4*`/`float6*`/`float8*` are storage-only today)
- `Math` constants (`PI`, `E`) and `random`
- `String.valueOf` / `toString` for class types (delegate to
  `obj.toString()`)
- `Encoding` enum as a real cajeta type (Lang.md)
