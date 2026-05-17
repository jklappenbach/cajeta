# Annotations.md

Catalog of every annotation cajeta recognizes today, plus the
planned Lombok-mirror surface that lands in stages, plus the new
`@Encoding` annotation for views.

The annotation system is a compile-time-only metadata channel:
the parser parses `@Name(arg = value, ...)` into
`AnnotationInstance` records attached to the element below,
which `Annotatable::findAnnotation(name)` lets the codegen + later
analysis stages consult. Most annotations have semantics that
fire at codegen; a few (`@Throws`, `@Override`) are pure
declarations the compiler verifies but emits no code for.

## Table of contents

1. [Conventions](#conventions)
2. [Section 1 — Implemented today](#section-1--implemented-today)
3. [Section 2 — Lombok-mirror additions](#section-2--lombok-mirror-additions)
4. [Section 3 — `@Encoding` for views](#section-3--encoding-for-views)
5. [Section 4 — Aspirational (documented, not implemented)](#section-4--aspirational-documented-not-implemented)
6. [Section 5 — User-defined marker annotations](#section-5--user-defined-marker-annotations)
7. [Implementation notes](#implementation-notes)

---

## Conventions

- `@Name` — Java-style `@` prefix, PascalCase identifier matching
  the source-declared name. Stable across compiler versions;
  renaming is a breaking change.
- Parameter syntax: `@Name`, `@Name(value)`, `@Name(key = value)`,
  `@Name("string-arg")`, `@Name({"a", "b"})`. Multiple parameters
  separated by commas: `@Name(a = 1, b = 2)`.
- Single positional argument may omit the name: `@Order(2)` is
  shorthand for `@Order(value = 2)`.
- All framework annotations live in `package cajeta.<group>;`:
  - `cajeta.aot` — Component, Inject, Aspect, lifecycle, Order
  - `cajeta.lang` — Override, FunctionalInterface, NonNull, Sealed
  - `cajeta.wire` — BigEndian, LittleEndian, HostEndian, Align,
    Encoding
  - `cajeta.lint` — SuppressLint
  - `cajeta.ffi` — Native
  - `cajeta.synth` — AutoHash, Getter, Setter, ToString,
    EqualsAndHashCode, Data, Value, Builder, NoArgsConstructor,
    AllArgsConstructor, RequiredArgsConstructor, With, Cleanup
- The package qualifier is omitted in code samples below; the
  compiler resolves bare names via the implicit import of each
  `cajeta.*` annotation package.
- User-defined annotation types use the `@interface` declaration
  syntax (grammar at `antlr4/CajetaParser.g4:452`).

---

## Section 1 — Implemented today

These are wired into the compiler and consumed at codegen.
Symbol where each is handled is the authoritative pointer; the
design doc is the prose spec.

### Dependency injection / stereotypes (`cajeta.aot`)

| Annotation        | Target               | Effect                                          | Handler                                            | Spec                                 |
|-------------------|----------------------|-------------------------------------------------|----------------------------------------------------|--------------------------------------|
| `@Component`      | class                | Class joins the compile-time DI graph; the synthesized `__cajeta_inject` provides instances at injection sites. | `CajetaLlvmVisitor.h:255`                          | `AspectModel.md` § Components       |
| `@Component(name="primary")` | class     | Named variant of the above; multiple instances of the same type differentiated by name at injection sites via `@Inject(name="primary")`. | same                                               | `AspectModel.md` § Named components |
| `@Repository`     | class                | Stereotype for data-access components. Identical wiring to `@Component`; the marker is for `@Aspect` pointcuts to target the data layer specifically (`@Around(Repository.class)`). | `CajetaLlvmVisitor.h:256`                          | `AspectModel.md` § Stereotypes      |
| `@TestComponent`  | class                | Component visible only when `--profile=test` is active. Lets a test build substitute fakes / stubs for production components without touching production source. | `CajetaLlvmVisitor.h:257`                          | `AspectModel.md` § TestComponent    |
| `@Profile("name")` | class               | Component is conditional on the active build profile (default, test, custom). Class is registered with the DI graph only if its `@Profile` matches the active profile. | `CajetaLlvmVisitor.h` (profile-filtering)          | `AspectModel.md` § Profiles         |
| `@PostConstruct`  | method (instance, no args, void) | Called by the DI runtime once after construction + all field injections complete. Per-instance. | `ComponentInjectMethod.cpp:294`                    | `AspectModel.md` § Lifecycle        |
| `@PreDestroy`     | method (instance, no args, void) | Called before the component's scope drop. Per-instance. Useful for resources that need ordered teardown beyond what the destructor + field auto-drop cover. | `ComponentInjectMethod.cpp:320`                    | `AspectModel.md` § Lifecycle        |
| `@Inject`         | field, parameter     | Marks an injection site. The DI graph resolves it to a component instance at the point of construction (field) or invocation (parameter). | `CajetaModule.cpp:528`                             | `AspectModel.md` § Injection        |
| `@Inject(name="primary")` | same        | Disambiguates when multiple `@Component(name=...)` variants of the same type exist. | same                                               | same                                |

### Aspects (`cajeta.aot`)

| Annotation                          | Target            | Effect                                                                                  | Handler                                      | Spec                                                       |
|-------------------------------------|-------------------|-----------------------------------------------------------------------------------------|----------------------------------------------|------------------------------------------------------------|
| `@Aspect`                           | class             | Marks an aspect class. Itself also a `@Component`, so it can have `@Inject` fields.     | `CajetaLlvmVisitor.h:241`                    | `AspectModel.md` § Aspect class                            |
| `@Before(Marker.class)`             | method in `@Aspect` | Run before any method annotated with `Marker`. Pointcut is by marker annotation.       | `CajetaModule.cpp:192` (advice match)        | `AspectModel.md` § Advice kinds                            |
| `@After(Marker.class)`              | method in `@Aspect` | Run after, regardless of normal/throw exit.                                            | same                                         | same                                                       |
| `@AfterReturning(Marker.class)`     | method in `@Aspect` | Run after only on normal return. Optional `returning="binding"` captures the return value. | same                                       | `AspectModel.md` § AfterReturning                          |
| `@AfterThrowing(Marker.class)`      | method in `@Aspect` | Run after only on exception exit. Optional `throwing="binding"` captures the exception. | same                                       | `AspectModel.md` § AfterThrowing                           |
| `@Around(Marker.class)`             | method in `@Aspect` | Wraps the target. Body must invoke `@Original` to forward.                              | `Method.cpp` extracted-body machinery        | `AspectModel.md` § Around                                  |
| `@Order(n)`                         | aspect method     | Controls execution order when multiple aspects match the same target. Lower runs first. | `CajetaModule.cpp:339`                       | `AspectModel.md` § Ordering                                |
| `@Original`                         | call site inside `@Around` | Pseudo-marker the body uses to forward to the wrapped method. Lowered to a direct call of the extracted body. | grammar + codegen                            | `AspectModel.md` § Around                                  |

### Wire formats / views (`cajeta.wire`)

| Annotation        | Target           | Effect                                              | Handler                              | Spec                                  |
|-------------------|------------------|-----------------------------------------------------|--------------------------------------|---------------------------------------|
| `@BigEndian`      | view class       | Numeric fields read/written in big-endian order. Bswap inserted at each access. | `CajetaLlvmVisitor.h:368`            | `WireFormats.md` § Endianness         |
| `@LittleEndian`   | view class       | Same, little-endian.                                | `CajetaLlvmVisitor.h:370`            | same                                  |
| `@HostEndian`     | view class       | Explicit "use host order"; required even when host == desired so the declaration is unambiguous. | `CajetaLlvmVisitor.h:372`            | same                                  |
| `@Align(natural)` | view class       | Opt out of the default packed layout; insert ABI-natural padding between fields. | `CajetaLlvmVisitor.h:374`            | `WireFormats.md` § Alignment          |

### Method body source (`cajeta.ffi`, `cajeta.synth`)

| Annotation                | Target  | Effect                                                                              | Handler                | Spec                                  |
|---------------------------|---------|-------------------------------------------------------------------------------------|------------------------|---------------------------------------|
| `@Native(value="symbol")` | method  | Method body is a forwarding call to the named native symbol (typically a C runtime helper). Wrapper IR is trivially inlinable. | `Method.cpp:544`       | `Compilation.md` § FFI                |
| `@AutoHash`               | class   | Synthesize a `hash()` method walking all primitive + class-ref fields. If a user `hash()` exists, it wins. | `CajetaClass.cpp:417`  | `StandardLibrary.md` § Hashing        |

### Diagnostics (`cajeta.lint`)

| Annotation                              | Target          | Effect                                                                 | Handler                                       | Spec                                |
|-----------------------------------------|-----------------|------------------------------------------------------------------------|-----------------------------------------------|-------------------------------------|
| `@SuppressLint("rule-id", ...)`         | method, class   | Silences listed lint rule IDs within scope. No catch-all `"*"` allowed. | `MethodCallExpression.cpp:1267` (per-rule)    | `LintRules.md` § Suppression syntax |

---

## Section 2 — Lombok-mirror additions

Borrowed wholesale from Lombok's surface. The boilerplate-elimination
story Lombok pioneered for Java translates directly: Cajeta's
classes have constructors, accessors, equality, toString, and
builder patterns that the user writes by hand today. Each
annotation below replaces a block of mechanical code.

All synthesized methods are emitted as ordinary methods on the
class — visible via reflection (`CajetaReflect.md`), debuggable
with line maps pointing at the annotation site, overridable by
declaring the same method by hand (the hand-written version
wins).

### Accessors

#### `@Getter` on class or field

```cajeta
@Getter
public class Person {
    String name;
    int32 age;
}
```

Synthesizes `public String name()` and `public int32 age()` (size()-
style naming — methods, not `getName()`). On a single field:

```cajeta
public class Person {
    @Getter String name;   // only this field gets a getter
    int32 age;
}
```

Visibility can be tightened with `@Getter(level = "private")`.
Default is `public`.

#### `@Setter` on class or field

Synthesizes `public void name(String v)`. Same shape and
visibility-control as `@Getter`. Setter does NOT generate for
`final` fields.

For fields that hold class refs, the setter takes ownership of
the new value (`#`-style transfer in) and drops the previous
holder via the live-set claim.

### Equality + hashing + toString

#### `@EqualsAndHashCode` on class

Synthesizes `equals(Object other)` and `hash()` walking all
non-static, non-`@EqualsAndHashCode.Exclude` fields. Subsumes
the existing `@AutoHash`; `@AutoHash` becomes a thin alias for
`@EqualsAndHashCode(onlyHash = true)` for backwards compatibility.

```cajeta
@EqualsAndHashCode
public class Point {
    int32 x;
    int32 y;
    @EqualsAndHashCode.Exclude String label;   // not part of identity
}
```

#### `@ToString` on class

Synthesizes `String toString()` returning `ClassName(field1=val,
field2=val, ...)`. `@ToString.Exclude` on a field omits it. By
default, walks fields in declaration order.

### Constructors

#### `@NoArgsConstructor`

Synthesizes the zero-arg constructor. If any field is declared
without a default and not nullable, this fails with a clear
diagnostic ("`@NoArgsConstructor` on Foo: field `bar` has no
default and is non-null. Add a default, mark `@Nullable`, or
remove `@NoArgsConstructor`.").

#### `@AllArgsConstructor`

Synthesizes a constructor taking every field in declaration order.

#### `@RequiredArgsConstructor`

Synthesizes a constructor taking only fields that are `final`
or `@NonNull`-annotated. Lombok's most-used constructor variant.

### Bundles

#### `@Data` on class

Shorthand for `@Getter @Setter @ToString @EqualsAndHashCode
@RequiredArgsConstructor`. The default starting point for a
plain data carrier.

#### `@Value` on class

Immutable variant: `@Getter @ToString @EqualsAndHashCode
@AllArgsConstructor`. All fields treated as `final`. No setters.
Class itself is marked `final` (no subclassing — immutability
contract).

### Builders

#### `@Builder` on class

Synthesizes a builder class + `static Builder builder()` factory:

```cajeta
@Builder
public class Request {
    String url;
    int32 timeoutMs;
    String method;
}

Request r = Request.builder()
    .url("https://example.com")
    .timeoutMs(5000)
    .method("GET")
    .build();
```

Builder methods chain (`return this;`). `.build()` performs any
`@NonNull` validation. `@Builder.Default fieldname = expr` on a
field gives a default the builder uses if the caller doesn't
override.

### Null safety

#### `@NonNull` on parameter, field, return type

```cajeta
public Response fetch(@NonNull Url url) { ... }
```

- On parameter: synthesizes a null-check at method entry; throws
  `NullPointerException` (or traps under `--null-checks=trap`) on
  null. Composes with `--null-checks` from `CompilerModes.md`.
- On field: enforced at construction. `@RequiredArgsConstructor`
  picks up `@NonNull` fields. Field accesses don't re-check
  (compiler trusts the construction-time check).
- On return type: emits a null-check after every `return` in the
  body, plus a runtime check that the returned value isn't null.

### Immutability friend

#### `@With` on class or field

Synthesizes per-field copy-with mutators:

```cajeta
@With
public class Point {
    int32 x;
    int32 y;
}

Point p = stack Point { x: 1, y: 2 };
Point p2 = p.withX(10);  // returns a new Point with x=10, y=2
```

Cheap and idiomatic for `@Value`-style immutable types.

### Resource cleanup

#### `@Cleanup` on local variable

```cajeta
public void copy(InputStream in, OutputStream out) {
    @Cleanup BufferedInput buf = heap BufferedInput(in);
    // ... use buf ...
}   // buf.close() called here, automatically
```

Synthesizes a `try/finally` that calls a cleanup method (`close()`
by default; configurable via `@Cleanup("methodName")`) at scope
exit. Less load-bearing in Cajeta than in Java because most
resources are released by the destructor chain already; useful
for resources whose `close()` is logically separate from
destruction (a stream you want flushed before the buffer is
freed, a transaction you want committed/rolled-back).

### Deferred from Lombok

- `@SneakyThrows` — Lombok wraps checked exceptions as unchecked.
  Cajeta's error model has `Unrecoverable` / `Recoverable` already
  (`ErrorModel.md`); the right Cajeta equivalent depends on the
  final shape there.
- `@Synchronized` — Cajeta's concurrency story is fibers (`AsyncStatus.md`),
  not pthread mutexes; `synchronized` doesn't translate. A `@Mutex`
  annotation may be appropriate once the fiber-mutex API stabilizes.
- `@Slf4j` / `@Log` / `@Log4j` etc. — depends on the logging
  story. Defer until cajeta picks a default logging framework.
- `@var` / `@val` — Cajeta has `var` as a keyword; no annotation
  needed.

### `@AutoHash` deprecation path

`@AutoHash` becomes an alias for `@EqualsAndHashCode(onlyHash =
true)`. Existing usage continues to compile; the alias is
flagged by a soft-deprecation lint rule (`auto-hash-deprecated`)
recommending the switch to `@EqualsAndHashCode`. Removal is a
future major-version concern.

---

## Section 3 — `@Encoding` for views

The view system today (`WireFormats.md`) supports three layout
control knobs: endianness (`@BigEndian` etc.), alignment
(`@Align`), and length-prefix inline encoding for variable-size
trailing fields. All three are byte-level packed layouts —
appropriate for self-defined binary protocols but not for views
backed by external formats (JSON, MessagePack, Protocol Buffers,
Avro, Cap'n Proto).

`@Encoding` lets the user route a view's serialization +
deserialization through a user-defined codec class:

```cajeta
@Encoding(JsonEncoder.class)
public class UserMessage {
    int32 id;
    String name;
    String email;
}
```

When the view is materialized from a byte buffer, the compiler
emits a call to `JsonEncoder.decode(bytes)` returning a
`UserMessage`. When the view is serialized, the compiler emits
a call to `JsonEncoder.encode(view)` returning `byte[]`. The
view's field layout in memory is the normal class layout; the
*wire* layout is whatever the encoder produces.

### `Encoder<T>` interface

```cajeta
package cajeta.wire;

public interface Encoder<T> {
    byte[] encode(T value);
    T decode(byte[] bytes);
}
```

Two methods. No streaming variant in v1 — encoders work
buffer-to-buffer. A future `StreamingEncoder<T>` interface for
incremental encoding can land separately without disturbing the
`Encoder<T>` contract.

Implementations live wherever the user puts them — `cajeta.wire`
ships baseline implementations (`JsonEncoder`, `MsgPackEncoder`,
`ProtobufEncoder`) once their respective parsers/writers are in
place; users can roll their own (`MyCustomEncoder<T> implements
Encoder<T>`) for proprietary formats.

### `@Encoding(EncoderClass)` semantics

```cajeta
@Encoding(JsonEncoder.class)
public class UserMessage { ... }
```

- The class reference (`JsonEncoder.class`) must resolve to a
  type that implements `Encoder<UserMessage>` (Self-typed). The
  compiler verifies this at type-resolution time; mismatched
  type parameter is a static error with a clear message ("`@Encoding`
  on `UserMessage`: `JsonEncoder` implements `Encoder<JsonValue>`,
  not `Encoder<UserMessage>`. Either parameterize as
  `JsonEncoder<UserMessage>` or supply an encoder whose type
  parameter matches.").
- The compiler synthesizes a view constructor `UserMessage(byte[]
  bytes)` whose body calls `JsonEncoder.decode(bytes)` and copies
  the returned object's fields into `this`.
- For serialization, the compiler synthesizes `byte[]
  toBytes()` whose body calls `JsonEncoder.encode(this)`.
- The encoder instance is stateless (the `Encoder<T>` interface
  has no state contract); the compiler emits static-method
  calls. If a future stateful encoder is needed (e.g., schema-
  cached), an `@Encoding(MyEncoder.class, instance = "...")`
  variant binding to a `@Component` can land later.

### Composition with existing annotations

`@Encoding` is mutually exclusive with `@BigEndian` /
`@LittleEndian` / `@HostEndian` / `@Align`. If both are present,
the compiler emits an error: "`@Encoding` controls the wire
layout entirely; remove the endianness/alignment annotations."

Rationale: the encoder owns the wire format. Mixing packed-layout
control with a custom encoder is incoherent — the user expects
one or the other.

`@Encoding` does NOT change the in-memory representation. The
view's fields are laid out per the normal class layout (vtable
ptr + fields). The encoder operates on `T` values; the bytes are
materialized only at the encode/decode boundary.

### Example: JSON-backed view

```cajeta
package cajeta.wire;
public class JsonEncoder<T> implements Encoder<T> {
    public byte[] encode(T value) { ... }
    public T decode(byte[] bytes) { ... }
}

package com.example;
@Encoding(JsonEncoder<UserMessage>.class)
public class UserMessage {
    int32 id;
    String name;
    String email;
}

public class Service {
    public UserMessage parse(byte[] body) {
        return heap UserMessage(body);   // calls JsonEncoder.decode
    }
    public byte[] serialize(UserMessage m) {
        return m.toBytes();   // calls JsonEncoder.encode
    }
}
```

### Open questions for `@Encoding`

- **Class-literal syntax for templated encoders.** `JsonEncoder<UserMessage>.class`
  vs `JsonEncoder.class` with the parameter inferred. Lean: require
  the explicit parameter so the type-resolution check is
  unambiguous (no inference needed).
- **What about views that are nested inside other views?** Outer
  view has `@BigEndian`; inner field is of a `@Encoding`-marked
  type. Lean: the inner view is opaque to the outer — outer
  treats it as `byte[]` of inner's encoded length, and the inner
  encoder operates on that slice.
- **Length framing for the encoded blob inside an outer view.**
  Likely follow the existing length-prefix convention for
  variable-size trailing fields (`WireFormats.md` § Variable-size
  fields). Carry it across without re-design.
- **Error path when decode fails.** Encoder throws; cajeta's
  exception model catches at the view-constructor call site. The
  encoder's thrown exception type propagates to the caller.

---

## Section 4 — Aspirational (documented, not implemented)

Mentioned in design docs without compiler support. Listed here so
contributors don't reach for them expecting them to work.

### Java compat

| Annotation              | Intended target | Intended effect                                                                                  | Spec                                       |
|-------------------------|-----------------|--------------------------------------------------------------------------------------------------|--------------------------------------------|
| `@Override`             | method          | Verify the method actually overrides a super method; compile error if not.                       | (no doc yet)                               |
| `@FunctionalInterface`  | interface       | Verify the interface has exactly one abstract method.                                            | `Lambdas.md`                               |
| `@Sealed`               | class, interface | Restrict subtyping to a declared set.                                                            | `MemoryModel.md`-adjacent                  |

### Error model

| Annotation              | Intended target | Intended effect                                                                                  | Spec                |
|-------------------------|-----------------|--------------------------------------------------------------------------------------------------|---------------------|
| `@Throws(IOException.class, ...)` | method | Advisory declaration of exceptions a method may throw. Used by lint to surface uncaught throws at call sites. | `ErrorModel.md`     |

### Aspects (advanced)

| Annotation              | Intended target | Intended effect                                                                                  | Spec                       |
|-------------------------|-----------------|--------------------------------------------------------------------------------------------------|----------------------------|
| `@NoAdvice`             | method          | Opts the method out of aspect matching. Hot-path escape hatch.                                   | `AspectModel.md`           |
| `@DeclareParents`       | aspect method   | AspectJ-style introduction of a new interface implementation onto a target class.                | `AspectModel.md`           |

### Lint variants

| Annotation                       | Intended target | Intended effect                                          | Spec                       |
|----------------------------------|-----------------|----------------------------------------------------------|----------------------------|
| `@SuppressWarnings(...)`         | method, class   | Java compat alias for `@SuppressLint`.                   | `LintRules.md`             |
| `@SuppressDeprecation`           | method, class   | Targeted suppression for the deprecation-warning rule.   | `LintRules.md`             |
| `@SuppressUncaughtThrow`         | method          | Targeted suppression for `uncaught-throws` lint.         | `LintRules.md`, `ErrorModel.md` |

### Design / documentation hints

| Annotation              | Intended target | Intended effect                                                                                  | Spec                |
|-------------------------|-----------------|--------------------------------------------------------------------------------------------------|---------------------|
| `@Immutable`            | class           | Verifies all fields are `final` and field types are themselves immutable.                        | (no doc yet)        |
| `@JsonSerializable`     | class           | Hint to a future JSON-codec generator. Likely subsumed by `@Encoding(JsonEncoder.class)`.        | (no doc yet)        |
| `@NoVTable`             | class           | Suppress vtable header for classes that don't need virtual dispatch. Smaller instances.          | (no doc yet)        |
| `@Reflectable`          | class           | Opts in to full RTTI metadata (field list, annotations) for runtime reflection.                  | `CajetaReflect.md`  |
| `@Retained`             | annotation type | Marks an annotation as runtime-readable (Java's `@Retention(RUNTIME)` equivalent).               | `CajetaReflect.md`  |
| `@Trainable`            | field           | ML training-loop marker (parameter slot for gradient updates).                                   | `CajetaML.md`       |
| `@Transactional`        | method          | Aspect marker for transactional methods (user-defined, but reserved name).                       | `AspectModel.md`    |
| `@DisplayAs("name")`    | method, field   | Override the display name in IDE / debugger views.                                               | `Debugging.md`      |
| `@Parameter`            | parameter       | Reflection hint — retains the parameter name in the symbol table for introspection.              | `CajetaReflect.md`  |
| `@Scope("singleton"|"prototype"|"request")` | component class | DI scope. Today implicit per-call-site; this would make it declarative. | `AspectModel.md`    |

### Rejected

- **`@Borrow`** on a field — proposed during the auto-drop work to
  mark fields that hold borrows. Rejected as "a hack and an
  ugly one" — runtime self-discrimination (`FieldOwnership.md`
  § Solution B) makes the annotation unnecessary.

---

## Section 5 — User-defined marker annotations

Users declare their own annotations via the standard `@interface`
syntax (parser at `antlr4/CajetaParser.g4:452`):

```cajeta
package com.example;

public @interface Audited {
    String reason() default "";
}
```

These are first-class citizens of the annotation system —
`Annotatable::findAnnotation("Audited")` works on any element
they're applied to. The primary v1 use case is aspect pointcuts:

```cajeta
public class UserService {
    @Audited(reason = "PII access")
    public User load(int64 id) { ... }
}

@Aspect public class AuditAspect {
    @Before(Audited.class)
    public void log(JoinPoint jp) { ... }
}
```

The `@Before(Audited.class)` pointcut argument is a class
reference; the framework matches any method annotated `@Audited`.

User-defined annotation methods may take parameters (Java
convention) — `reason()` above. Aspect advice reads them via
`JoinPoint.getAnnotation(Audited.class).reason()`.

User annotations not consumed by an `@Aspect` are inert (no
code generated, no runtime overhead). They become useful via
either an aspect pointcut or via reflection (`CajetaReflect.md`).

---

## Implementation notes

### Annotation parsing

Grammar at `antlr4/CajetaParser.g4`:
- `annotation` — `'@' qualifiedName ('(' elementValuePairs? ')')?`
- `elementValuePairs` — `key = value, key = value, ...`
- `elementValue` — literal, array, or another annotation

Parsed into `AnnotationInstance` (`src/cajeta/type/Annotatable.h`)
which holds the name, the keyword-argument map, and the source
position. `Annotatable::findAnnotation(shortName)` is the canonical
lookup.

### Today's argument-parsing limitations

`Annotatable` captures annotation **names** uniformly but only
parses string arguments for `@SuppressLint` and value arguments
for `@Native`. Per `AspectModel.md` § A1, extending the parameter-
value capture to all annotations is the next infrastructure
piece — required for `@Order(2)`, `@Component(name = "primary")`,
`@Inject(name = "primary")`, the Lombok-mirror annotations'
configuration, and `@Encoding(JsonEncoder.class)`'s class-
literal argument.

### Annotation resolution order

1. Parser attaches `AnnotationInstance` to AST node.
2. `Structure::generatePrototype` (called once per top-level
   class during parse) populates the class's annotation map.
3. `CajetaLlvmVisitor` walks the class table after parse and
   dispatches per-annotation handlers (`Aspect` → aspect
   registration, `Component` → DI graph entry, etc.).
4. Method-level annotations (`@Before`, `@Native`, `@SuppressLint`)
   are consulted during codegen of the annotated method.
5. Field-level annotations (`@Inject`, `@BigEndian`, future
   `@Getter`) drive synthesis at class-codegen time.

### Synthesized-method conventions

Per the Lombok-mirror surface: every annotation-synthesized method
- carries source-position metadata pointing at the annotation
  site (so debugger / stack traces show the originating annotation
  rather than `<synthesized>`),
- is overridable by the user declaring a matching signature (the
  user's wins; the synthesizer skips synthesis when a match
  exists — same convention `@AutoHash` already follows at
  `CajetaClass.cpp:419`),
- emits a soft-deprecation lint if the user's hand-written
  version exists with a different signature (signal of probable
  intent drift),
- respects the `--source-tags` debug feature (`CompilerModes.md`)
  by passing source position to the chain-push helpers like any
  user-written method.

### Phasing for the Lombok-mirror work

Roughly in dependency order:

1. **Annotation argument capture for all annotations** (currently
   only `@SuppressLint` and `@Native` parse their args). Blocks
   everything else.
2. **`@Getter` / `@Setter`** — simplest field-walk synthesizers.
3. **`@ToString`** — same pattern.
4. **`@EqualsAndHashCode`** — composes with existing `@AutoHash`.
5. **`@NoArgsConstructor` / `@AllArgsConstructor` / `@RequiredArgsConstructor`** —
   constructor synthesis machinery.
6. **`@Data` / `@Value`** — bundle annotations, expand into the
   above.
7. **`@NonNull`** — null-check synthesis; integrates with
   `--null-checks` from `CompilerModes.md`.
8. **`@Builder`** — builder class synthesis, larger piece.
9. **`@With`** — copy-with synthesis.
10. **`@Cleanup`** — `try/finally` synthesis.

`@Encoding` is independent; can land in parallel after step 1.
