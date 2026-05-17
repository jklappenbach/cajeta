# Primitives — boxed wrappers and the unboxed types they wrap

Cajeta has a fixed set of primitive types with explicit width:

| Family | Types |
|--------|-------|
| Signed integer | `int8`, `int16`, `int32`, `int64` |
| Unsigned integer | `uint8`, `uint16`, `uint32`, `uint64` |
| Floating-point | `float32`, `float64` |
| Boolean | `boolean` |
| Pointer | `pointer` |

There is no implicit numeric widening — every cross-width conversion
goes through an explicit cast. Bytes are `int8` (or `uint8` depending
on signedness intent); there's no separate `byte` type.

This doc covers the boxed wrapper types (`Integer`, `Long`, `Double`,
`Boolean`) and the static parse/format intrinsics that operate on
primitives.

## Status

| Feature | Status |
|---------|--------|
| Primitive arithmetic, bitwise ops, comparisons | shipped |
| `(int32) v` cast syntax | shipped |
| `intLiteral.hash()`, `floatLiteral.hash()`, etc. | shipped (HashTests) |
| `Integer.parseInt(String)` etc. intrinsic | shipped (intrinsic dispatcher) |
| `String.valueOf(int32 v)` etc. intrinsic | shipped |
| Boxed `Integer` / `Long` / `Double` / `Boolean` classes | designed, not implemented |
| `Math` namespace (`abs`, `min`, `max`, `sqrt`, …) | partial (MathIntrinsicTests) |

## Boxed wrappers — designed

Thin wrappers that exist so primitives can be stored in `Collection<T>`
and related templated types when the element-type slot is class-typed.
Boxing happens at the boundary; the unboxed types stay separate.

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

The static parse/format methods are already intrinsic in
`MethodCallExpression`'s namespace dispatcher; the boxed wrapper
classes themselves haven't been declared in cajeta source yet.

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

## `Math` namespace — partial

Intrinsics in the runtime, dispatched by name. Currently shipped:

| Method | Notes |
|--------|-------|
| `Math.abs(int32)` / `Math.abs(int64)` / `Math.abs(float32)` / `Math.abs(float64)` | unsigned wrapping behavior for INT_MIN |
| `Math.min(a, b)` / `Math.max(a, b)` | all numeric widths |
| `Math.sqrt(float64)` | |
| `Math.pow(float64, float64)` | |
| `Math.floor`, `Math.ceil`, `Math.round` | float64 only |

Pinned by `test/expression/MathIntrinsicTests.cpp`.

Full `Math` design — trig (sin/cos/tan/atan2), logs (log, log2,
log10, ln, exp), constants (PI, E), random — tracked in Features.md.

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

- Boxed `Integer` / `Long` / `Double` / `Boolean` cajeta-source classes
- Full `Math` namespace (trig, logs, random)
- `String.valueOf` / `toString` for class types (delegate to
  `obj.toString()`)
- `Encoding` enum as a real cajeta type (Lang.md)
