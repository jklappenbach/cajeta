# 15 — Lambdas & captures

Function types are primitive type-formers: `(int32, int32) -> int32` is a
type, and a value of that type is callable. There is no `Runnable` or
`@FunctionalInterface` indirection, and primitives flow through without
boxing. Tour demo:
[LambdasDemo](../../samples/tour/src/main/cajeta/tour/lang/LambdasDemo.cajeta);
spec: [Lambdas.md](../specification/lang/Lambdas.md).

## Syntax and parameter inference

A lambda is `(params) -> body`, where the body is an expression or a block.
Parameter types may be written explicitly, or omitted when the surrounding
context — the variable's declared type, or the parameter slot the lambda is
passed to — pins the function type:

```cajeta
(int32, int32) -> int32 add = (a, b) -> a + b;
int32 five = add(2, 3);
(int32) -> int32 dbl = (int32 x) -> { return x * 2; };
```

A lambda whose parameter types are neither written nor inferable from context
is a compile error. Method references are sugar for lambdas: `MathUtil::abs`
for a static method, `obj::method` for a bound instance method (which
captures `obj`).

## Primitives capture by value

Primitive locals are copied into the closure at capture time. Later writes to
the original do not affect what the closure sees:

```cajeta
int32 k = 10;
(int32) -> int32 mul = (x) -> x * k;
k = 20;
int32 fifty = mul(5);
```

Writing *to* a value-captured primitive inside the lambda is a compile error —
the write would only touch the closure's private copy, so the compiler rejects
it rather than let it silently do nothing:

<!-- snippet: skip -->
```cajeta
int32 counter = 0;
() -> int32 inc = () -> { counter = counter + 1; return counter; };
// error: write to value-captured primitive
```

For shared mutable state, wrap the value in a class and mutate through the
capture.

## Heap values capture by borrow

Class instances, arrays, and strings capture by borrow. The closure sees live
state through the borrow; the original local still owns the value:

```cajeta
public class Tally {
    public int32 v;
    public Tally() { this.v = 0; }
    public void bumpBy(int32 n) { this.v = this.v + n; }
    public int32 value() { return this.v; }
}
```

```cajeta
int32[] xs = {1, 2, 3, 4};
Tally t = stack Tally();
xs.stream().forEach((x) -> t.bumpBy(x));
int32 sum = t.value();
```

This is the shape for inline consumption — the closure is used and dropped
while the borrowed value is still in scope.

## `#` transfers ownership into the closure

A closure that must outlive its captures' scope takes ownership instead, with
the same `#` marker used everywhere else in the language. Here the returned
closure owns the `Cell`; it survives the factory's return and keeps state
across calls:

```cajeta
public class Cell {
    public int32 v;
    public Cell() { this.v = 0; }
    public int32 next() { this.v = this.v + 1; return this.v; }
}
public class Factory {
    public static () -> int32 makeCounter() {
        Cell c = heap Cell();
        return () -> #c.next();
    }
}
```

```cajeta
() -> int32 fn = Factory.makeCounter();
int32 one = fn();
int32 two = fn();
```

After the transfer, `c` is moved — the factory body can't touch it again.
When the closure drops, the captured `Cell` drops with it, through the normal
drop chain.

Next: [16 — Operator overloading](16-operators.md).
