# 14 — Templates & wildcards

Cajeta templates monomorphize. There is no erasure: each (class, type-argument)
pair compiles to a distinct runtime type with its own field layout and vtable.
`Box<int32>` stores a bare `int32` — no boxing — and `Box<String>` is a separate
compiled class. Tour demo:
[TemplatesDemo](../../samples/tour/src/main/cajeta/tour/lang/TemplatesDemo.cajeta).

## Class templates

```cajeta
public class Box<T> {
    public T value;
    public Box(T v) { this.value = v; }
    public T get() { return this.value; }
    public void set(T v) { this.value = v; }
}
```

## Method templates

A single method can carry its own type parameters. The parameter list sits
after the method *name*, not before the return type, and the method must be
`final` (instance) or `static` — templated methods are excluded from the
vtable, and the modifier makes that explicit. Each call-site type tuple
monomorphizes separately. See
[MethodLevelTemplate.md](../specification/lang/templates/MethodLevelTemplate.md).

```cajeta
public class Pick {
    public final U first<U>(U a, U b) { return a; }
    public static V second<V>(V a, V b) { return b; }
}
```

## Numeric pseudo-bounds

Class bounds (`<T extends Base>`) can't admit primitives, so three built-in
pseudo-bounds do: `Numeric` (all integer and float types), `Integral`
(integers only), `Floating` (`float32`/`float64`). Arithmetic on `T` lowers to
the primitive op directly — no boxing, no vtable hop. Instantiating outside
the bound (`Bitset<float64>`) fails at compile time with
`CAJETA_ERROR_TYPE_PARAMETER_BOUND`. Tour demo:
[NumericTemplatesDemo](../../samples/tour/src/main/cajeta/tour/lang/NumericTemplatesDemo.cajeta);
spec: [numeric-bounds-spec.md](../specification/lang/templates/numeric-bounds-spec.md).

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

`Box<Dog>` is not a `Box<Animal>` — invariance is the default. Wildcards
express variance at the use site, the standard PECS shape (Producer Extends,
Consumer Super). Tour demo:
[WildcardsDemo](../../samples/tour/src/main/cajeta/tour/lang/WildcardsDemo.cajeta);
spec: [TemplateWildcard.md](../specification/lang/templates/TemplateWildcard.md).

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
project through the upper bound (`b.value.tag()` dispatches virtually);
foreign writes are rejected with `CAJETA_ERROR_PECS_WRITE_VIOLATION`.
`Box<? super Dog>` is the reverse: writes of `Dog` are accepted, reads don't
project. `Box<?>` is unbounded — any instantiation fits.

```cajeta
Box<Dog> bd = heap Box<Dog>(heap Dog());
int32 tag = Kennel.inspect(bd);
Box<? super Dog> sink = heap Box<Animal>(heap Animal());
sink.value = heap Dog();
```

## Capture conversion

A wildcard hides `T`, but the compiler still tracks it per receiver: a value
read from a wildcard receiver can be fed back to the same receiver, even
though `T` is unnameable in source:

```cajeta
Box<? extends Animal> b = heap Box<Dog>(heap Dog());
b.set(b.get());
```

Next: [15 — Lambdas & captures](15-lambdas.md).
