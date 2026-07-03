# The Cajeta Language — A Guide

A ground-up introduction to Cajeta: the primitives, operators, and keywords
first, then progressively larger topics built on them. Each section links to the
deep-dive spec for its area. Everything here reflects what the compiler actually
accepts — when the grammar reserves a word the compiler doesn't yet honor, it is
called out as *reserved*, not documented as a feature.

Cajeta is a statically-typed, compiled language in the C++/Java family with
Rust-style ownership: explicit `stack`/`heap` allocation, a static borrow
checker, single inheritance for state plus multiple inheritance for behavior,
C++-style monomorphized templates, and a first-class GPU/compute path. It
compiles to LLVM IR.

---

## 1. A first program

```cajeta
package demo;

public final class App {
    public static int32 run() {
        int32 sum = 0;
        for (int32 i = 1; i <= 10; i = i + 1) {
            sum = sum + i;
        }
        return sum;          // 55
    }
}
```

- Every source file opens with `package <name>;`. The file path under the source
  root mirrors the package (`demo/App.cajeta`).
- Code lives in types; there are no free functions. A program is entered through
  a `public static` method named on the command line as `package.Class.method`
  (e.g. `demo.App.run`) — there is no special `main` symbol.
- `//` line comments and `/* … */` block comments are the two comment forms.

---

## 2. Primitive types

Cajeta has only **explicit-width** numeric primitives — there is no `int`,
`long`, `float`, or `double`. Every cross-width conversion is an explicit cast
(§4); there is no implicit widening.

| Family | Types | Backing |
|--------|-------|---------|
| Boolean | `boolean` | i1 |
| Character | `char` | a **32-bit Unicode codepoint** (i32) — `'A'` is 65, `'😀'` is 0x1F600 |
| Signed integer | `int8` `int16` `int32` `int64` `int128` | iN |
| Unsigned integer | `uint8` `uint16` `uint32` `uint64` `uint128` | iN |
| Floating-point (IEEE) | `float16` `float32` `float64` `float128` | half / float / double / fp128 |
| Brain float | `bfloat16` | LLVM `bfloat` (16-bit, float32-width exponent) — the ML training dtype |
| Low-precision floats | `float4e2m1` `float6e2m3` `float6e3m2` `float8e4m3` `float8e5m2` `float8e4m3fnuz` `float8e5m2fnuz` | OCP Microscaling formats — **storage only** today (opaque iN; arithmetic/conversion helpers are future work), for ML kernels |
| Raw pointer | `pointer` | an opaque address; low-level/interop use |

Notes grounded in the type bootstrap (`src/cajeta/type/CajetaType.cpp`):

- **There is no `byte` type.** The canonical byte buffer is `int8[]` (or
  `uint8[]`). `String.bytes` is `int8[]`; `String.bytes()` returns `int8[]`.
- `uchar` is a **deprecated** alias for `uint8`; new code should write `uint8`.
- `char` is a codepoint, not a byte — don't use it for binary data.
- `void` is a return-position-only type.

### Literals

```cajeta
42            // int32
42L           // int64 (L suffix)
0xFF  0b1010  017    // hex / binary / octal (a leading 0 is octal, C-style)
1_000_000     // underscores group digits
3.14   3.14f  // float64; f suffix is float32
1.5e-9        // scientific
'A'   '\n'   '😀'   // char (codepoint)
"hello"       // String (a class — see §11), not a primitive
"""           // text block (multi-line)
multi
line"""
true  false   // boolean
null          // the null reference
```

There is no unsigned-literal suffix (`2u` is a parse error); write the value and
let the target type carry the signedness.

See [`specification/lang/Primitives.md`](../specification/lang/Primitives.md),
[`specification/lang/FloatingPointModel.md`](../specification/lang/FloatingPointModel.md),
[`specification/lang/String.md`](../specification/lang/String.md) (`char` semantics), and
[`specification/lang/EncodingPrefixedLiterals.md`](../specification/lang/EncodingPrefixedLiterals.md).

---

## 3. Operators

| Group | Operators |
|-------|-----------|
| Arithmetic | `+` `-` `*` `/` `%` |
| Unary | `-` (negate) `!` (logical not) `~` (bitwise not) |
| Increment / decrement | `++` `--` (prefix and postfix) |
| Comparison | `==` `!=` `<` `<=` `>` `>=` |
| Logical (short-circuit) | `&&` `\|\|` |
| Bitwise | `&` `\|` `^` |
| Shift | `<<` `>>` `>>>` (unsigned right) |
| Assignment | `=` and compound `+=` `-=` `*=` `/=` `%=` `&=` `\|=` `^=` `<<=` `>>=` `>>>=` |
| Ternary | `cond ? a : b` |
| Type test / cast | `x instanceof T`, `(T) x` |
| Member / call / index | `.`  `()`  `[]` |
| Method reference / lambda | `::`  `->` |
| Ownership transfer | `#` (prefix — see §6) |

Integer overflow and divide-by-zero are checked by default (configurable; see
[`CompilerModes.md`](../specification/buildtool/CompilerModes.md)). User types can define most operators
via `operator` overloads — see [`OperatorOverloading.md`](../specification/lang/OperatorOverloading.md).

> On value types (Vector/Matrix) the comparison operators produce a *per-lane
> mask*, not a single boolean; reduce with `.all()`/`.any()` and blend with
> `.select(a, b)`. See [`gpu/MaskSelect.md`](../specification/gpu/MaskSelect.md).

---

## 4. Variables, `var`, and casts

```cajeta
int32 a = 5;
var   b = a + 1;          // type inferred from the initializer (int32)
int64 wide   = (int64) a; // explicit widening cast
int32 narrow = (int32) wide;
float64 f    = (float64) a;
```

- `var` infers a local's type from its initializer, and the per-element type in a
  for-each loop (`for (var x : xs)`). It is **not** allowed for lambda parameters.
- A variable read before it is definitely assigned is a compile error
  (`CAJETA_ERROR_VARIABLE_NOT_ASSIGNED`).
- There is no implicit numeric conversion; widen and narrow with `(T) x`.

---

## 5. Allocation: `stack` and `heap`

Every class/view instance is created with an explicit placement prefix. `new` does
not exist.

```cajeta
Point a = stack Point(3, 4);            // lives in the stack frame
Point b = heap  Point(5, 12);           // malloc'd
Point c = stack Point { x: 1, y: 2 };   // aggregate initializer
Point d;                                // null reference; reading before assignment is rejected
```

A type is **one type** whether it lives on the stack or the heap — the storage
mode is a property of the *value*, not the type, and the borrow checker tracks
the lifetime. Class instances always pass and return by pointer (no slicing).

`shared` is the third placement, used inside GPU kernels for workgroup-shared
memory (§16). See [`specification/lang/UnifiedClasses.md`](../specification/lang/UnifiedClasses.md) and
[`specification/lang/MemoryModel.md`](../specification/lang/MemoryModel.md).

---

## 6. Ownership, borrowing, and `#`-transfer

Every owned value has exactly one owner. **Plain assignment borrows; `#name`
transfers ownership.**

```cajeta
public void demo() {
    MyClass a = heap MyClass();
    MyClass b = a;        // borrow — `a` still owns; `b` must not outlive a's scope
    MyClass c = #a;       // transfer — `c` owns; reading `a` after this is a compile error
}
```

The borrow checker is **static**. Use-after-move, borrow-escape-on-return,
alias-mutation, and definite-assignment violations are caught at compile time
(`CAJETA_ERROR_USE_AFTER_MOVE`, `CAJETA_ERROR_BORROW_ESCAPE`, …).

Reclamation is automatic at scope exit via a per-thread drop chain (entries fire
in reverse declaration order, and the throw path unwinds them so drops run on the
exceptional path too). To release a value early, **reassign or null it** (`x =
null`) — there is no `delete`/`free`. See
[`specification/lang/MemoryModel.md`](../specification/lang/MemoryModel.md),
[`specification/lang/FieldOwnership.md`](../specification/lang/FieldOwnership.md),
[`specification/lang/OwnershipTransfer.md`](../specification/lang/OwnershipTransfer.md), and
[`specification/lang/BorrowSoundness.md`](../specification/lang/BorrowSoundness.md).

---

## 7. Control flow

```cajeta
if (x > 0) { … } else if (x < 0) { … } else { … }

for (int32 i = 0; i < n; i = i + 1) { … }   // C-style
for (T item : items) { … }                  // for-each over arrays/iterables
while (cond) { … }
do { … } while (cond);

switch (tag) {                              // integer subjects only
    case 0: …; break;
    case 1: …; break;
    default: …;
}

outer: for (…) {
    for (…) {
        break outer;       // labeled break/continue jump to the named loop
    }
}
return value;
```

Implemented: `if`/`else`, both `for` forms, `while`, `do`/`while`, `switch`
(integer subjects), `break`/`continue` (including **labeled**), `return`.
*Reserved but not implemented:* `goto`, `yield`, `assert` — avoid them.

---

## 8. Functions, function types, and lambdas

Methods live on types. A method's modifiers, return type, name, and parameter
list precede its body:

```cajeta
public static int32 add(int32 a, int32 b) { return a + b; }
```

Functions are **first-class values** with the type `(P1, P2) -> R`:

```cajeta
(int32, int32) -> int32 op = (a, b) -> a + b;   // lambda
int32 s = op(2, 3);                              // 5

Stream<String> names = people.stream().map<String>(Person::getName);  // method reference
```

- Lambdas may have expression or block bodies; parameter types infer from the
  target type. Captures are by borrow (lifetime-tracked); `#name` in a capture
  transfers ownership in.
- **Arrays of functions** use grouping parens: `((int32)->int32)[] ops = { f, g
  };` then `ops[i](x)`. Without the parens, `(int32)->int32[]` is a function
  *returning* `int32[]`.
- Method references: `Class::staticMethod`, `instance::method`,
  `Class::instanceMethod`, `Class::heap` (constructor).

See [`specification/lang/Lambdas.md`](../specification/lang/Lambdas.md) and
[`specification/lang/MethodLevelTemplate.md`](../specification/lang/MethodLevelTemplate.md).

### Named arguments

Any call — method, static, or constructor — may pass its arguments **by name**,
using the parameter name followed by a colon:

```cajeta
public class Rect {
    public int32 w;
    public int32 h;
    public Rect(int32 w, int32 h) { this.w = w; this.h = h; }
    public int32 area(int32 scale, int32 bias) { return this.w * this.h * scale + bias; }
}

Rect r = heap Rect(w: 4, h: 3);            // named constructor args
int32 a = r.area(scale: 2, bias: 1);       // named method args  → 25
int32 b = r.area(2, bias: 1);              // positional prefix + named suffix → 25
```

- **Matched by name, so order is free.** `r.area(bias: 1, scale: 2)` binds the
  same as `r.area(scale: 2, bias: 1)` — the labels, not the positions, decide
  which parameter each value fills.
- **Positional prefix, then named suffix.** A call may start with positional
  arguments and switch to named ones: `area(2, bias: 1)` binds `scale` by position
  and `bias` by name. The named suffix is still order-free
  (`combine(1, c: 3, b: 2)` and `combine(1, b: 2, c: 3)` are equal).
- **A positional argument may not follow a named one.** Once you name an argument,
  every following argument must also be named — `area(scale: 2, 1)` is a **compile
  error** (`LANG-NAMEDARG`), not a silent re-interpretation. (A named argument also
  can't re-target a parameter the positional prefix already filled.)
- The same `name: value` shape is used by **struct aggregate initializers**
  (`Foo { field: value, … }`, see §5) and by the XPU **kernel launch** form
  (`kernel.launch(stream, grid: […], block: […])`, see the XPU guide) — there the
  unlabeled `stream` alongside labeled `grid`/`block` is handled by a dedicated
  launch lowering, not the general rule above.

---

## 9. Classes, interfaces, enums, and views

### Classes

Single inheritance of **state**, multiple inheritance of **behavior**. Every
instance carries a vtable pointer at offset 0, so dispatch is uniform across
storage modes. A subclass overrides a method by re-declaring it with the same
signature — there is no `override` keyword.

```cajeta
public class Shape  { public int32 area() { return 0; } }
public class Square extends Shape {
    int32 side;
    public Square(int32 s) { this.side = s; }
    public int32 area() { return this.side * this.side; }   // overrides Shape.area
}

Shape s = heap Square(5);   // 25 via vtable
```

Diamond inheritance resolves through a hash-based vtable lookup. See
[`specification/lang/UnifiedClasses.md`](../specification/lang/UnifiedClasses.md) and
[`specification/lang/MultiClassing.md`](../specification/lang/MultiClassing.md).

### Interfaces

Pure behavior contracts; an instance reference to an interface is a fat pointer
carrying a kind tag. Implement with `implements`.

### Enums

```cajeta
public enum Color { RED, GREEN, BLUE; }
```

### Value types (`@ValueType`)

A plain class of primitive fields can be marked a by-value POD (`@ValueType`) — it
is copied like a primitive rather than referenced, and is the basis for the
GPU/SIMD `Vector`/`Matrix` types. See
[`gpu/ValueTypeCatalog.md`](../specification/gpu/ValueTypeCatalog.md).

### Views

A `view` is a typed, zero-copy window over an `int8[]`, with layout fixed by
endianness annotations — for binary protocols and wire formats:

```cajeta
@LittleEndian
public view PacketHeader {
    int32 magic;
    int16 version;
    int32 payloadLength;
}

PacketHeader h = stack PacketHeader(buf);   // borrow over buf; no copy
```

Supports `@BigEndian`/`@LittleEndian`/`@HostEndian`/`@Align`, nested views, and
borrow vs ownership-transfer construction. See [`specification/lang/Views.md`](../specification/lang/Views.md).

> `structure` and `record` are reserved words but are **not** implemented type
> forms today — use `class`, `view`, or `@ValueType`.

---

## 10. Templates and wildcards

True C++-style templates — full monomorphization per instantiation, no type
erasure. Type parameters are class-level or method-level.

```cajeta
public class Box<T> {                 // class-level
    T value;
    public Box(T v) { this.value = v; }
    public T get() { return this.value; }
}

public static R fold<R, T>(R seed, T[] items, (R, T) -> R fn) {  // method-level
    R acc = seed;
    for (T x : items) { acc = fn(acc, x); }
    return acc;
}
```

Bounded templates (`<T extends Bound>`), wildcards (`?`, `? extends Bound`, `?
super Bound`), capture conversion, and PECS reads/writes are all supported, with
at-least-Java-strength inference. See [`TemplateWildcard.md`](../specification/cajeta-templates/TemplateWildcard.md),
[`CaptureConversion.md`](../specification/cajeta-templates/reified-capture-spec.md), and
[`specification/lang/NumericBoundedTemplates.md`](../specification/lang/NumericBoundedTemplates.md).

---

## 11. Strings and collections

`String` is a **class** (in `cajeta.lang`), not a primitive; string literals are
`String` instances and `+` autostringifies operands. The collection library lives
in `cajeta.collection` (`ArrayList`, `HashMap`, …).

**Every collection — including arrays and `String` — reports its element count
with `count()`, never `length` or `size`:**

```cajeta
int32[] xs = heap int32[4];
int32 n = xs.count();              // 4
ArrayList<int32> list = heap ArrayList<int32>();
int32 m = list.count();
```

(`Vector.length()` is the geometric magnitude of a SIMD vector — a different
operation.) See [`specification/collection/Collections.md`](../specification/collection/Collections.md),
[`specification/lang/String.md`](../specification/lang/String.md), and
[`specification/hash/Hashing.md`](../specification/hash/Hashing.md).

---

## 12. Errors

`throw` / `try` / `catch` / `finally` over an `Exception` hierarchy, with `throws`
clauses. **Recoverable** exceptions are expected and catchable; **unrecoverable**
ones abort the process with a stderr dump. Throw sites can capture a native stack
trace (gated by `--stack-trace-capture`).

```cajeta
try {
    Connection c = Connection.builder().host(null).build();
} catch (NullPointerException e) {
    log.warn("bad config: {}", e.getMessage());
}
```

See [`specification/error/ErrorModel.md`](../specification/error/ErrorModel.md).

---

## 13. Concurrency

Structured concurrency: `async` declares a suspendable method (returning a
`Task<T>`); `await` suspends on one; `scope { … }` joins all children before it
exits; `spawn` launches a child inside a scope; `detach` is fire-and-forget.

```cajeta
public static async int32 fetchAll(String[] urls) {
    int32[] sizes = heap int32[urls.count()];
    scope {
        for (int32 i = 0; i < urls.count(); i = i + 1) {
            spawn fetchOne(urls[i], sizes, i);   // scope joins all spawned before continuing
        }
    }
    int32 total = 0;
    for (int32 s : sizes) { total = total + s; }
    return total;
}
```

The runtime schedules fibers over a work-stealing carrier pool — by default
`min(cpus, 4)` OS-thread carriers, so spawned tasks run in parallel (set
`CAJETA_CARRIERS=1` for deterministic single-carrier runs). See
[`specification/concurrent/Concurrency.md`](../specification/concurrent/Concurrency.md),
[`specification/concurrent/AsyncStatus.md`](../specification/concurrent/AsyncStatus.md), and the threading primitives in
[`specification/concurrent/Concurrency.md`](../specification/concurrent/Concurrency.md).

---

## 14. Streams

A pull-protocol `Stream<T>` (`cajeta.lang.stream`) with intermediate ops
(`filter`, `map`, `flatMap`, `peek`, `take`, `skip`, …) and terminals (`count`,
`forEach`, `reduce`, `fold`, `collect`, `anyMatch`/`allMatch`/`noneMatch`,
`findFirst`):

```cajeta
int32 total = xs.stream()
    .filter((x) -> x > 0)
    .map<int32>((x) -> x * x)
    .reduce(0, (a, b) -> a + b);
```

`.parallel()` runs the chain across a `scope`/`spawn` worker fan-out over a
splittable root. See [`specification/lang/stream/Streams.md`](../specification/lang/stream/Streams.md) and
[`specification/lang/stream/StreamParallelism.md`](../specification/lang/stream/StreamParallelism.md).

---

## 15. Annotations

A Lombok-style annotation system over the language's reflection: `@Getter`/
`@Setter`, `@Builder`, `@ToString`, `@EqualsAndHashCode`, `@Data`/`@Value`,
`@With`, `@NonNull`, view-layout annotations (`@LittleEndian`, …), `@Encoding`,
and the aspect/DI set (`@Aspect`, `@Component`, `@Inject`, `@Around`/`@Before`/
`@After`). See [`specification/reflect/Annotations.md`](../specification/reflect/Annotations.md),
[`specification/lang/AspectModel.md`](../specification/lang/AspectModel.md), and
[`specification/reflect/Reflection.md`](../specification/reflect/Reflection.md).

---

## 16. GPU / compute (XPU)

Cajeta compiles compute kernels to four backends from one source: CPU, Vulkan
(SPIR-V), AMD (ROCm), and NVIDIA (NVPTX). `@Kernel` marks a launchable entry,
`@Device` a device-callable helper; `Buffer<T>` is a device buffer; `shared`
declares workgroup memory.

```cajeta
@Kernel public static void saxpy(Buffer<float32> y, Buffer<float32> x,
                                 float32 a, uint32 n) {
    uint32 i = Thread.globalIdX();
    if (i < n) { y[i] = a * x[i] + y[i]; }
}
```

Resource logistics — occupancy, register budget, launch bounds — are tuned
**automatically**: the compiler pins each kernel's real launch workgroup size so
the backend budgets registers for the true occupancy (no spilling because the
backend assumed a worst-case 1024-thread workgroup). To override, annotate the
kernel with the portable, vendor-neutral `@Occupancy`:

```cajeta
@Kernel
@Occupancy(maxThreads = 256, minResident = 2, maxRegisters = 128)
public static void gemm(...) { ... }
```

All three parameters are optional. They lower per-backend (AMD →
`flat-work-group-size` / `waves-per-eu`; NVIDIA → `maxntid` / `minctasm` /
`maxnreg`) and are a no-op where a backend has no equivalent. An explicit
`@Occupancy` always wins over the automatic budgeting.

See [`gpu/xpu/CajetaXPU.md`](../specification/xpu/CajetaXPU.md),
[`gpu/CajetaGPU.md`](../specification/gpu/CajetaGPU.md), and the capability matrix
[`gpu/xpu/CajetaXPU-Matrix.md`](../specification/xpu/CajetaXPU-Matrix.md).

---

## 17. Keyword reference

**Implemented** — allocation `stack` `heap` `shared`; types `class` `interface`
`enum` `view`; modifiers `public` `private` `protected` `static` `final`
`abstract` `const` `operator`; control `if` `else` `for` `while` `do`
`switch` `case` `default` `break` `continue` `return`; exceptions `throw` `try`
`catch` `finally` `throws`; OO `extends` `implements` `super` `this`
`instanceof`; concurrency `async` `await` `spawn` `scope` `detach`; misc `var`
`import` `package` `true` `false` `null`; ownership marker `#`.

**Reserved but inert today** (parsed, but not yet honored — don't rely on them):
`structure` `record` `native` `transient` `volatile` `strictfp` `sealed`
`permits` `non-sealed` `goto` `yield` `assert`, and the module-system words
(`pModule` `open` `requires` `exports` `opens` `to` `uses` `provides` `with`
`transitive`).

---

## 18. Where to go next

| Topic | Doc |
|-------|-----|
| Class model + allocation | [`specification/lang/UnifiedClasses.md`](../specification/lang/UnifiedClasses.md) |
| Memory + ownership | [`specification/lang/MemoryModel.md`](../specification/lang/MemoryModel.md), [`specification/lang/FieldOwnership.md`](../specification/lang/FieldOwnership.md) |
| Primitives + floats | [`specification/lang/Primitives.md`](../specification/lang/Primitives.md), [`specification/lang/FloatingPointModel.md`](../specification/lang/FloatingPointModel.md) |
| Templates + wildcards | [`TemplateWildcard.md`](../specification/cajeta-templates/TemplateWildcard.md), [`CaptureConversion.md`](../specification/cajeta-templates/reified-capture-spec.md) |
| Lambdas + function types | [`specification/lang/Lambdas.md`](../specification/lang/Lambdas.md) |
| Operator overloading | [`OperatorOverloading.md`](../specification/lang/OperatorOverloading.md) |
| Strings + collections | [`specification/lang/String.md`](../specification/lang/String.md), [`specification/collection/Collections.md`](../specification/collection/Collections.md) |
| Streams | [`specification/lang/stream/Streams.md`](../specification/lang/stream/Streams.md), [`specification/lang/stream/StreamParallelism.md`](../specification/lang/stream/StreamParallelism.md) |
| Annotations + aspects | [`specification/reflect/Annotations.md`](../specification/reflect/Annotations.md), [`specification/lang/AspectModel.md`](../specification/lang/AspectModel.md) |
| Views / wire formats | [`specification/lang/Views.md`](../specification/lang/Views.md) |
| Concurrency | [`specification/concurrent/Concurrency.md`](../specification/concurrent/Concurrency.md) |
| Errors | [`specification/error/ErrorModel.md`](../specification/error/ErrorModel.md) |
| GPU / compute | [`gpu/xpu/CajetaXPU.md`](../specification/xpu/CajetaXPU.md), [`gpu/CajetaGPU.md`](../specification/gpu/CajetaGPU.md) |
| Compiler modes + flags | [`CompilerModes.md`](../specification/buildtool/CompilerModes.md) |
| Lint rules | [`LintRules.md`](../specification/lang/LintRules.md) |
