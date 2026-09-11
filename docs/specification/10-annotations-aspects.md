# 10 — Annotations & Aspects

This chapter defines Cajeta's declared metadata and what acts on it. Annotations are a compile-time-only metadata channel: the parser attaches them to the element below, compiler stages consult them, and nothing survives to run time except the code they cause to be generated. The consumers are the compiler's own annotation families — synthesis, dependency injection, wire formats, FFI, lint — and user-written aspects.

## 10.1 Annotation Declarations

A user annotation is declared with the `annotation` keyword. Its methods are its elements, and an element may declare a default.

```cajeta
public annotation Audited {
    String reason() default "";
}
```

An applied user annotation is inert until something consumes it — an aspect pointcut (§10.4) or reflection. No code is generated for an unconsumed annotation and it has no runtime overhead.

## 10.2 Applying Annotations

An annotation applies to the declaration that follows it: `@Name`, `@Name(value)`, `@Name(key = value, ...)`, with array values in braces. A single positional argument may omit its name — `@Order(2)` is `@Order(value = 2)`. Argument values of every kind — strings, integers, booleans, class literals (`Foo.class`), arrays, nested annotations — are captured uniformly.

**Example 10.2-1.** A synthesis annotation and a user annotation together.

```cajeta
import cajeta.synth.Data;
public annotation Audited { String reason() default ""; }

@Data
public class Point {
    public int32 x;
    public int32 y;
    public Point(int32 x, int32 y) { this.x = x; this.y = y; }
    @Audited(reason = "test")
    public int32 sum() { return this.x + this.y; }
}
Point p = heap Point(3, 4);
System.stdout.println(p.toString() + " " + p.sum());    // Point(x=3,y=4) 7
```

## 10.3 Annotations the Compiler Consumes

The framework annotations live in `cajeta.*` packages, grouped by what they drive:

- **Synthesis** (`cajeta.synth`) — `@Getter`, `@Setter`, `@ToString`, `@EqualsAndHashCode`, `@Data`, `@Value`, `@Builder`, `@NoArgsConstructor`, `@AllArgsConstructor`, `@RequiredArgsConstructor`, `@With`, `@AutoHash`. The compiler synthesizes the corresponding members, and a user-written member of the same signature wins over the synthesized one.
- **Dependency injection and aspects** (`cajeta.aot`) — §10.4 and §10.5.
- **Wire formats** (`cajeta.wire`) — `@BigEndian`, `@LittleEndian`, `@HostEndian`, `@Align` on `view` classes (Arrays, Views, Slices & Records §12).
- **FFI** (`cajeta.ffi`) — `@Native(value = "symbol")`: the method body is a forwarding call to the named native symbol.
- **Lint** (`cajeta.lint`) — `@SuppressLint("rule-id", …)` silences the listed rules in its scope. There is no catch-all.
- **Verification** (`cajeta.lang`) — `@Override` and its kin: checked by the compiler, no code emitted.
- **Accelerated compute** — `@Kernel`, `@Device`, `@Backend`, `@Wave`, and the rest of the XPU family are specified in Accelerated Compute §17.

Annotation names are stable API: renaming one is a breaking change.

## 10.4 Aspects

An aspect is a class marked `@Aspect` whose methods are advice over a **pointcut** — a predicate selecting methods. Two pointcut forms are defined: *marker-annotation* (every method tagged with a given user annotation) and *type-based* (every method of a class or of an interface's implementers).

The advice kinds:

- `@Before` — runs before the matched body.
- `@After` — runs on every exit path, return or throw.
- `@AfterReturning` — runs on normal return, receiving the return value.
- `@AfterThrowing` — runs on a throw, receiving the throwable.
- `@Around` — wraps the call. It receives the original body as a proceed function (`@Original`) and chooses if and when to invoke it.

`@Order(n)` sequences multiple aspects matching the same method, and `@NoAdvice` opts a method out of weaving. An `@Aspect` class is itself a `@Component`, so advice can `@Inject` collaborators.

**Weaving guarantees.** Weaving happens at compile time — no runtime proxies and no load-time instrumentation. For `@Around`, the original body is extracted into a private helper and the public method *becomes* the woven wrapper. There is no self-invocation bypass — `this.method()` inside the class reaches the advice like any external call. Overrides inherit advice through virtual dispatch.

> *Discussion.* Two pieces are specified but not yet lowered: the typed `JoinPoint` advice parameter and matched-annotation capture (advice receiving the `@Audited` instance's element values). Advice runs today without that typed context. String-expression pointcuts (`execution(...)`) are deferred.

## 10.5 Dependency Injection

The DI *substrate* is core language, and only policy above it is framework. The compiler owns a compile-time graph:

- `@Component` declares an injectable node (optional `name=` qualifier).
- `@Inject` marks an injection site — a field, resolved at construction, or a parameter, resolved at invocation. `allocate=` selects the identity scope of the injected instance (`ALLOCATE_SINGLETON`, `OWNER_SCOPE`, `TRANSIENT`). `@Inject(name = "…")` disambiguates among named providers.
- `@Factory` declares producer methods for types the graph cannot construct directly — third-party types, assisted arguments, initialization beyond a constructor. A producer returns a fresh owned `#T`.
- `@PostConstruct` and `@PreDestroy` are lifecycle hooks: after injection, and on drop.

Resolution and bootstrap generation happen at compile time. An unresolvable or ambiguous site is a compile-time error. The opinion layer — stereotypes, deployment profiles, request/session scopes, the web model — is the `primavera` framework's, outside this specification.
