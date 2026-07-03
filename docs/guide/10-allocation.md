# 10 — Allocation

Every class instance is created with an explicit placement prefix: `stack` or
`heap`. There is no `new`. Omitting the prefix at a constructor call is a
compile error — placement is always visible at the allocation site.

```cajeta
public class Point {
    public int32 x;
    public int32 y;

    public Point(int32 x, int32 y) {
        this.x = x;
        this.y = y;
    }

    public int32 distSq() {
        return this.x * this.x + this.y * this.y;
    }
}
```

```cajeta
Point onStack = stack Point(3, 4);    // this frame; dropped at scope exit
Point onHeap  = heap  Point(5, 12);   // heap block; freed by the drop chain
Point named   = stack Point { x: 1, y: 2 };   // aggregate initializer
```

Run it: [AllocationDemo](../../samples/tour/src/main/cajeta/tour/lang/AllocationDemo.cajeta).

## One type, either storage

`Point` is the same type wherever it lives. The storage mode is a property of
the value, not the type — the borrow checker tracks lifetime as metadata.
A method that takes `Point` accepts both:

```cajeta
public class Meter {
    public int32 measure(Point p) {
        return p.distSq();
    }

    public void run() {
        Point s = stack Point(3, 4);
        Point h = heap Point(5, 12);
        int32 a = this.measure(s);
        int32 b = this.measure(h);
    }
}
```

There is no separate pointer or value flavor of a class, and no slicing:
class instances always pass and return by reference.

## When to pick which

- **`stack`** when the value lives and dies inside the current method or
  block. No allocation call; the instance drops at the closing `}`.
- **`heap`** when the value must outlive the frame: it is returned to the
  caller, stored in a field or collection, or handed to another owner with
  `#` (next chapter). The heap block is freed automatically when its owner
  goes out of scope — there is no `delete`.

The compiler enforces the boundary. Returning a stack-allocated local from a
method is a compile error — the frame it lives in is gone before the caller
can look at it:

<!-- snippet: skip -->
```cajeta
public Point make() {
    Point p = stack Point(1, 2);
    return p;               // ERROR — stack value cannot escape its frame
}
```

Heap values never leak either way: if nothing transfers ownership out, the
drop chain reclaims the block at the owner's scope exit. Cleanup on both the
normal and the exception path is covered in chapter 11.

A third placement, `shared`, exists inside GPU kernels for workgroup-shared
memory; it is covered with the compute material. Full semantics:
[MemoryModel](../specification/lang/MemoryModel.md) and
[UnifiedClasses](../specification/lang/UnifiedClasses.md).

Next: [Ownership & borrowing](11-ownership.md).
