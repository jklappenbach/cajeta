# 3 — Types, Values & Variables

This chapter defines Cajeta's types — the fixed set of explicit-width primitives and the reference types built from classes, interfaces, and arrays — together with the kinds of variables that hold values and the definite-assignment rule that governs reading them.

## 3.1 Primitive Types

Every numeric primitive names its width. There is no `int`, `long`, `float`, or `double`.

| Family | Types | Notes |
|---|---|---|
| Boolean | `boolean` | `true` / `false` |
| Character | `char` | a 32-bit Unicode codepoint, not a byte |
| Signed integer | `int8` `int16` `int32` `int64` `int128` | two's complement |
| Unsigned integer | `uint8` `uint16` `uint32` `uint64` `uint128` | |
| Floating-point | `float16` `float32` `float64` `float128` | IEEE 754 binary16/32/64/128 |
| Brain float | `bfloat16` | ML training dtype |
| Reduced-precision floats | `float4e2m1` `float6e2m3` `float6e3m2` `float8e4m3` `float8e5m2` `float8e4m3fnuz` `float8e5m2fnuz` | OCP Microscaling formats, storage-only — no arithmetic |
| Raw pointer | `pointer` | an opaque address, for low-level and interop code |

There is no `byte` type. The canonical byte buffer is `int8[]` (or `uint8[]`). `uchar` is a deprecated alias for `uint8`.

Most primitive type names are keywords (Grammar §2.5). `bfloat16` and `pointer` are predeclared type names rather than keywords.

**Example 3.1-1.** Explicit widths, and `char` as a codepoint.

```cajeta
int128 big = (int128) 5;
uint64 u = 18_000_000_000_000_000_000L;
char cp = 'é';
System.stdout.println("codepoint: " + (int32) cp);    // 233
```

> *Discussion.* Boxed wrapper classes (`Int32`, `Int64`, `Float32`, `Float64`, `Boolean`, `UInt8`…`UInt64`) exist so primitives can occupy class-typed template slots such as `Collection<T>` elements. Boxing happens at that boundary and nowhere else. They are library types, documented in the Stdlib Reference.

## 3.2 Reference Types

The reference types are classes (Classes §8), interfaces (Interfaces §9), and arrays (Arrays, Views & Slices §12). A `view` declaration also introduces a type (Arrays, Views & Slices §12).

A class is one type regardless of where its instances are placed: `stack MyClass()` and `heap MyClass()` produce values of the same type `MyClass`, and placement is chosen at the allocation site, not in the declaration (Allocation §4). The borrow checker tracks lifetime, and the type system does not.

Class instances always pass and return by pointer, never by value — there is no object slicing, no implicit copy construction, and no implicit boxing. A stack-allocated instance returned by value travels through a caller-allocated slot (Allocation §4).

## 3.3 Kinds of Variables and Scope

A **scope** is the region of a program a variable lives in. For a local variable it is a method invocation, or a scope block (`{ … }`) inside a method: the scope runs from the declaration to the closing brace of the declaring block, and one invocation of a method is one instance of every scope in its body. A static variable is scoped for the lifetime of the application, from program start to exit (Execution §20). A session binding is scoped to the session (Script Units §18). Reaching the end of a scope is what triggers a drop (Allocation §4). Name resolution and shadowing within scopes are Names §7.

- **Local variables** — declared in a block, and dropped at the closing brace of the declaring block when they own (Allocation §4).
- **Fields** — instance and static members of a class (Classes §8). A field's ownership status is resolved at drop time (Ownership §5.8).
- **Formals** — declared parameters. A formal's ownership mode is fixed at the call site and carried at run time (Ownership §5.5).
- **Array elements and slots** — indexed storage. A slot records its own ownership bit (Ownership §5.4, Arrays §12).
- **Session bindings** — top-level declarations of a script unit. They bind into the session scope and outlive the entry frame (Script Units §18).

## 3.4 Definite Assignment

A local variable may be declared without an initializer. Reading it before every path to the read assigns it is a compile-time error, `CAJETA_ERROR_VARIABLE_NOT_ASSIGNED`. The rule is the same for primitive-typed and reference-typed locals: an unassigned reference is not observable as a null — the read is rejected.

**Example 3.4-1.** A rejected program: a read before assignment.

<!-- snippet: skip -->
```cajeta
public final class C {
    public static int32 run() {
        int32 x;
        return x;       // CAJETA_ERROR_VARIABLE_NOT_ASSIGNED
    }
}
```

`null` is a literal assignable to any reference-typed variable. Assigning `null` to an owning binding releases its value early (Allocation §4).

## 3.5 Type Parameters and Wildcards

A class or method may declare type parameters. Parameterized classes are not genericized, and there is no run-time type erasure. As in C++, each parameterized type declaration generates a distinct type in the language that can then be inspected at run time through reflection, or leveraged by AoT compilation. `Box<int32>` and `Box<float64>` are two types, each with its own generated code and layout, and neither is assignable to the other. A wildcard `?` may stand for an unknown type argument at a use site. The full rules — declaration, bounds, deduction, specialization, and instantiation across archive boundaries — are Templates & Wildcards §11.

> *Discussion.* As of 0.27.0 the local-declaration path does not yet enforce initializer compatibility between distinct reference types — a cross-parameterization assignment such as `Box<float64> b = a` with `a : Box<int32>` compiles, and reading through it reinterprets the source's layout (the distinct layouts are why the read is wrong). Static-field initializers already reject this as `CAJETA_ERROR_INITIALIZER_TYPE_MISMATCH`. The gap is recorded as disabled pinning tests in `test/type/AssignmentCompatibilityTests.cpp`.
