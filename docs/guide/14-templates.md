# 14 — Templates & wildcards

Cajeta templates monomorphize. There is no erasure, so each (class,
type-argument) pair compiles to a distinct runtime type with its own field
layout and vtable. `Box<int32>` stores a bare `int32` with no boxing, and
`Box<String>` is a separate compiled class. The tour demo is
[TemplatesDemo](../../samples/tour/src/main/cajeta/tour/lang/TemplatesDemo.cajeta).

## Class templates

```cajeta
public class Box<T> {
    public T value;
    public Box(T v) { this.value #= v; }
    public T get() { return this.value; }
    public void set(T v) { this.value #= v; }
}
```

A `Box` holds whatever it is handed, so it takes a plain `T` and stores it with
`#=`, which is the container shape from [chapter 11](11-ownership.md). `set(v)` lends
and `set(#v)` transfers. Storing a `T` parameter with a plain `=` is rejected
as `CAJETA_ERROR_KEEP_NEEDS_SHARP_STORE`, because a caller that passed `#v`
would have the value freed when the call returns. A parameter that is only
ever borrowed is spelled `^T` and may be stored with `=`. The mark does
nothing when `T` is instantiated with a primitive, and it never goes on the
type-parameter declaration itself (`class Box<^T>` is an error).

## Method templates

A single method can carry its own type parameters. The parameter list sits
after the method name. There is no form that puts it before the return type.
The method must be `final` (instance) or `static`, because templated methods
are excluded from the vtable, and the modifier makes that explicit. Each
call-site type tuple monomorphizes separately. See
[MethodLevelTemplate.md](../specification/lang/templates/MethodLevelTemplate.md).

```cajeta
public class Pick {
    public final U first<U>(U a, U b) { return a; }
    public static V second<V>(V a, V b) { return b; }
}
```

## Numeric pseudo-bounds

Class bounds (`<T extends Base>`) can't admit primitives, so three built-in
pseudo-bounds do. `Numeric` admits all integer and float types, `Integral`
admits integers only, and `Floating` admits `float32` and `float64`.
Arithmetic on `T` lowers to the primitive op directly, with no boxing and no
vtable hop. Instantiating outside the bound (`Bitset<float64>`) fails at
compile time with `CAJETA_ERROR_TYPE_PARAMETER_BOUND`. The tour demo is
[NumericTemplatesDemo](../../samples/tour/src/main/cajeta/tour/lang/NumericTemplatesDemo.cajeta),
and the spec is [numeric-bounds-spec.md](../specification/lang/templates/numeric-bounds-spec.md).

```cajeta
public class Duo<T extends Numeric> {
    public T x;
    public T y;
    public Duo(T x, T y) { this.x = x; this.y = y; }
    public Duo<T> add(Duo<T> o) { return stack Duo<T>(this.x + o.x, this.y + o.y); }
}
public class Bitset<T extends Integral> {
    public T value;
    public Bitset(T initial) { this.value = initial; }
    public void set(int32 bit) {
        T mask = 1L << bit;
        this.value = this.value | mask;
    }
}
```

```cajeta
Duo<int32> a = stack Duo<int32>(1, 2);
Duo<float64> b = stack Duo<float64>(0.5, 1.5);
Bitset<int64> bits = heap Bitset<int64>(0L);
bits.set(3);
```

`Duo<int32>` and `Duo<float64>` are distinct compiled classes, one with
`int32` fields and one with `float64` fields.

## Wildcards

Invariance is the default, so `Box<Dog>` is not a `Box<Animal>`. Wildcards
express variance at the use site, in the standard PECS shape (Producer
Extends, Consumer Super). The tour demo is
[WildcardsDemo](../../samples/tour/src/main/cajeta/tour/lang/WildcardsDemo.cajeta),
and the spec is [TemplateWildcard.md](../specification/lang/templates/TemplateWildcard.md).

```cajeta
public class Animal {
    public int32 tag() { return 1; }
}
public class Dog extends Animal {
    public int32 tag() { return 2; }
}
public class Kennel {
    public static int32 inspect(Box<? extends Animal> b) {
        return b.value.tag();
    }
}
```

`Box<? extends Animal>` accepts a `Box` of any `Animal` subtype. Reads
project through the upper bound, so `b.value.tag()` dispatches virtually,
and foreign writes are rejected with `CAJETA_ERROR_PECS_WRITE_VIOLATION`.
`Box<? super Dog>` is the reverse. Writes of `Dog` are accepted, and reads
don't project. `Box<?>` is unbounded, so any instantiation fits.

```cajeta
Box<Dog> bd = heap Box<Dog>(heap Dog());
int32 tag = Kennel.inspect(bd);
Box<? super Dog> sink = heap Box<Animal>(heap Animal());
sink.value = heap Dog();
```

## Capture conversion

A wildcard hides `T`, but the compiler still tracks it per receiver. A value
read from a wildcard receiver can be fed back to the same receiver, even
though `T` is unnameable in source:

```cajeta
Box<? extends Animal> b = heap Box<Dog>(heap Dog());
b.set(b.get());
```

Next: [15 — Lambdas & captures](15-lambdas.md).
