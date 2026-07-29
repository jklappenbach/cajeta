# DType

`cajeta.math.DType` — a numeric dtype descriptor: the runtime value that
identifies an element type's kind, bit width, and signedness. A `Tensor<T>`
carries its dtype in the reified type parameter `T`; a `DType` value is what
the NEP-50 type-promotion table (`promote`) and the runtime-dtype paths
reason over. Identity is `(kind, bits, variant)` — `variant` distinguishes
same-width floats (`float16` vs `bfloat16`, the four `float8` encodings).
The v1 dtype set covers `bool`, `int8..128`, `uint8..128`, `float16..128`,
`bfloat16`, and the `float8`/`float6`/`float4` variants; `complex64/128` are
reserved (the `Complex` bound exists, but no concrete complex primitive yet).

```cajeta
package snip.dtype;

import cajeta.math.DType;

public final class Demo {
    public static void run() {
        DType f = DType.of<float32>();     // type → DType bridge
        DType i = DType.i32();
        DType r = DType.promote(f, i);     // NEP-50: float32
        boolean fl = r.isFloating();       // true
        int32 width = r.bits();            // 32
        return;
    }
}
```

## Methods

| Signature | |
|---|---|
| **Descriptor** | |
| `DType(int32 kindCode, int32 bitWidth, int32 variantCode)` | Build a descriptor from kind, bit width, and variant codes |
| `int32 kind()` | Kind code (`KIND_BOOL` / `KIND_INT` / `KIND_UINT` / `KIND_FLOAT` / `KIND_COMPLEX`) |
| `int32 bits()` | Bit width |
| `int32 variant()` | Variant code (distinguishes same-width float encodings) |
| `int32 bytes()` | Bytes per element |
| `boolean equals(DType other)` | Same `(kind, bits, variant)` identity |
| **Category predicates** | |
| `boolean isBool()` | Kind is bool |
| `boolean isSignedInt()` | Kind is signed int |
| `boolean isUnsignedInt()` | Kind is unsigned int |
| `boolean isIntegral()` | Signed or unsigned int |
| `boolean isFloating()` | Kind is floating |
| `boolean isComplex()` | Kind is complex (reserved) |
| `boolean isNumeric()` | Any numeric kind (not bool) |
| `boolean isSigned()` | Signed in the sign-bit sense: signed ints, floats, complex (not bool, not uint) |
| **Named factories** | |
| `static #DType boolType()` | `bool` |
| `static #DType i8()` | `int8` |
| `static #DType i16()` | `int16` |
| `static #DType i32()` | `int32` |
| `static #DType i64()` | `int64` |
| `static #DType i128()` | `int128` |
| `static #DType u8()` | `uint8` |
| `static #DType u16()` | `uint16` |
| `static #DType u32()` | `uint32` |
| `static #DType u64()` | `uint64` |
| `static #DType u128()` | `uint128` |
| `static #DType f16()` | `float16` |
| `static #DType f32()` | `float32` |
| `static #DType f64()` | `float64` |
| `static #DType f128()` | `float128` |
| `static #DType bf16()` | `bfloat16` |
| `static #DType f8e4m3()` | `float8` e4m3 |
| `static #DType f8e5m2()` | `float8` e5m2 |
| `static #DType f8e4m3fnuz()` | `float8` e4m3fnuz |
| `static #DType f8e5m2fnuz()` | `float8` e5m2fnuz |
| `static #DType f6e2m3()` | `float6` e2m3 |
| `static #DType f6e3m2()` | `float6` e3m2 |
| `static #DType f4e2m1()` | `float4` e2m1 |
| **Type bridge and promotion** | |
| `static int32 codeOf<T>()` | Compiler intrinsic: the packed dtype code of `T`, `(kind << 16) \| (bits << 4) \| variant` |
| `static #DType of<T>()` ⚑ | The `DType` descriptor of the static type parameter `T` — the bridge `Tensor<T>.dtype()` is built on |
| `static #DType promote(DType a, DType b)` ⚑ | NEP-50 type-based promotion: the result dtype of a binary op on operands of dtypes `a` and `b`, from the types (never the values) |

⚑ = `@EntryPoint`

## See also

- Source: [`runtime/src/cajeta/math/DType.cajeta`](../../../runtime/src/cajeta/math/DType.cajeta)
- [Tensor](Tensor.md) — carries its dtype in the reified `T`
