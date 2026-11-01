# OperatorOverloading

This document specifies operator overloading in Cajeta. Two design
rules carry everything else:

1. **Binary operators and non-mutating unaries are `public static`.**
   Both operands are explicit parameters, and there is no implicit
   `this`. The operator cannot mutate either operand. It returns a
   fresh value. This is the shape C# uses.
2. **Mutating unaries (`++`, `--`) and indexed access (`[]`, `[]=`)
   are instance methods.** They mutate the receiver, and the borrow
   checker enforces that the call site holds a mutable borrow.

The split exists because the host's role is different in the two
categories. `a + b` is symmetric arithmetic. Neither operand is
privileged, and neither should mutate. `x++` is in-place mutation,
and the receiver is the target. One shape for both would force a
wrong default on one of them. A shape for each keeps both readings
clean.

> **Implementation status (verified against `test/parser/`).** Shipped:
> static binary arithmetic/bitwise/comparison operators (`+ - * / %
> & | ^ << >>  == != < > <= >=`), `==`/`!=` and the `< > <= >=`
> derivations, and instance `[]` / `[]=`
> (`OperatorOverloadTests.cpp`, `ValueTypeOperatorHostTests.cpp`,
> `ValueTypeIndexOperatorTests.cpp`, `ObjectOperatorEqTests.cpp`).
> Still deferred: unary `+` / `-`, mutating unary `++` / `--`, and
> compound assignment `+= -= …` have grammar alternatives but are not
> yet lowered. Logical and bitwise not, `!` / `~`, have no grammar
> alternative at all, since `OPERATOR BANG` / `OPERATOR TILDE` are
> absent from `operatorOverloadDeclaration`, so `operator!` /
> `operator~` do not parse yet. The sections that describe these forms
> are the specification. They are not current behavior.

## Table of contents

1. [Operator categories](#1-operator-categories)
2. [Static binary operators](#2-static-binary-operators)
3. [Static unary operators](#3-static-unary-operators)
4. [Instance mutating unaries (`++`, `--`)](#4-instance-mutating-unaries---)
5. [Instance indexed access (`[]`, `[]=`)](#5-instance-indexed-access---)
6. [Compound assignment (`+=`, `-=`, ...)](#6-compound-assignment---)
7. [Comparison operators and `==`](#7-comparison-operators-and-)
8. [Call-site dispatch](#8-call-site-dispatch)
9. [Borrow-checker interaction](#9-borrow-checker-interaction)
10. [Grammar changes](#10-grammar-changes)
11. [Visitor / codegen changes](#11-visitor--codegen-changes)
12. [Migration from the instance-method-binary form](#12-migration-from-the-instance-method-binary-form)
13. [Worked example — `Vec2`](#13-worked-example--vec2)
14. [Open questions](#14-open-questions)

---

## 1. Operator categories

| Category | Operators | Form | Arity | Returns | Mutates |
|---|---|---|---|---|---|
| Binary arithmetic   | `+ - * / %`              | `public static` | 2 (LHS, RHS)         | new value | no |
| Binary bitwise      | `& \| ^ << >>`           | `public static` | 2                    | new value | no |
| Comparison          | `== != < > <= >=`        | `public static` | 2                    | `boolean` | no |
| Boolean             | `&& \|\|`                | not overloadable | —                    | —         | — |
| Non-mutating unary  | `- + ! ~`                | `public static` | 1 (single operand)   | new value | no |
| Mutating unary      | `++ --`                  | instance        | 0                    | `void`    | yes (`this`) |
| Indexed read        | `[]`                     | instance        | 1 (the index)        | element   | no |
| Indexed write       | `[]=`                    | instance        | 2 (index, value)     | `void`    | yes (`this`) |
| Compound assignment | `+= -= *= /= %= &= \|= ^= <<= >>=` | derived (default) or instance (explicit) | 1 (RHS) | `void` (instance form) | yes (instance form) |

Boolean `&&` and `||` are intentionally not overloadable.
Short-circuit evaluation cannot be expressed faithfully through a
single function call, because both operands would have to be
evaluated to be passed in, and short-circuit semantics are too
important to drop silently.

---

## 2. Static binary operators

```cajeta
public final class Vec2 {
    public float32 x;
    public float32 y;

    public Vec2(float32 x, float32 y) {
        this.x = x;
        this.y = y;
    }

    public static Vec2 operator+ (Vec2 a, Vec2 b) {
        return stack Vec2(a.x + b.x, a.y + b.y);
    }

    public static Vec2 operator- (Vec2 a, Vec2 b) {
        return stack Vec2(a.x - b.x, a.y - b.y);
    }

    public static Vec2 operator* (Vec2 v, float32 k) {
        return stack Vec2(v.x * k, v.y * k);
    }
}
```

`a + b` lowers to `Vec2.operator+(a, b)`. Both operands cross the
ABI as borrows; the return is an owned value. Chained expressions
(`a + b * 2.0`) build a fresh value at each step and the borrow
checker is satisfied throughout because no operand is ever mutated.

The return value can be `stack`-allocated, which returns it by value
and copies it into the caller's slot, or `heap`-allocated and
transferred via the `#T` return-type form:

```cajeta
public static #Vec2 operator+ (Vec2 a, Vec2 b) {
    return heap Vec2(a.x + b.x, a.y + b.y);
}
```

The stack form is preferred for value-type math on small POD-shaped
classes such as Vec2, Quaternion and Mat4. The `#T` form is for cases
where the result needs heap residency for downstream lifetime
reasons. That is rare for math types and common for
collection-returning ops.

### Asymmetric LHS / RHS types

The two operands don't have to be the same type. `Vec2 * f32` is a
common asymmetry:

```cajeta
public static Vec2 operator* (Vec2 v, float32 k) { ... }
```

`v * 2.0f` resolves. `2.0f * v` does NOT, because cajeta only
dispatches on the LHS type. To support the reversed form, declare a
second overload:

```cajeta
public static Vec2 operator* (float32 k, Vec2 v) {
    return Vec2.operator*(v, k);     // delegate
}
```

The two-overload pattern keeps the dispatch rule simple. Languages
that try to make `k * v` find the operator on `Vec2`, such as Scala
with implicit conversions and Python with `__rmul__`, all end up
trading dispatch cost or surprise for the convenience.

---

## 3. Static unary operators

A static unary operator takes a single explicit operand and has no
`this`:

```cajeta
public final class Vec2 {
    public static Vec2 operator- (Vec2 v) {            // unary negation
        return stack Vec2(-v.x, -v.y);
    }
}

public final class Mask {
    public uint32 bits;

    public static Mask operator~ (Mask m) {            // bitwise not
        return stack Mask(~m.bits);
    }

    public static boolean operator! (Mask m) {         // logical not
        return m.bits == 0;
    }
}
```

`-v` lowers to `Vec2.operator-(v)`, the single-operand overload.
The grammar uses the same `OPERATOR SUB` token for both unary and
binary `-`, and the arity at the declaration site disambiguates them.

---

## 4. Instance mutating unaries (`++`, `--`)

A mutating unary takes zero parameters, mutates `this`, and returns
`void`:

```cajeta
public final class Counter {
    public int32 value;

    public Counter() { this.value = 0; }

    public void operator++ () {
        this.value = this.value + 1;
    }

    public void operator-- () {
        this.value = this.value - 1;
    }
}
```

`x++` and `++x` both lower to `x.operator++()`. Cajeta does NOT
distinguish pre-increment from post-increment at the declaration
level. By default the operator runs after the surrounding expression
is evaluated, which is the post-increment shape. Pre-increment is the
same call, and the surrounding expression sees `x`'s new value.

The borrow checker fires on the call site:

```cajeta
Counter c = stack Counter();
c++;                                  // OK — mutable borrow of c
let const_ref = &c;
const_ref++;                          // ERROR: ++ requires mut borrow
```

### Why not C#-style static `operator++`?

C# requires `public static T operator++(T x)` returning a new T, and
rewrites `x++` to `x = T.op_Increment(x)`. That works well for
immutable value types, where allocation is free, but it precludes a
true in-place increment on a large object. For `Counter`, the
difference is negligible. For `Tensor`, copying every backing buffer
on `++` would be catastrophic. The instance form gives you the
choice, and the C#-style static form is still reachable as a
`static` method named `incremented`.

---

## 5. Instance indexed access (`[]`, `[]=`)

```cajeta
public final class Grid<T> {
    T[] cells;
    int32 width;

    public T operator[] (int32 i) {
        return this.cells[i];
    }

    public void operator[]= (int32 i, T value) {
        this.cells[i] #= value;
    }
}
```

`grid[5]` lowers to `grid.operator[](5)`. `grid[5] = v` lowers to
`grid.operator[]=(5, v)`. The grammar already collapses the two
into a single `OPERATOR LBRACK RBRACK ASSIGN?` alternative, and that
shape stays.

Multi-index (`grid[i, j]`) is a future extension once the lexer
distinguishes a 2-D index from a comma-expression in a 1-D index.

---

## 6. Compound assignment (`+=`, `-=`, ...)

By default, compound assignment is derived from the binary form.
`a += b` rewrites to `a = a + b` at the AST-lowering level, with no
separate user code required and no separate symbol emitted. The
static `operator+` is the only thing that exists, and `+=` is sugar.

For cases where in-place beats allocate-and-reassign, such as large
objects where copying the result back over the LHS dominates the
actual mutation, you can explicitly declare an instance
`operator+=`:

```cajeta
public final class Tensor {
    float32[] data;
    int32 size;

    // Binary + still exists; returns a fresh Tensor.
    public static Tensor operator+ (Tensor a, Tensor b) { ... }

    // Explicit instance += mutates in place. When present, the
    // compiler routes `a += b` through this instead of the
    // derive-from-+ default.
    public void operator+= (Tensor other) {
        for (int32 i = 0; i < this.size; i = i + 1) {
            this.data[i] = this.data[i] + other.data[i];
        }
    }
}
```

The compiler picks the route. If an instance `operator+=` exists on
the LHS type, it routes through it, and otherwise it derives from
binary `+`. A class can ship one without the other, for example a
class that does not support `+` at all but has a mutate-in-place
`+=` for performance reasons.

Same pattern for `-=`, `*=`, `/=`, `%=`, `&=`, `|=`, `^=`, `<<=`,
`>>=`.

---

## 7. Comparison operators and `==`

`==` on class instances defaults to identity equality, which is
pointer comparison. Java's `Object.equals` does NOT default to the
same, which Java got wrong. To define structural equality, declare:

```cajeta
public final class Point {
    public int32 x;
    public int32 y;

    public static boolean operator== (Point a, Point b) {
        return a.x == b.x && a.y == b.y;
    }
}
```

When `operator==` is defined, `!=` is derived automatically and
returns the negation. Defining only `==` is the idiomatic shape.
Defining only `!=` is rejected with a diagnostic suggesting the
caller add `==` too.

The pairing rule is that `<` and `>` are independent. `<=` and `>=`
are derived from `<` or `>` when not defined explicitly.
Implementing all three of `<`, `<=`, `==` is fine but unnecessary,
because the compiler picks up the gaps.

When defined, the four comparison operators are required to be
consistent. The type system cannot fully enforce it, but a
`@TotalOrder` annotation lets you opt in to runtime checks under
`--debug`. That annotation is out of v1 scope.

### Value-type (Vector / Matrix) comparisons yield masks, not booleans

The rules above govern user-declared `operator==` / `operator<` etc.,
which return a scalar `boolean`. A user `@ValueType` `Vec2` can
declare `public static boolean operator== (Vec2 a, Vec2 b)`, as
`test/parser/ValueTypeOperatorHostTests.cpp` shows. The built-in
`@ValueType` SIMD types `Vector<T,N>` and `Matrix<…>` are different.
`== != < <= > >=` on them produce a per-lane mask (`<N x i1>`). They
do not produce a reduced boolean. Reduce a mask with `.all()` /
`.any()`, and blend with `.select(a, b)`. Do not use a raw
Vector/Matrix comparison directly as an `if` condition. Reduce it
first.

### Hash consistency

A class that defines `operator==` for structural equality MUST also
override `hash()` for structural hashing. Otherwise
`HashMap<MyClass, V>` silently mis-keys, because two equal-by-`==`
instances with different identity hashes land in different buckets.
The compiler emits `CAJETA_WARN_HASH_EQUALS_MISMATCH` when one is
present without the other, and the `@AutoHash` annotation
synthesizes both from a field list.

---

## 8. Call-site dispatch

| Expression | Lowers to |
|---|---|
| `a + b`     | `T.operator+(a, b)` where T is the static type of `a` |
| `a < b`     | `T.operator<(a, b)` |
| `-a`        | `T.operator-(a)` (unary form — chosen by arity at the declaration) |
| `!a`        | `T.operator!(a)` |
| `~a`        | `T.operator~(a)` |
| `a++`       | `a.operator++()` (instance call, mutable borrow of `a`) |
| `++a`       | Same call; surrounding expression sees `a`'s post-state |
| `a[i]`      | `a.operator[](i)` |
| `a[i] = v`  | `a.operator[]=(i, v)` |
| `a += b`    | `a.operator+=(b)` if declared, else `a = T.operator+(a, b)` |

Static-operator dispatch is not virtual. The static type of the LHS
determines which class's operator runs. Subclasses can shadow a
parent's static operator but cannot override it, and the runtime
type is ignored at the operator-dispatch site. This matches how C#
and Scala both handle static operators and avoids the "surprising
override" trap.

Method-template operators are not supported. The same restriction
applies to method-templated virtuals, as described in
`MethodLevelTemplate.md`.

---

## 9. Borrow-checker interaction

The borrow lifetimes that an overload introduces:

- **Static binary / unary**: each operand crosses the call as a plain
  borrow. The return is a fresh, owned value. There are no new
  lifetime questions, the same as any other static method.
- **Instance `++` / `--`**: requires a mutable borrow of the
  receiver. Calling on an immutable borrow is a compile error.
- **Instance `[]`**: returns a borrow of an element. The element
  borrow lives as long as the receiver borrow.
- **Instance `[]=`**: requires a mutable borrow of the receiver and
  ownership or a borrow of the value, per the value's type's
  ownership rules.
- **Derived `+=`**: the rewritten `a = a + b` form requires the LHS
  to be assignable; the borrow checker treats this as any other
  reassignment.
- **Explicit instance `+=`**: requires a mutable borrow of the
  receiver.

The borrow checker does not need any operator-specific rules. The
existing borrow and ownership inference already handles every case
above once the dispatch lowers correctly.

---

## 10. Grammar changes

Today the rule in `antlr4/CajetaParser.g4` § `operatorOverloadDeclaration`,
after the in-progress `REFERENCE?` work, accepts the broad shape
`typeType OPERATOR <sym> formalParameters methodBody` with no
constraints on staticness or arity. The grammar already accepts
`static` as a `modifier` via `classOrInterfaceModifier`, so
`public static Vec2 operator+ (Vec2 a, Vec2 b) { ... }` parses
today. The issue is that the grammar does not require it.

Two options:

### Option A: Enforce in the grammar (strict)

Split the rule into two productions. One requires `STATIC` and a
binary-shaped parameter list, and the other is instance-shaped and
limited to mutating-unary and indexed forms.

Sketch:

```antlr
operatorOverloadDeclaration
    : staticBinaryOperatorDeclaration
    | staticUnaryOperatorDeclaration
    | instanceMutatingUnaryDeclaration
    | instanceCompoundAssignDeclaration
    | indexedOperatorDeclaration
    ;

staticBinaryOperatorDeclaration
    : STATIC REFERENCE? typeType OPERATOR
        ( ADD | SUB | MUL | DIV | MOD
        | BITAND | BITOR | CARET | LSHIFT | RSHIFT
        | EQUAL | NOTEQUAL | LT | GT | LE | GE
        | ASSIGN  // operator= for assignment overload (if cajeta wants it)
        )
      formalParameters methodBody
    ;

staticUnaryOperatorDeclaration
    : STATIC REFERENCE? typeType OPERATOR
        ( SUB | ADD     // unary +/- (1-param formalParameters)
        | BANG | TILDE  // logical / bitwise not
        )
      formalParameters methodBody
    ;

instanceMutatingUnaryDeclaration
    : VOID OPERATOR ( INC | DEC ) formalParameters methodBody
    ;

instanceCompoundAssignDeclaration
    : VOID OPERATOR
        ( ADD_ASSIGN | SUB_ASSIGN | MUL_ASSIGN | DIV_ASSIGN | MOD_ASSIGN
        | AND_ASSIGN | OR_ASSIGN | XOR_ASSIGN
        | LSHIFT_ASSIGN | RSHIFT_ASSIGN | URSHIFT_ASSIGN
        )
      formalParameters methodBody
    ;

indexedOperatorDeclaration
    : typeTypeOrVoid OPERATOR LBRACK RBRACK ASSIGN? formalParameters methodBody
    ;
```

Pros:
- The grammar mechanically prevents the wrong shape from compiling.
- Diagnostics are at parse time and pinpoint-clear.

Cons:
- The unary vs binary distinction for `+`, `-`, `*` (e.g. `operator-`
  one-param vs two-param) cannot be expressed at the grammar level
  without splitting on parameter count, which ANTLR does not
  naturally do. The parser would have to disambiguate after the
  parse.
- The grammar becomes ambiguous. `STATIC` is already a `modifier`,
  which `modifier*` consumes before `memberDeclaration`. If
  `staticBinaryOperatorDeclaration` also names `STATIC` explicitly,
  the grammar has two paths and picks one non-deterministically.

### Option B: Keep one production, enforce in the visitor (recommended)

Leave `operatorOverloadDeclaration` as it is, with one alternative
per operator token. The visitor inspects the AST and rejects shapes
that do not match the operator-category table in §1.

```antlr
// Unchanged from today — except clarifying comment.

// Operator-overload signatures. The grammar accepts the broad
// shape (any operator token, any parameter count); the per-category
// staticness + arity rules from docs/OperatorOverloading.md §1
// are enforced semantically in visitOperatorOverloadDeclaration.
operatorOverloadDeclaration
    : REFERENCE? typeType OPERATOR ASSIGN     formalParameters methodBody
    | REFERENCE? typeType OPERATOR ADD        formalParameters methodBody
    | REFERENCE? typeType OPERATOR SUB        formalParameters methodBody
    | ... (every operator token, as today)
    | typeTypeOrVoid OPERATOR LBRACK RBRACK ASSIGN? formalParameters methodBody
    ;
```

The `static` modifier is consumed by `modifier*` ahead of the
declaration, the same path any static method takes. The visitor
checks `ctx->modifier()` against the operator-category requirement.

Pros:
- No grammar churn.
- Per-operator error messages can be richer, for example "operator+
  must be declared `public static` with two parameters; got instance
  method with one parameter".
- The arity-driven unary/binary `-` distinction falls out naturally.

Cons:
- The wrong shape parses, and is rejected later. That is acceptable,
  because errors have always landed late for semantic mistakes.

The recommendation is Option B. No grammar diff is needed beyond the
comment update, and all enforcement moves to the visitor.

---

## 11. Visitor / codegen changes

`CajetaLlvmVisitor::visitOperatorOverloadDeclaration` already builds
a method object from the parsed shape. The new checks:

1. **Operator category check** (per §1 table):
   - `+ - * / % & | ^ << >> == != < > <= >=`: require `static`,
     require 1 or 2 formal parameters. Single-param `+`/`-` is unary
     negation/plus; two-param is binary. Other operators in this
     list must have exactly 2 params.
   - `! ~`: require `static`, require 1 formal parameter.
   - `++ --`: forbid `static`, require 0 formal parameters, require
     `void` return type.
   - `+= -= *= /= %= &= |= ^= <<= >>=`: forbid `static`, require 1
     formal parameter, require `void` return type.
   - `[]`: forbid `static`, require 1 formal parameter.
   - `[]=`: forbid `static`, require 2 formal parameters, require
     `void` return type.

2. **Per-class invariant checks**:
   - If `operator==` is defined but `hash()` isn't overridden,
     emit `CAJETA_WARN_HASH_EQUALS_MISMATCH`.
   - If both `==` and `!=` are explicit, the `!=` must logically
     negate `==`. The compiler cannot verify that statically, but
     the warning steers users toward defining only `==`.

3. **Method-name mangling** stays the same. Operators encode as
   `operator+`, `operator++`, `operator[]`, `operator[]=`, etc. on
   the method-name side. Static binary ops sit on the class as
   static methods, and the existing static-dispatch path picks them
   up without a separate lookup mechanism.

On the `BinaryOpExpression` side, the recently patched code in
`src/cajeta/asn/expression/BinaryOpExpression.cpp`, the lookup used
to find an instance method named `operator+` on the LHS that takes
(RHS-type). Now it finds a static method named `operator+` on the
LHS's type that takes (LHS-type, RHS-type). A static call emits as
`callT::operator+(lhs, rhs)`. Both operands pass through the ABI as
plain values, or as pointers per cajeta's class-pass-by-pointer
rule. The fix you landed for the chained-`+` vtable load is moot
under static dispatch, because there is no instance to load through.

The `UnaryExpression` side is new, and is currently deferred per the
demo's comment. For `++` / `--`, it emits the `lhs.operator++()`
instance call. For `-` / `!` / `~`, it emits the `T.operator-(lhs)`
static call.

The `AssignmentExpression` side handles `+=` and friends in lookup
order. An explicit instance `operator+=` on the LHS type emits
`lhs.operator+=(rhs)`. Otherwise, if a `static operator+` exists,
the expression rewrites to `lhs = T.operator+(lhs, rhs)`. Otherwise
it falls through to the primitive-types `+=` lowering.

---

## 12. Migration from the instance-method-binary form

The current in-progress demo, `samples/Tour/src/tour/OperatorOverloadDemo.cajeta`,
declares `operator+` as an instance method that mutates `this` and
returns `this`. Under the new rule, that shape becomes:

```cajeta
// Before (current Vec2 demo):
public Vec2 operator+ (Vec2 other) {
    this.x = this.x + other.x;
    this.y = this.y + other.y;
    return this;
}

// After:
public static Vec2 operator+ (Vec2 a, Vec2 b) {
    return stack Vec2(a.x + b.x, a.y + b.y);
}
```

Semantic differences:

- `Vec2 r = a + b - c` no longer mutates `a`. `a`, `b`, `c` all
  survive unchanged, and `r` is a fresh Vec2.
- Each binary op allocates a temporary. For `stack` that is cheap,
  since it is the caller's slot and there is no heap traffic.
- The chain `a + b - c` walks through two static calls, each
  returning a new value.

The BinaryOpExpression fix you landed for the "load-through-vtable
on the chained-op return" goes away as a problem class. The static
call's return is the value directly, and no instance-pointer load is
needed.

The tests under `test/parser/OperatorOverloadTests.cpp` and the
value-type variants already exercise the static form. This migration
has landed.

---

## 13. Worked example — `Vec2`

The shape the demo should land on after the migration:

```cajeta
package tour;

// Mathy 2-D vector with the canonical operator surface. Compare to
// any C# / Rust / Scala Vec2 — same shape, different syntax skin.
public final class Vec2 {
    public float32 x;
    public float32 y;

    public Vec2(float32 x, float32 y) {
        this.x = x;
        this.y = y;
    }

    // ----- arithmetic -----
    public static Vec2 operator+ (Vec2 a, Vec2 b) {
        return stack Vec2(a.x + b.x, a.y + b.y);
    }
    public static Vec2 operator- (Vec2 a, Vec2 b) {
        return stack Vec2(a.x - b.x, a.y - b.y);
    }
    public static Vec2 operator- (Vec2 v) {                // unary negation
        return stack Vec2(-v.x, -v.y);
    }
    public static Vec2 operator* (Vec2 v, float32 k) {     // scalar mul
        return stack Vec2(v.x * k, v.y * k);
    }
    public static Vec2 operator* (float32 k, Vec2 v) {     // reversed-form overload
        return Vec2.operator*(v, k);
    }

    // ----- equality -----
    public static boolean operator== (Vec2 a, Vec2 b) {
        return a.x == b.x && a.y == b.y;
    }
    // != derived automatically; hash() must be overridden for
    // structural-equality semantics under HashMap. Overriding is by
    // signature match — there is no `override` keyword (postfix
    // `override` is a parse error); `@Override` is an accepted,
    // optional no-op annotation that documents intent.
    @Override public int64 hash() {
        return Hash.combine(Hash.f32(this.x), Hash.f32(this.y));
    }

    // ----- methods -----
    public float32 lengthSq() {
        return this.x * this.x + this.y * this.y;
    }
}

// Usage:
//   Vec2 a = stack Vec2(1.0f, 2.0f);
//   Vec2 b = stack Vec2(3.0f, 4.0f);
//   Vec2 c = stack Vec2(0.5f, 0.5f);
//   Vec2 r = a + b - c;             // a, b, c unchanged; r = (3.5, 5.5)
//   boolean eq = a == b;             // false
//   Vec2 neg = -a;                   // (-1, -2)
//   Vec2 scaled = a * 2.0f;          // (2, 4)
//   Vec2 also = 2.0f * a;            // (2, 4), reversed-form overload
```

Counter-example for instance `++`:

```cajeta
public final class Tick {
    public int64 count;
    public Tick() { this.count = 0L; }

    public void operator++ () {
        this.count = this.count + 1L;
    }
}

// Usage:
//   Tick t = stack Tick();
//   t++; t++; t++;          // t.count == 3
```

---

## 14. Open questions

- **Reversed binary overloads** (e.g. `float32 * Vec2`). Current rule
  is "user declares two overloads, one for each LHS type." Pythonic
  `__rmul__` and Scala's implicit-conversion approach both have
  worse failure modes, namely silent surprise dispatch. Stay with
  the explicit-two-overloads rule.
- **`operator=` (plain assignment).** C++ allows overloading `=`,
  and C# and Java do not. Cajeta leans toward "no", because the
  borrow checker and drop-chain semantics rely on assignment being a
  known shape, and letting users replace it opens the door to subtle
  aliasing bugs. It is listed in §10's grammar sketch for
  completeness but proposed as not overloadable in v1.
- **`operator()` (call).** Functors and function objects are useful
  for building DSLs and for lambda-like classes. This would
  plausibly be an instance method with any signature. It is out of
  v1 unless a concrete use case surfaces.
- **Conversion operators** (C++'s `operator T()`). These are implicit
  casts to another type. They are powerful but hazardous, since a
  single accidental conversion overload can wreck overload
  resolution everywhere. They are out of v1. The feature is reserved
  for an explicit `@Conversion` annotation on a named method later.
  It will not be a syntax-level operator.
- **Hash-equality lint as warning or error.** §7 currently says
  warning. Under `--strict` it should be an error. Decide whether
  `--strict` matters here.
- **Method-templated operators.** These are currently forbidden, in
  parallel with method-templated virtuals. A `Tensor<T>.operator+`
  that is monomorphized per `T` would be useful. It is plausibly
  worth revisiting once the underlying method-template-on-static-
  methods path is well exercised.
