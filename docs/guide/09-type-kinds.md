# 09 — Type kinds

Six type kinds ship today: `class`, `interface`, `enum`, `record`, `view`,
and `annotation`; a method becomes a GPU kernel with `@Kernel`. `structure`
is a reserved word with no syntax yet.

## class

The one aggregate type. Whether an instance is a reference or a value is
decided at the construction site (`heap` vs `stack` — [chapter
10](10-allocation.md)), not in the declaration.

```cajeta
public class Circle {
    public float64 radius;

    public Circle(float64 radius) {
        this.radius = radius;
    }

    public float64 area() { return 3.14159 * radius * radius; }
}
```

Tour: [`ClassesDemo.cajeta`](../../samples/tour/src/main/cajeta/tour/lang/ClassesDemo.cajeta).

## interface

Method contracts, no state, no default methods. Dispatch is virtual through
the implementing class.

```cajeta
public interface Shape {
    float64 area();
}

public class Disc implements Shape {
    public float64 r;
    public Disc(float64 r) { this.r = r; }
    public float64 area() { return 3.14159 * r * r; }
}
```

## enum

A fixed set of named constants. An enum value casts to `int32` — `(int32)
Color.BLUE` is `2`, its ordinal.

```cajeta
public enum Color {
    RED,
    GREEN,
    BLUE
}
```

## record

A value type: a flat bundle of fields with no per-instance header and no
vtable — the in-memory value is exactly the field bytes
([records spec](../specification/nucleo/records-spec.md) §1.4). Assignment
copies, `==` compares field-by-field (structural equality), and fields are
immutable unless a field opts in with `mut`.

```cajeta
public record Tick {
    mut float64 price;
    int32 volume = 1;
}

public class TickDemo {
    public void demo() {
        Tick a = Tick { price: 2.5, volume: 4 };  // labeled binding
        Tick b = Tick { 2.5, 4 };                 // positional, declared order
        boolean same = a == b;                    // structural equality: true
        Tick c = Tick { price: 9.0 };             // volume fills from its default
        Tick d = a.with(price: 3.0);              // copy-with; a is untouched
        a.price = 3.5;                            // mut field writes in place
    }
}
```

Construction is the aggregate initializer, labeled or positional (never
mixed in one initializer); a field with a declared default may be omitted —
in the positional form, only trailing fields. A
record can also declare an ordinary constructor and be built with `heap` /
`stack` like a class. Writing a non-`mut` field is a compile error — use
`with(...)`, which returns a copy with the named fields replaced.

Records may `extends` another record: static, non-virtual inheritance —
derived adds fields and methods but cannot override. What a record cannot
have: virtual or abstract methods, `implements`, or reference-type fields
(a class field, including `String`, is a compile error). A class may hold
record fields; never the reverse.

Records reflect like classes: `Tick.class` enumerates field names, types,
offsets, and sizes ([chapter 21](21-reflection.md)). One gap today: `T.class`
inside a template body does not yet resolve the substituted type argument —
that applies to any type argument, not just records.

The stdlib dogfoods it: [`Color`](../stdlib/math/Color.md)
(`cajeta.math.Color`) is a record over four `float32` channels.

## view

A typed, zero-copy overlay onto a byte buffer — no allocation, no object
graph. Layout is byte-exact and declared; endianness is `@BigEndian` /
`@LittleEndian` / host by default. Views cannot hold class references or
implement interfaces ([Views.md](../specification/lang/Views.md)).

```cajeta
@BigEndian
public view PacketHeader {
    int32 magic;
    int16 version;
    int16 flags;
}
```

Tour: [`ViewsDemo.cajeta`](../../samples/tour/src/main/cajeta/tour/wire/ViewsDemo.cajeta).

## annotation

Declares an annotation type; apply it with `@Name(...)`. Annotations drive
code synthesis (`@Builder`), DI (`@Inject`, `@Singleton`), and reflection.

```cajeta
public annotation Todo {
    String value;
}

@Todo(value = "cache the result")
public class Planner {
    public int32 estimate() { return 3; }
}
```

Tour: [`AnnotationsDemo.cajeta`](../../samples/tour/src/main/cajeta/tour/lang/AnnotationsDemo.cajeta).

## @Kernel functions

`@Kernel` on a static method compiles it for the GPU (or the CPU fallback) —
the same source targets NVPTX, AMDGPU, SPIR-V, or host SIMD.

```cajeta
import cajeta.xpu.KernelBuffer;
import cajeta.xpu.KernelThread;

public final class Kernels {
    // y[i] = a*x[i] + y[i]
    @Kernel
    public static void saxpy(KernelBuffer<float32> y, KernelBuffer<float32> x,
                             float32 a, uint32 n) {
        uint32 i = KernelThread.globalIdX();
        if (i < n) { y[i] = a * x[i] + y[i]; }
    }
}
```

Tour: [`XpuTour.cajeta`](../../samples/tour/xpu/src/tour/xpu/XpuTour.cajeta).

## Reserved: structure

`structure` has no declaration syntax — writing one is a parse error today.

Next: [Allocation](10-allocation.md).
