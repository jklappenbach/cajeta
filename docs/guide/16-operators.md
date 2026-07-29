# 16 — Operator overloading

Two rules carry the whole design. Binary operators and non-mutating unaries
are `public static`: both operands are explicit parameters, nothing mutates,
and the result is a fresh value. Mutating unaries (`++`, `--`) and indexed
access (`[]`, `[]=`) are instance methods: the receiver *is* the target, and
the call site needs a mutable borrow. Tour demo:
[OperatorOverloadDemo](../../samples/tour/src/main/cajeta/tour/lang/OperatorOverloadDemo.cajeta);
spec: [OperatorOverloading.md](../specification/lang/OperatorOverloading.md).

Overloadable today: arithmetic `+ - * / %`, bitwise `& | ^ << >>`,
comparisons `== != < > <= >=`, unary `-` and `+`, instance `++ --`, and
`[]` / `[]=`. Boolean `&&` / `||` are deliberately not overloadable — a
function call can't preserve short-circuit evaluation. Unary `!` and `~`
are specified but do not compile yet (roadmap).

## Static operators

```cajeta
@AutoHash
public class Vec2 {
    public int32 x;
    public int32 y;
    public Vec2(int32 x, int32 y) { this.x = x; this.y = y; }

    public static #Vec2 operator+ (Vec2 a, Vec2 b) {
        return heap Vec2(a.x + b.x, a.y + b.y);
    }
    public static #Vec2 operator- (Vec2 a, Vec2 b) {
        return heap Vec2(a.x - b.x, a.y - b.y);
    }
    public static #Vec2 operator- (Vec2 v) {
        return heap Vec2(0 - v.x, 0 - v.y);
    }
    public static boolean operator== (Vec2 a, Vec2 b) {
        return a.x == b.x && a.y == b.y;
    }
}
```

```cajeta
Vec2 a = stack Vec2(1, 2);
Vec2 b = stack Vec2(3, 4);
Vec2 sum = a + b;
Vec2 neg = -a;
boolean same = a == b;
```

`a + b` lowers to `Vec2.operator+(a, b)`; no operand is mutated. Unary and
binary `-` are distinguished by arity. Dispatch is on the static type of the
LHS only, and it is not virtual — so `v * 2` needs an overload on `Vec2`, and
the reversed `2 * v` would need one on `int32`, which primitives can't
declare. One direction is the practical scope.

## Derived operators

Declare `==` and the compiler derives `!=` as its negation. Declare `<` and
`<=`, `>`, `>=` come free. Compound assignment derives from the binary form:
`a += b`
rewrites to `a = Vec2.operator+(a, b)` with no `operator+=` declared. When an
explicit instance `operator+=` *is* declared, it wins — useful when in-place
mutation beats allocate-and-reassign:

```cajeta
public class Acc {
    public int32 total;
    public Acc() { this.total = 0; }
    public void operator+= (int32 n) { this.total = this.total + n; }
}
```

## The `==` / `hash()` pairing rule

A class that defines structural `operator==` must also make `hash()`
structural, or `HashMap<K, V>` mis-keys: two `==`-equal instances with
identity hashes land in different buckets. The compiler enforces the pairing
with `CAJETA_WARN_HASH_EQUALS_MISMATCH` when `operator==` appears without a
`hash()` override. `@AutoHash` (on `Vec2` above) synthesizes a structural
`hash()` from the fields, keeping it consistent with `==` automatically.

## Instance operators

`++` and `--` mutate the receiver: no parameters, `void` return. Both `t++`
and `++t` lower to `t.operator++()`. Subscripts read and write through a pair
of instance methods — `g[i]` lowers to `g.operator[](i)`, and `g[i] = v` to
`g.operator[]=(i, v)`. Subscript expressions hand the index over as `int64`.

```cajeta
public class Ticker {
    public int32 count;
    public Ticker() { this.count = 0; }
    public void operator++ () { this.count = this.count + 1; }
    public void operator-- () { this.count = this.count - 1; }
}

public class Grid {
    public int32[] cells;
    public Grid() { this.cells = heap int32[4]; }
    public int32 operator[] (int64 i) { return this.cells[(int32) i]; }
    public void operator[]= (int64 i, int32 v) { this.cells[(int32) i] = v; }
}
```

```cajeta
Ticker t = stack Ticker();
t++;
t++;
t--;

Grid g = stack Grid();
int64 i = 1;
g[i] = 20;
int32 read = g[i];
```

The stdlib's `HashMap` exposes the same subscript pair — `m[k]` is this
mechanism, not special-cased syntax.

Next: [17 — Inheritance](17-inheritance.md).
