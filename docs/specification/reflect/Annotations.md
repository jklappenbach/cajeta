# Annotations

This is the catalog of every annotation cajeta recognizes. It includes the
Lombok-mirror surface (`@Getter` / `@Setter` / `@Data` / `@Builder` / …),
which is now implemented, and the `@Encoding` annotation for views.

The annotation system is a compile-time-only metadata channel. The
parser parses `@Name(arg = value, ...)` into `AnnotationInstance`
records attached to the element below, which
`Annotatable::findAnnotation(name)` lets the codegen + later analysis
stages consult. Argument values of every kind are captured uniformly by
the annotation parser (`AnnotationParser.cpp`). That covers strings,
integers, booleans, class literals (`Foo.class`), arrays, and nested
annotations, and none of them is a per-annotation special case. Most
annotations have semantics that fire at codegen. A couple (`@Override`,
the aspirational `@Throws`) are declaration/verification annotations
the compiler checks but emits no code for.

## Table of contents

1. [Conventions](#conventions)
2. [Section 1 — Implemented today](#section-1--implemented-today)
3. [Section 2 — Lombok-mirror annotations](#section-2--lombok-mirror-annotations-implemented)
4. [Section 3 — `@Encoding` for views](#section-3--encoding-for-views)
5. [Section 4 — Aspirational (documented, not implemented)](#section-4--aspirational-documented-not-implemented)
6. [Section 5 — User-defined marker annotations](#section-5--user-defined-marker-annotations)
7. [Implementation notes](#implementation-notes)

---

## Conventions

- `@Name` is a Java-style `@` prefix with a PascalCase identifier
  matching the source-declared name. It is stable across compiler
  versions, and renaming is a breaking change.
- Parameter syntax: `@Name`, `@Name(value)`, `@Name(key = value)`,
  `@Name("string-arg")`, `@Name({"a", "b"})`. Multiple parameters
  separated by commas: `@Name(a = 1, b = 2)`.
- Single positional argument may omit the name: `@Order(2)` is
  shorthand for `@Order(value = 2)`.
- All framework annotations live in `package cajeta.<group>;`:
  - `cajeta.aot`: Component, Inject, Aspect, lifecycle, Order
  - `cajeta.lang`: Override, FunctionalInterface, NonNull, Sealed
  - `cajeta.wire`: BigEndian, LittleEndian, HostEndian, Align,
    Encoding
  - `cajeta.lint`: SuppressLint
  - `cajeta.ffi`: Native
  - `cajeta.synth`: AutoHash, Getter, Setter, ToString,
    EqualsAndHashCode, Data, Value, Builder, NoArgsConstructor,
    AllArgsConstructor, RequiredArgsConstructor, With, Cleanup
- The package qualifier is omitted in code samples below, because the
  compiler resolves bare names via the implicit import of each
  `cajeta.*` annotation package.
- User-defined annotation types use the `annotation` declaration
  syntax (`annotationTypeDeclaration`, `antlr4/CajetaParser.g4:471`).

---

## Section 1 — Implemented today

These are wired into the compiler and consumed at codegen. The
symbol where each is handled is the authoritative pointer, and the
design doc is the prose spec.

### Dependency injection — the DI substrate (`cajeta.aot`)

`@Component`, `@Inject`, `@Factory` and `@Scope` are the core DI substrate, one
indivisible mechanism the compiler owns. A core `@Inject` is incoherent without
core producers to resolve against. Scopes are core too, as a mechanism with no
names of its own, and primavera publishes `Request` and `Session` on its own
types. The opinion layer on top is framework policy and lives in primavera.
That layer is those scope names, the web model, stereotypes, and deployment
profiles.

| Annotation        | Target               | Effect                                          | Handler                                            | Spec                                 |
|-------------------|----------------------|-------------------------------------------------|----------------------------------------------------|--------------------------------------|
| `@Component`      | class                | Declares an injectable node in the compile-time DI graph. Optional `name=` qualifier. | `CajetaLlvmVisitor.h:352`, `CajetaModule.cpp`      | [`AspectModel.md`](../lang/AspectModel.md) § DI substrate |
| `@Inject`         | field, parameter     | Marks an injection site. The compiler resolves it to a provider at construction (field) or invocation (parameter). It takes the component's scope, and `scope = "Owner"` or `"Transient"` asks for a fresh instance of a component that declares none. | `CajetaModule.cpp`                             | [`AspectModel.md`](../lang/AspectModel.md) § `@Inject` |
| `@Inject(name="primary")` | same        | Disambiguates when multiple named providers of the same type exist (the unqualified provider is the implicit default). | same                                               | same                                |
| `@Scope("Name")` | method, class | Publishes a component lifetime: one activation of the method, or one instance of the class. A component consumes it with `@Component(scope = "Name")`. The compiler emits both ends, so a throw ends a method scope too. | `CajetaModule.cpp`, `Method.cpp` | [`AspectModel.md`](../lang/AspectModel.md) § `@Inject` and `@Scope` |
| `@Factory`        | class (+ methods)    | One method per produced type — third-party/unowned types, assisted (non-`@Inject`) args, init beyond the ctor. Resolves on method signature; returns a fresh owned `#T`. | `CajetaModule.cpp`, `FactoryProviderMethod.cpp`    | [`AspectModel.md`](../lang/AspectModel.md) § `@Factory` |
| `@PostConstruct` / `@PreDestroy` | method | Lifecycle hooks — run after injection / on drop. | `ComponentInjectMethod.cpp:294`/`:320`             | [`AspectModel.md`](../lang/AspectModel.md) § Lifecycle |

> **Substrate is core; only policy is primavera.** `@Component` / `@Inject` /
> `@Factory`, lifecycle hooks, and the `@Aspect` / `@Before` / `@After` /
> `@AfterReturning` / `@AfterThrowing` / `@Around` / `@Original` / `@Order` family
> are core language, recognized and lowered by the compiler
> (`CajetaLlvmVisitor.h`, `CajetaModule.cpp`, `ComponentInjectMethod.cpp`,
> `FactoryProviderMethod.cpp`). The opinion layer is primavera policy
> (`org.cajeta.primavera`). That layer is stereotypes (`@Repository`,
> `@Service`), deployment `@Profile`, `@TestComponent`, request/session scope,
> and the web model (`@RestServer`). See
> [`AspectModel.md`](../lang/AspectModel.md) for the core/policy split.

### Wire formats / views (`cajeta.wire`)

| Annotation        | Target           | Effect                                              | Handler                              | Spec                                  |
|-------------------|------------------|-----------------------------------------------------|--------------------------------------|---------------------------------------|
| `@BigEndian`      | view class       | Numeric fields read/written in big-endian order. Bswap inserted at each access. | `CajetaLlvmVisitor.h:552`            | `WireFormats.md` § Endianness         |
| `@LittleEndian`   | view class       | Same, little-endian.                                | `CajetaLlvmVisitor.h:554`            | same                                  |
| `@HostEndian`     | view class       | Explicit "use host order"; required even when host == desired so the declaration is unambiguous. | `CajetaLlvmVisitor.h:556`            | same                                  |
| `@Align(natural)` | view class       | Opt out of the default packed layout; insert ABI-natural padding between fields. | `CajetaLlvmVisitor.h:558`            | `WireFormats.md` § Alignment          |

### Method body source (`cajeta.ffi`, `cajeta.synth`)

| Annotation                | Target  | Effect                                                                              | Handler                | Spec                                  |
|---------------------------|---------|-------------------------------------------------------------------------------------|------------------------|---------------------------------------|
| `@Native(value="symbol")` | method  | Method body is a forwarding call to the named native symbol (typically a C runtime helper). Wrapper IR is trivially inlinable. | `Method.cpp:761`       | `Compilation.md` § FFI                |
| `@AutoHash`               | class   | Synthesize a `hash()` method walking all primitive + class-ref fields. If a user `hash()` exists, it wins. Also implied by `@Data` / `@Value`. | `CajetaClass.cpp:1204` (`synthesizeAutoHash`) | `Hashing.md` § @AutoHash      |

### Diagnostics (`cajeta.lint`)

| Annotation                              | Target          | Effect                                                                 | Handler                                       | Spec                                |
|-----------------------------------------|-----------------|------------------------------------------------------------------------|-----------------------------------------------|-------------------------------------|
| `@SuppressLint("rule-id", ...)`         | method, class   | Silences listed lint rule IDs within scope. No catch-all `"*"` allowed. | `CajetaLlvmVisitor.h:949` (per-rule)          | `LintRules.md` § Suppression syntax |

---

## Section 2 — Lombok-mirror annotations (implemented)

These are borrowed wholesale from Lombok's surface and implemented today.
The synthesizers live in `CajetaClass.cpp` (`synthesizeGetters` 1265,
`synthesizeSetters` 1316, `synthesizeToString` 1379,
`synthesizeNoArgsConstructor` 1543, `synthesizeAllArgsConstructor` 1553,
`synthesizeRequiredArgsConstructor` 1574, `synthesizeWith` 1596,
`@NonNull` checks 1587, `@Builder` 1810, and the `@Data` / `@Value`
bundles). The boilerplate-elimination story Lombok pioneered for Java
translates directly, and each annotation below replaces a block of
mechanical code.

Two caveats vs. the Lombok surface:
- **`@EqualsAndHashCode` is not a standalone annotation.** Hash synthesis is
  driven by `@AutoHash` (and implied by `@Data` / `@Value`), and there is no
  separate `@EqualsAndHashCode` handler. Where the bundles below name it,
  read "the `@AutoHash` synthesis path."
- **`@Cleanup` is not implemented** (see § Resource cleanup below). It is
  documented here as planned.

All synthesized methods are emitted as ordinary methods on the
class. They are visible via reflection (`Reflection.md`), debuggable
with line maps pointing at the annotation site, and overridable by
declaring the same method by hand, in which case the hand-written
version wins.

### Accessors

#### `@Getter` on class or field

```cajeta
@Getter
public class Person {
    String name;
    int32 age;
}
```

This synthesizes `public String name()` and `public int32 age()`. The
naming follows the `size()` style, so the method is `name()` and never
`getName()`. On a single field:

```cajeta
public class Person {
    @Getter String name;   // only this field gets a getter
    int32 age;
}
```

Visibility can be tightened with `@Getter(level = "private")`.
The default is `public`.

#### `@Setter` on class or field

This synthesizes `public void name(String v)`. It has the same shape and
visibility-control as `@Getter`. A setter does NOT generate for
`final` fields.

For fields that hold class refs, the setter takes ownership of
the heap value (`#`-style transfer in) and drops the previous
holder via the live-set claim.

### hashing + toString

#### `@ToString` on class

It accepts `format` (enum, default `TO_STRING_PROPERTIES`):
- `TO_STRING_PROPERTIES` renders `ClassName(field1=val,field2=val, ...)`
- `TO_STRING_JSON` renders a JSON object literal. It is deferred to S-1102 (the `cajeta.codec.json` library). When that library ships, the synthesizer delegates to its writer. Until then, requesting `TO_STRING_JSON` is a compile error pointing at the deferral.

This synthesizes `String toString()`. `@ToString.Exclude` on a field omits it. By default, it walks fields in declaration order. Class-typed fields recurse via `field.toString()`, and null class fields render as `"null"`. Primitives render via their natural string form (int → decimal, float → `%g`, boolean → `true`/`false`).

### Constructors

#### `@NoArgsConstructor`

This synthesizes the zero-arg constructor. If any field is declared
without a default and not nullable, this fails with a clear
diagnostic ("`@NoArgsConstructor` on Foo: field `bar` has no
default and is non-null. Add a default, mark `@Nullable`, or
remove `@NoArgsConstructor`.").

#### `@AllArgsConstructor`

This synthesizes a constructor taking every field in declaration order.

#### `@RequiredArgsConstructor`

This synthesizes a constructor taking only fields that are `final`
or `@NonNull`-annotated. It is Lombok's most-used constructor variant.

### Bundles

#### `@Data` on class

`@Data` is shorthand for `@Getter @Setter @ToString @EqualsAndHashCode
@RequiredArgsConstructor`. It is the default starting point for a
plain data carrier.

#### `@Value` on class

`@Value` is the immutable variant, `@Getter @ToString @EqualsAndHashCode
@AllArgsConstructor`. All fields are treated as `final`, and there are
no setters. The class itself is marked `final`, so there is no
subclassing, which is the immutability contract.

### Builders

#### `@Builder` on class

This synthesizes a builder class + `static Builder builder()` factory:

```cajeta
@Builder
public class Request {
    String url;
    int32 timeoutMs;
    String method;
}

Request r #= Request.builder()
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

- On a parameter, it synthesizes a null-check at method entry and throws
  `NullReferenceException` (or traps under `--null-checks=trap`) on
  null. It composes with `--null-checks` from `CompilerModes.md`.
- On a field, it is enforced at construction. `@RequiredArgsConstructor`
  picks up `@NonNull` fields. Field accesses don't re-check, because
  the compiler trusts the construction-time check.
- On a return type, it emits a null-check after every `return` in the
  body, plus a runtime check that the returned value isn't null.

### Immutability friend

#### `@With` on class or field

This synthesizes per-field copy-with mutators:

```cajeta
@With
public class Point {
    int32 x;
    int32 y;
}

Point p = stack Point { x: 1, y: 2 };
Point p2 = p.withX(10);  // returns a new Point with x=10, y=2
```

It is cheap and idiomatic for `@Value`-style immutable types.

### Resource cleanup

#### `@Cleanup` on local variable — planned, not implemented

> **Status: not shipped.** No `@Cleanup` handler exists in the compiler today,
> and the design below is retained as the intended shape. Until it lands, rely
> on the destructor / scope-exit drop chain or an explicit `try`/`finally`.

```cajeta
public void copy(InputStream in, OutputStream out) {
    @Cleanup BufferedInput buf = heap BufferedInput(in);
    // ... use buf ...
}   // buf.close() called here, automatically
```

This synthesizes a `try/finally` that calls a cleanup method at scope
exit. The method is `close()` by default and configurable via
`@Cleanup("methodName")`. It is less load-bearing in Cajeta than in
Java, because most resources are released by the destructor chain
already. It is useful for resources whose `close()` is logically
separate from destruction, such as a stream you want flushed before the
buffer is freed or a transaction you want committed/rolled-back.

### Deferred from Lombok

- `@SneakyThrows` is how Lombok wraps checked exceptions as unchecked.
  Cajeta's error model has `Unrecoverable` / `Recoverable` already
  (`ErrorModel.md`), and the right Cajeta equivalent depends on the
  final shape there.
- `@Synchronized` doesn't translate, because Cajeta's concurrency story
  is fibers (`AsyncStatus.md`) and has no pthread mutexes. A `@Mutex`
  annotation may be appropriate once the fiber-mutex API stabilizes.
- `@Slf4j` / `@Log` / `@Log4j` etc. depend on the logging
  story. Defer them until cajeta picks a default logging framework.
- `@var` / `@val` are unnecessary, because Cajeta has `var` as a
  keyword.

### `@AutoHash` deprecation path

`@AutoHash` becomes an alias for `@EqualsAndHashCode(onlyHash =
true)`. Existing usage continues to compile. The alias is
flagged by a soft-deprecation lint rule (`auto-hash-deprecated`)
recommending the switch to `@EqualsAndHashCode`. Removal is a
future major-version concern.

---

## Section 3 — `@Encoding` for views

The view system today (`WireFormats.md`) supports three layout
control knobs: endianness (`@BigEndian` etc.), alignment
(`@Align`), and length-prefix inline encoding for variable-size
trailing fields. All three are byte-level packed layouts. They are
appropriate for self-defined binary protocols. They are not appropriate
for views backed by external formats (JSON, MessagePack, Protocol
Buffers, Avro, Cap'n Proto).

`@Encoding` lets you route a view's serialization +
deserialization through a user-defined codec class. For
binary formats (MessagePack, Protobuf, Avro, Cap'n Proto) it looks
like this:

```cajeta
@Encoding(MsgPackEncoder.class)
public class UserMessage {
    int32 id;
    String name;
    String email;
}
```

For JSON, do NOT use `@Encoding`. Call `Json.parse<T>(bytes)` /
`Json.toBytes(value)` directly, with per-field `@JsonProperty` /
`@JsonIgnore` annotations to control the mapping. See
`docs/specification/codec/json/Json.md` § Tier 1 for the rationale.

When the view is materialized from a byte buffer, the compiler
emits a call to `MsgPackEncoder.decode(bytes)` returning a
`UserMessage`. When the view is serialized, the compiler emits
a call to `MsgPackEncoder.encode(view)` returning `int8[]`. The
view's field layout in memory is the normal class layout, and the
wire layout is whatever the encoder produces.

### `Encoder<T>` interface

```cajeta
package cajeta.wire;

public interface Encoder<T> {
    int8[] encode(T value);
    T decode(int8[] bytes);
}
```

It has two methods. There is no streaming variant in v1, so encoders
work buffer-to-buffer. A future `StreamingEncoder<T>` interface for
incremental encoding can land separately without disturbing the
`Encoder<T>` contract.

Implementations live wherever you put them. `cajeta.wire`
ships baseline implementations (`MsgPackEncoder`, `ProtobufEncoder`,
`AvroEncoder`) once their respective parsers/writers are in place, and
you can roll your own (`MyCustomEncoder<T> implements Encoder<T>`)
for proprietary formats.

**JSON does NOT use `@Encoding`.** `@Encoding` is the right shape for
binary formats whose wire bytes are opaque without a class-level
binding. JSON's format is fixed, and what varies between classes is
the field-name mapping plus include / exclude / required behavior.
That is per-field annotation territory (`@JsonProperty`, `@JsonIgnore`,
`@JsonRequired`, etc.) consumed by the `Json.parse<T>` /
`Json.toBytes` codec-direct entry points. See
`docs/specification/codec/json/Json.md` § Tier 1 for the full design and
the rationale.

### `@Encoding(EncoderClass)` semantics

```cajeta
@Encoding(MsgPackEncoder.class)
public class UserMessage { ... }
```

- The class reference (`MsgPackEncoder.class`) must resolve to a
  type that implements `Encoder<UserMessage>` (Self-typed). The
  compiler verifies this at type-resolution time, and a mismatched
  type parameter is a static error with a clear message ("`@Encoding`
  on `UserMessage`: `MsgPackEncoder` implements `Encoder<MsgPackValue>`,
  not `Encoder<UserMessage>`. Either parameterize as
  `MsgPackEncoder<UserMessage>` or supply an encoder whose type
  parameter matches.").
- The compiler synthesizes a view constructor `UserMessage(int8[]
  bytes)` whose body calls `MsgPackEncoder.decode(bytes)` and copies
  the returned object's fields into `this`.
- For serialization, the compiler synthesizes `int8[]
  toBytes()` whose body calls `MsgPackEncoder.encode(this)`.
- The encoder instance is stateless, since the `Encoder<T>` interface
  has no state contract, so the compiler emits static-method
  calls. If a future stateful encoder is needed (e.g., schema-
  cached), an `@Encoding(MyEncoder.class, instance = "...")`
  variant binding to a `@Component` can land later.

### Composition with existing annotations

`@Encoding` is mutually exclusive with `@BigEndian` /
`@LittleEndian` / `@HostEndian` / `@Align`. If both are present,
the compiler emits the error "`@Encoding` controls the wire
layout entirely; remove the endianness/alignment annotations."

The rationale is that the encoder owns the wire format. Mixing
packed-layout control with a custom encoder is incoherent, because you
expect one or the other.

`@Encoding` does NOT change the in-memory representation. The
view's fields are laid out per the normal class layout (vtable
ptr + fields). The encoder operates on `T` values, and the bytes are
materialized only at the encode/decode boundary.

### Example: MessagePack-backed view

```cajeta
package cajeta.wire;
public class MsgPackEncoder<T> implements Encoder<T> {
    public int8[] encode(T value) { ... }
    public T decode(int8[] bytes) { ... }
}

package com.example;
@Encoding(MsgPackEncoder<UserMessage>.class)
public class UserMessage {
    int32 id;
    String name;
    String email;
}

public class Service {
    public UserMessage parse(int8[] body) {
        return heap UserMessage(body);   // calls MsgPackEncoder.decode
    }
    public int8[] serialize(UserMessage m) {
        return m.toBytes();   // calls MsgPackEncoder.encode
    }
}
```

For JSON, the same `Service` shape is:

```cajeta
public class Service {
    public UserMessage parse(int8[] body) {
        return Json.parse<UserMessage>(body);
    }
    public int8[] serialize(UserMessage m) {
        return Json.toBytes(m);
    }
}
```

There is no class-level annotation on `UserMessage` and no encoder class
mentioned, and field-level mapping is handled by `@JsonProperty` etc.
where needed.

### Open questions for `@Encoding`

- **Class-literal syntax for templated encoders.** `MsgPackEncoder<UserMessage>.class`
  vs `MsgPackEncoder.class` with the parameter inferred. The lean is to
  require the explicit parameter so the type-resolution check is
  unambiguous, with no inference needed.
- **What about views that are nested inside other views?** The outer
  view has `@BigEndian`, and an inner field is of a `@Encoding`-marked
  type. The lean is that the inner view is opaque to the outer. The
  outer treats it as `int8[]` of the inner's encoded length, and the
  inner encoder operates on that slice.
- **Length framing for the encoded blob inside an outer view.**
  This likely follows the existing length-prefix convention for
  variable-size trailing fields (`WireFormats.md` § Variable-size
  fields). Carry it across without re-design.
- **Error path when decode fails.** The encoder throws, and cajeta's
  exception model catches at the view-constructor call site. The
  encoder's thrown exception type propagates to the caller.

---

## Section 4 — Aspirational (documented, not implemented)

These are mentioned in design docs without compiler support. They are
listed here so contributors don't reach for them expecting them to work.

> **`@Override` is implemented.** It is an accepted verification annotation,
> and there is no `override` keyword in the language. On its own it is
> inert. With the optional `from = "Parent"` element it checks that the
> named class is actually an ancestor and errors otherwise. The handler is
> `CajetaClass.cpp:3385`. Absence of `@Override`, or of `from=`, is fine and
> skips the check.
>
> **`@Sealed` is a recognized modifier.** It surfaces as
> `Modifiers.isSealed()` in reflection and is the gate for reflective
> private-member access (see `Reflection.md` § Access control). Full
> "restrict subtyping to a declared set" enforcement is still future work.

### Java compat (aspirational)

| Annotation              | Intended target | Intended effect                                                                                  | Spec                                       |
|-------------------------|-----------------|--------------------------------------------------------------------------------------------------|--------------------------------------------|
| `@FunctionalInterface`  | interface       | Verify the interface has exactly one abstract method.                                            | `Lambdas.md`                               |

### Error model

| Annotation              | Intended target | Intended effect                                                                                  | Spec                |
|-------------------------|-----------------|--------------------------------------------------------------------------------------------------|---------------------|
| `@Throws(IOException.class, ...)` | method | Advisory declaration of exceptions a method may throw. Used by lint to surface uncaught throws at call sites. | `ErrorModel.md`     |

### Aspects (advanced) — core language

Aspect weaving is core (see [`AspectModel.md`](../lang/AspectModel.md) §
Aspect weaving). These are the advanced extensions:

| Annotation              | Intended target | Intended effect                                                                                  | Spec                       |
|-------------------------|-----------------|--------------------------------------------------------------------------------------------------|----------------------------|
| `@NoAdvice`             | method          | Opts the method out of aspect matching. Hot-path escape hatch.                                   | [`AspectModel.md`](../lang/AspectModel.md) |
| `@DeclareParents`       | aspect method   | AspectJ-style introduction of a new interface implementation onto a target class. (Rejected in v1 — use inheritance.) | [`AspectModel.md`](../lang/AspectModel.md) |

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
| `@JsonStrict`           | class           | Reject unknown keys during `Json.parse<T>`. Default policy silently skips them. Per-field `@JsonProperty` / `@JsonIgnore` / `@JsonRequired` / `@JsonAlias` / `@JsonInclude` / `@JsonNamingStrategy` are also part of the JSON annotation surface. | `docs/specification/codec/json/Json.md` § Tier 1 |
| `@NoVTable`             | class           | Suppress vtable header for classes that don't need virtual dispatch. Smaller instances.          | (no doc yet)        |
| `@Retained`             | class           | Keeps a class in the reflection registry even when no static path reaches it (AOT-stripping opt-out). Recognized as `Modifiers.isRetained()` today; full `forName` registry retention is in progress. | `Reflection.md`  |
| `@Trainable`            | field           | ML training-loop marker (parameter slot for gradient updates).                                   | `CajetaMath.md`       |
| `@Transactional`        | method          | Aspect marker for transactional methods (user-defined, but reserved name).                       | `AspectModel.md`    |
| `@DisplayAs("name")`    | method, field   | Override the display name in IDE / debugger views.                                               | `Debugging.md`      |
| `@Parameter`            | parameter       | Reflection hint — retains the parameter name in the symbol table for introspection.              | `Reflection.md`  |

### Rejected

- **`@Borrow`** on a field was proposed during the auto-drop work to
  mark fields that hold borrows. It was rejected as "a hack and an
  ugly one", because runtime self-discrimination (`FieldOwnership.md`
  § Solution B) makes the annotation unnecessary.

---

## Section 5 — User-defined marker annotations

You declare your own annotations via the standard `annotation`
syntax (`annotationTypeDeclaration`, `antlr4/CajetaParser.g4:471`):

```cajeta
package com.example;

public annotation Audited {
    String reason() default "";
}
```

These are first-class citizens of the annotation system, so
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
reference, and the framework matches any method annotated `@Audited`.

User-defined annotation methods may take parameters, following the
Java convention, like `reason()` above. Aspect advice reads them via
`JoinPoint.getAnnotation(Audited.class).reason()`.

User annotations not consumed by an `@Aspect` are inert, with no
code generated and no runtime overhead. They become useful via
either an aspect pointcut or via reflection (`Reflection.md`).

---

## Implementation notes

### Annotation parsing

The grammar is at `antlr4/CajetaParser.g4`:
- `annotation` is `'@' qualifiedName ('(' elementValuePairs? ')')?`
- `elementValuePairs` is `key = value, key = value, ...`
- `elementValue` is a literal, an array, or another annotation

The result is parsed into `AnnotationInstance`
(`src/cajeta/type/Annotatable.h`), which holds the name, the
keyword-argument map, and the source position.
`Annotatable::findAnnotation(shortName)` is the canonical lookup.

### Argument-value capture (implemented)

`AnnotationParser.cpp` captures argument values for all annotations
uniformly into the `AnnotationInstance.args` list. That covers string
literals, integers, booleans, class literals (`Foo.class` → a `ClassRef`),
arrays (`StringList` / `Int64List` / `BoolList`), and nested annotations.
Typed lookups (`getString`, `getInt`, `getBool`, `getClassRef`) read them
back. This is what powers `@Order(2)`, `@Component(name = "primary")`,
`@Inject(name = "primary", scope = "Owner")`, the
Lombok-mirror configuration, and `@Encoding(MsgPackEncoder.class)`'s
class-literal argument. Earlier drafts of this doc claimed only
`@SuppressLint` / `@Native` parsed their args, and that is no longer true.

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

Per the Lombok-mirror surface, every annotation-synthesized method
- carries source-position metadata pointing at the annotation
  site, so debugger / stack traces show the originating annotation
  rather than `<synthesized>`,
- is overridable by the user declaring a matching signature, in which
  case the user's wins and the synthesizer skips synthesis, the same
  convention `@AutoHash` already follows in `synthesizeAutoHash`,
  `CajetaClass.cpp:1204`,
- emits a soft-deprecation lint if the user's hand-written
  version exists with a different signature, which signals probable
  intent drift,
- respects the `--source-tags` debug feature (`CompilerModes.md`)
  by passing source position to the chain-push helpers like any
  user-written method.

### Lombok-mirror status

All of the following are implemented, with synthesizers in `CajetaClass.cpp`
(see § Section 2): annotation argument capture, `@Getter` / `@Setter`,
`@ToString`, `@NoArgsConstructor` / `@AllArgsConstructor` /
`@RequiredArgsConstructor`, the `@Data` / `@Value` bundles, `@NonNull`,
`@Builder`, `@With`, and `@Encoding`.

Still outstanding:
- **`@Cleanup`**, the `try`/`finally` synthesis, is not started.
- **`@EqualsAndHashCode` as a standalone annotation.** Today hashing is
  driven only by `@AutoHash` (and the `@Data` / `@Value` bundles).
- **`@ToString(format = TO_STRING_JSON)`** is deferred until the
  `cajeta.codec.json` writer ships. Requesting it is a compile error today.
