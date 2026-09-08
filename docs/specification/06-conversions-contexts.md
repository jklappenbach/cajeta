# 6 — Conversions & Contexts

This chapter defines the conversions between Cajeta types and the contexts in which each applies. Cajeta's primitives are explicit-width (Types §3.1), so numeric conversion is a visible part of the language rather than a background activity; the cast expression is its primary spelling.

## 6.1 Conversion Contexts

A conversion can occur in five contexts:

- **Assignment context** — the right side of `=` or `#=` converting to the declared type of the destination.
- **Invocation context** — an argument converting to the type of its formal.
- **Return context** — a `return` operand converting to the declared return type.
- **Cast context** — an explicit `(T) expr`.
- **String context** — an operand of `+` whose other operand is a `String` (§6.4).

## 6.2 Numeric Conversions

A widening conversion takes a numeric value to a type that can represent every value of the source type: a narrower integer to a wider integer of the same signedness, an integer to a floating-point type, or a narrower float to a wider float. Widening conversions apply implicitly in assignment, invocation, and return contexts, and preserve the numeric value (integer-to-float may round when the target's precision cannot represent the value exactly).

**Example 6.2-1.** Implicit widening in assignment and invocation contexts.

```cajeta
public final class C {
    public static int32 takes32(int32 v) { return v; }
}
int8 small = (int8) 7;
int32 wide = small;               // assignment context
float64 d = 1.5f;                 // float widening
float32 g = 3;                    // integer to float
System.stdout.println("" + C.takes32(small) + " " + wide + " " + d + " " + g);
```

> *Discussion.* **The implicit-conversion policy beyond widening is TBD.** The design intent recorded in the internal primitives specification is that every cross-width conversion is an explicit cast; as of 0.27.0 the compiler also accepts narrowing and sign-crossing conversions implicitly, truncating silently (`int8 n = wide` compiles and wraps). That permissiveness is not a stable guarantee: programs should spell every narrowing or sign-crossing conversion with a cast, and this section will bind the policy when it is settled.

## 6.3 Cast Expressions

`(T) expr` converts `expr` to type `T`.

- **Numeric casts** are always available between numeric types. A float-to-integer cast truncates toward zero. An integer narrowing cast keeps the low-order bits.
- **Reference upcasts** — to a superclass or implemented interface — need no cast; the conversion is implicit in every context.
- **Reference downcasts** — `(Derived) base` — denote the same instance viewed at the narrower type. Method dispatch remains virtual through the result.

**Example 6.3-1.** Numeric truncation and a guarded downcast.

```cajeta
public class Base { public int32 tag() { return 1; } }
public class Derived extends Base { public int32 tag() { return 2; } }
public final class C {
    public static int32 run() {
        float64 f = 3.99;
        int32 t = (int32) f;                       // 3 — toward zero
        Base b = heap Derived();
        int32 kind = b instanceof Derived ? ((Derived) b).tag() : 0;
        return t * 10 + kind;                      // 32
    }
}
System.stdout.println(C.run());
```

> *Discussion.* Reference downcasts are **unchecked** in v1: a cast to a type the instance does not have is not diagnosed at the cast, and use of the result is undefined behavior. Guard downcasts with `instanceof`. A checked cast with a defined failure is planned but not implemented.

## 6.4 String Conversion

When either operand of `+` is a `String`, the other operand converts to a `String` and the result is the concatenation. Numeric values render in their decimal form, `boolean` as `true`/`false`. A `char` renders as its numeric codepoint value.

## 6.5 Conversions and Ownership

A reference conversion — an upcast, or a downcast — denotes the same instance: it does not create a value and does not move a title; the result is a reference in the same ownership state as its source. Numeric and string conversions produce new values, owned according to the receiving context (a concatenation result is a fresh owned `String`).

> *Discussion.* Overload-resolution ranking across applicable conversions — which of several convertible-to overloads a call selects — is not yet specified; it will be bound together with the invocation rules of Expressions §14.
