# 08 — Native types

Every numeric type has an explicit width — there is no `int`, `long`,
`float`, or `double`. There is no implicit widening either: every cross-width
conversion is an explicit cast. And there is no `byte` type; a byte buffer is
`int8[]` or `uint8[]`.

| Family | Types |
|---|---|
| Truth | `boolean` |
| Codepoint | `char` — a 32-bit Unicode codepoint, not a byte |
| Signed integers | `int8` `int16` `int32` `int64` `int128` |
| Unsigned integers | `uint8` `uint16` `uint32` `uint64` `uint128` |
| IEEE-754 floats | `float16` `float32` `float64` `float128` |
| Brain float | `bfloat16` — float32's exponent range in 16 bits (ML dtype) |
| Microscaling floats | `float4e2m1` `float6e2m3` `float6e3m2` `float8e4m3` `float8e5m2` `float8e4m3fnuz` `float8e5m2fnuz` |
| Raw pointer | `pointer` — opaque address, low-level interop |

## Integers and their literals

Decimal, hex (`0x`), octal (leading `0`), and binary (`0b`) literals, with
`_` separators anywhere inside and an `L` suffix for 64-bit values.

```cajeta
int8   tiny  = 7;
int32  count = 1_000_000;
int64  big   = 9_000_000_000L;
uint8  octet = 255;
int32  hex   = 0xCAFE_BABE;
int32  oct   = 0755;            // 493
int32  bin   = 0b1010_0101;     // 165
```

The 128-bit widths are full native types with arithmetic and casts:

```cajeta
int64   seed  = 42;
int128  wide  = (int128) seed;
uint128 uwide = 7;
int128  sum   = wide + wide;
```

## Floats

`f` (or `F`) suffixes a single-precision literal; unsuffixed decimal literals
are `float64`.

```cajeta
float32  f = 3.14f;
float64  d = 2.71828;
float16  h = 1.5f;
float128 q = 2.75;
bfloat16 b = 1.25f;
```

The seven microscaling formats (OCP MX: FP8, FP6, FP4) are storage-only
today — declare them, put them in buffers, move them around. Conversion and
arithmetic helpers are future work; see
[Primitives.md](../specification/lang/Primitives.md).

```cajeta
float8e4m3 scale;
float8e5m2 gradient;
float4e2m1 weight;
```

## boolean and char

`char` is a 32-bit Unicode codepoint — Go's `rune`, not C's byte. Character
literals take any single codepoint or an escape.

```cajeta
boolean ready  = true;
char    z      = 'Z';
char    accent = 'é';            // 233
char    emoji  = '😀';           // 0x1F600
char    tab    = '\t';
int32   cp     = (int32) emoji;
```

## Casts

Conversions never happen silently. Float-to-int truncates.

```cajeta
int32   n = 1000;
int64   w = (int64) n;           // widen
float64 f = (float64) n;         // int → float
int32   t = (int32) 3.99;        // 3 — truncation
int8    s = (int8) n;            // narrow, top bits dropped
```

Boxed wrappers (`Int32`, `Float64`, `UInt8`, …) live in `cajeta.lang` for
storing primitives in templated collections; see
[Primitives.md](../specification/lang/Primitives.md).

Runnable tour:
[`PrimitivesDemo.cajeta`](../../samples/tour/src/main/cajeta/tour/lang/PrimitivesDemo.cajeta).

Next: [Type kinds](09-type-kinds.md).
