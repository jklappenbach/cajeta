# 09 — Type kinds

Five type kinds ship today: `class`, `interface`, `enum`, `view`, and
`annotation`; a method becomes a GPU kernel with `@Kernel`. `record` and
`structure` are reserved words with no syntax yet.

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

## Reserved: record and structure

Neither has declaration syntax — writing one is a parse error today. Their
value-type role is covered by `class` plus the construction-site storage class.

Next: [Allocation](10-allocation.md).
