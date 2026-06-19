# Numerics for Bounded Templates

Three built-in pseudo-bounds — `Numeric`, `Integral`, `Floating` — admit
primitive numeric type arguments at template instantiation. They satisfy
the niche Java's `T extends Number` was supposed to cover but couldn't,
because cajeta's primitives are real first-class types (not boxed), and
each instantiation is monomorphic: `Vec3<int32>` and `Vec3<float64>` are
distinct compiled classes with int32 / float64 field layouts, and the
arithmetic on `T` lowers to the primitive op directly (no vtable hop, no
boxing, no `Number`-style abstract method dispatch).

## The three bounds

| Bound | Admits | Excludes |
|-------|--------|----------|
| `Numeric`  | every primitive carrying `NUMBER_FLAG` except `boolean`: `int8`..`int128`, `uint8`..`uint128`, `float16`, `bfloat16`, `float32`, `float64`, `float128` | `boolean`, classes, interfaces |
| `Integral` | every primitive carrying `INT_FLAG` except `boolean`: `int8`..`int128`, `uint8`..`uint128` | `boolean`, floats, classes, interfaces |
| `Floating` | every primitive carrying `FLOAT_FLAG`: `float16`, `bfloat16`, `float32`, `float64`, `float128` | ints, classes, interfaces |

The check is purely a flag test (`TemplateInstantiator.cpp`), so it
admits the *full* primitive width — `int128`, `uint128`, `float128`, and
the half-precision `float16` / `bfloat16` all satisfy their category. The
low-precision storage floats (`float8…`, `float6…`, `float4e2m1`) also
carry `FLOAT_FLAG | NUMBER_FLAG`, so they technically pass `Floating` /
`Numeric` too, even though arithmetic on them normalizes up to a wider
float. (The compiler's *diagnostic text* on rejection still names only
`int8..int64` / `float32`/`float64`; the accepted set is whatever the
flags admit, which is wider — see Implementation below.)

`Integral` ⊂ `Numeric` and `Floating` ⊂ `Numeric`, but they're not
arranged as a class hierarchy — the parameter declares which category it
needs, and the bound check at instantiation walks the substituted type's
flag bits, not its parent chain.

`boolean` is explicitly excluded from `Numeric` / `Integral` even though
it carries the `INT_FLAG | NUMBER_FLAG` bits in `CajetaType.h` (see
`BOOLEAN_TYPE_ID`) — those flags reflect the zero/one storage
representation, not arithmetic semantics.

## Why this and not `T extends Number`?

Java's `Number` is a boxing-driven workaround: `Integer`, `Double`, etc.
each subclass `Number`, the template parameter ranges over the boxed
subclasses, and every arithmetic operation involves a method call
through the boxed type. Cajeta's primitives aren't boxed, so there's no
shared parent class to extend; the bound has to be a category check on
the primitive itself.

This way the user writes the natural template:

```cajeta
public class Vec3<T extends Numeric> {
    public T x, y, z;
    public Vec3<T> add(Vec3<T> other) {
        return stack Vec3<T>(this.x + other.x, this.y + other.y, this.z + other.z);
    }
}

Vec3<int32>   vi = heap Vec3<int32>(1, 2, 3);
Vec3<float64> vd = heap Vec3<float64>(0.5, 1.5, 2.5);
```

…and the compiler monomorphizes `Vec3<int32>::add` into a function that
adds three `i32` pairs, and `Vec3<float64>::add` into a separate function
that adds three `double` pairs. They share a source declaration but
produce independent LLVM functions, vtables, and field layouts.

## Where each bound earns its keep

```cajeta
// Numeric — any primitive numeric arithmetic
public class Adder<T extends Numeric> {
    public T sum(T a, T b) { return a + b; }
}

// Integral — bitwise ops require integer T (shift on a float is a
// type error at the primitive level; the bound catches it at the
// template instantiation site)
public class Bitset<T extends Integral> {
    public T value;
    public void set(int32 bit) { this.value = this.value | (1L << bit); }
}

// Floating — division-heavy operations that need fp semantics; e.g.
// L2-norm, normalize, sqrt. Integer T's would lose precision or
// truncate to zero.
public class Vec3F<T extends Floating> {
    public T x, y, z;
    public T magnitude() {
        T sq = this.x * this.x + this.y * this.y + this.z * this.z;
        return Math.sqrt(sq);
    }
}
```

## What instantiation rejects

A type argument that doesn't fit the category throws
`CAJETA_ERROR_TYPE_PARAMETER_BOUND` at instantiation:

```
Vec3<String>     // Numeric  rejects: String is a class
Vec3F<int32>     // Floating rejects: int32 is integral, not floating
Bitset<float32>  // Integral rejects: float32 carries FLOAT_FLAG
Holder<boolean>  // Numeric  rejects: boolean is not an arithmetic type
```

The check runs at template instantiation time in
`TemplateInstantiator.cpp`'s constraint pass — before any IR is
emitted, so a rejected combination never produces a runtime artifact.

## Implementation

`CajetaType.h` already partitions the primitives with `NUMBER_FLAG`,
`INT_FLAG`, `FLOAT_FLAG`. The bound check in
`TemplateInstantiator.cpp` intercepts the three category names by string
(`bname == "Numeric" | "Integral" | "Floating"`) before the standard
class-resolve path, then tests the substituted type's flags:

- `Numeric`  → `PRIMITIVE_FLAG && NUMBER_FLAG && !boolean`
- `Integral` → `PRIMITIVE_FLAG && INT_FLAG && !boolean`
- `Floating` → `PRIMITIVE_FLAG && FLOAT_FLAG`

(`boolean` is detected by name, not by a flag, since it carries
`INT_FLAG | NUMBER_FLAG`.)

`Numeric`, `Integral`, and `Floating` aren't first-class types — you
can't declare a field of type `Numeric` or pass one around as a value.
They exist only as the right-hand side of `T extends …` in a template
parameter declaration. Using one outside that context resolves to an
unknown type name the normal way.

## Multiple bounds

The grammar supports `T extends A & B` (intersection). Numerics
participate the same way: `T extends Numeric & Comparable<T>` requires
the substituted type to be a primitive numeric AND implement
`Comparable<T>`. (Whether a primitive can implement an interface is its
own design question — see `docs/specification/lang/UnifiedClasses.md`.) The
constraint pass checks each bound independently.

## See also

- [`Templates`](MethodLevelTemplate.md) — method-level templates and the
  monomorphization caching strategy.
- [`Primitives`](Primitives.md) — the flag system the category checks
  consult.
- [`ValueReturns`](ValueReturns.md) — `Vec3<T>::add` ships its result
  via sret + NRVO, so the per-instantiation arithmetic stays
  allocation-free.
