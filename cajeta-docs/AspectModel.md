# Cajeta Aspect-Oriented Programming + Dependency Injection — Specification v1

## Goals

- **AoT compilation, period.** Aspects and dependency injection are resolved at compile time by the Cajeta compiler itself. No runtime proxies, no IR weaving at deploy time, no annotation processors as a separate tool. The compiler reads the annotations and emits the wired/woven code directly into the binary.
- **Zero runtime overhead for unaffected methods.** A method with no matching aspect and no `@Inject` dependencies compiles unchanged. Aspect resolution is a per-method check at codegen; if nothing matches, no extra IR.
- **Type-safe advice and injection.** Advice receives a typed `JoinPoint<...>` parameterized by the matched method's signature. `@Around` proceed is a typed function pointer, not reflection. The DI graph is checked at compile time — missing implementations, cycles, ambiguous resolution are compile errors.
- **Compose with the rest of the language.** Drop chain unwinds advice locals on every exit path. Fibers + scope + try/catch all work uniformly because the wrapper is just an ordinary Cajeta method.

## Rejected (v1)

- **String-pattern pointcut expressions** (`execution(* com.example..*.save(..))`). Powerful but stringly-typed, slow to match, and divorces the pointcut from the targeted code. Deferred until annotation-based + type-based pointcuts prove insufficient — probably never.
- **Runtime proxies / load-time weaving.** Architectural rejection. Aspects are a compile-time concern.
- **Aspect-on-aspect.** Advising the advice. Spring forbids it; we follow.
- **`@DeclareParents`-style introductions** (adding new interfaces to existing classes via aspects). Use ordinary inheritance.
- **Prototype / request / session scopes.** Singleton is the default and only scope in v1. Other scopes add later if needed.
- **Constructor / setter injection.** Spring's docs prefer constructor injection so tests can `new SomeService(mockA, mockB)` directly — Spring's `ApplicationContext` is heavy enough at runtime that side-stepping it is the test idiom. Cajeta has no such context: DI is compile-time, no runtime registry, no lazy context to load. Tests substitute by recompiling with `@TestComponent` (or a `@Profile("test")` wiring), not by hand-constructing instances. Field injection is therefore the only injection vector; `@Inject` on a constructor parameter is a compile error.
- **Reflection / runtime registry.** The DI "container" is generated code with direct calls.

---

## Package

All framework-provided annotations and types in this document live in `package cajeta.aot;` — `@Aspect`, `@Before`, `@After`, `@Around`, `@AfterReturning`, `@AfterThrowing`, `@Order`, `@Original`, `@NoAdvice`, `@Component`, `@Repository`, `@Inject`, `@Profile`, `@TestComponent`, `@PostConstruct`, `@PreDestroy`, and the `JoinPoint` class. Same convention `cajeta.error` uses for the exception hierarchy — keeps the AoP/DI surface discoverable as a group and encourages reuse over re-rolling. Code samples below omit the package qualifier for brevity. User-defined marker annotations (`@Audited`, `@Logged`, `@Transactional`, ...) live wherever the user declares them — they're inputs to the framework, not part of it.

---

## Core concepts

### Pointcut

A predicate over methods: "does this method match?" Pointcuts come in three forms; v1 supports the first two:

1. **Marker-annotation pointcut.** Targets methods annotated with a specific user-defined annotation. The most common shape.
2. **Type-based pointcut.** Targets every method on a class or implementer-of-interface.
3. **Expression pointcut** (v2+). String-pattern matching à la Spring's `execution()`. Deferred.

### Advice

Code that runs around a matched method. Five forms:

- `@Before` — runs before the matched method body.
- `@After` — runs on every exit path (normal return or thrown exception). Same shape as a finally arm.
- `@Around` — wraps the call. Receives a typed function reference to the original; chooses if/when to invoke it.
- `@AfterReturning` — runs only on normal return. Receives the return value.
- `@AfterThrowing` — runs only on throw. Receives the thrown `Throwable`.

### JoinPoint

Carries context to the advice: which method was matched, its arguments, the matched annotation instance. Typed via templates.

### Aspect

A class annotated `@Aspect`. Holds one or more advice methods plus any state. Registered as a `@Component` so it can itself receive `@Inject` dependencies (an aspect can inject a logger or metrics collector).

---

## Annotation surface

```cajeta
// User-defined marker — applied to methods to opt them into advice.
public @interface Audited {
    String reason = "";   // optional annotation parameter
}

// User-defined aspect.
@Aspect public class AuditAspect {

    @Inject AuditLog log;

    // `@Before(Audited.class)` — match every method annotated @Audited.
    @Before(Audited.class)
    void onCall(JoinPoint jp, Audited annot) {
        log.write(jp.methodName + " — reason: " + annot.reason);
    }

    @AfterThrowing(Audited.class)
    void onFail(JoinPoint jp, Throwable t) {
        log.error(jp.methodName + " failed: " + t.message);
    }

    @Around(Audited.class)
    int timedCall(@Original Function<int> proceed, JoinPoint jp) {
        long start = nanos();
        try {
            return proceed();
        } finally {
            log.metric(jp.methodName, nanos() - start);
        }
    }
}

// Method opts in by tagging itself with the marker.
class UserService {
    @Audited(reason = "data deletion")
    public void deleteUser(int id) { ... }
}
```

### Where annotations apply

- `@Aspect` on a class. Marks it as an aspect class.
- `@Before`/`@After`/`@Around`/`@AfterReturning`/`@AfterThrowing` on advice methods inside an `@Aspect` class. Each takes a pointcut argument.
- `@Order(n)` on advice methods to control execution order when multiple aspects match the same target.
- `@Original` on a function-typed parameter of an `@Around` advice to bind it to the original method.

### Pointcut parameter shapes

```cajeta
@Before(Audited.class)            // marker-annotation pointcut
@Before(Repository.class)         // type-based: every method on a Repository (or impl)
@Before(Repository.class, "save") // type-based, scoped to method-name (v1.1)
```

---

## Typed JoinPoint

The `JoinPoint<R, A...>` template carries the matched method's signature.

```cajeta
class JoinPoint<R, A...> {
    public String methodName;
    public String className;
    public A... args;   // template-arg pack, accessible via .arg0, .arg1, ...
}
```

The compiler enforces that the advice's declared JoinPoint type-parameters match (or are compatible with) the matched method's signature:

```cajeta
@Before(Audited.class)
void log(JoinPoint<void, int, String> jp) {   // matches void m(int, String)
    log.write("called " + jp.methodName + " with " + jp.arg0 + ", " + jp.arg1);
}
```

If a method tagged `@Audited` has a different signature, the compiler rejects the aspect-target pair. Use `JoinPoint<*, *>` (wildcards) for advice that should match any signature; the `args` are accessible only as a runtime array (`jp.argsAsArray`) at that point — the tradeoff for being polymorphic.

### Catching the matched annotation instance

If the advice declares a parameter typed as the matched annotation, the compiler binds it to the annotation instance:

```cajeta
@Before(Audited.class)
void log(JoinPoint jp, Audited annot) {
    log.write("reason: " + annot.reason);   // typed access to annotation params
}
```

The compiler synthesizes the annotation-instance construction at the call site — `Audited`'s element values become a struct accessible to the advice. Zero reflection.

---

## @Around and the proceed pattern

`@Around` advice wraps the call. The original method is bound to a typed function parameter marked `@Original`:

```cajeta
@Around(Audited.class)
int around(@Original Function<int, int> proceed, JoinPoint jp, int x) {
    if (!authorized()) throw new AuthException("denied");
    return proceed(x);   // typed call to the original
}
```

`Function<int, int>` is the signature of the original method (returns int, takes int). The compiler binds `proceed` to a synthesized wrapper that invokes the original method's extracted body.

### Codegen for @Around

The compiler:
1. Extracts the original method body into a private helper (`compute__original`).
2. Generates a replacement public method (`compute`) whose body calls the @Around advice, passing `compute__original` as the proceed function.
3. The advice's body is inlined (or called) inside the replacement.

Result IR for `int compute(int x)` advised by `@Around timed`:

```
int compute__original(int x) { <original body> }

int compute(int x) {
    return AuditAspect_instance.timedCall(compute__original, joinPoint, x);
}
```

The aspect instance is the singleton resolved via DI (see below). The joinPoint is a stack-allocated struct populated with the method's metadata.

---

## Multiple aspects on one method

When multiple aspects match the same method, the compiler chains them in `@Order` order:

```
@Order(1) @Around(Audited.class) void aspect1(...) { ... }
@Order(2) @Around(Audited.class) void aspect2(...) { ... }
```

Codegen produces nested wrappers — `aspect1` wraps `aspect2`, which wraps the original. `@Before`/`@After` flatten into a single wrapper with multiple before/after calls.

Without `@Order`, source-declaration order in the aspect class is used. Across aspects without `@Order`, the order is implementation-defined (advice should not depend on it).

---

## Compile-time dependency injection

### Component registration

A class annotated `@Component` becomes part of the compile-time DI graph. The annotation takes two optional parameters:

- `name = "..."` — a qualifier for disambiguation when multiple components implement the same interface. Default: no qualifier (the component matches unqualified injections of its declared types).
- `allocate = ALLOCATE_SINGLETON | ALLOCATE_CONTAINER_SCOPE | ALLOCATE_CALL_SCOPE | ALLOCATE_TRANSIENT` — the instantiation policy. Default: `ALLOCATE_SINGLETON`. The four modes form a hierarchy of decreasing lifetime:

  | Mode | Lifetime | One instance per |
  |------|----------|------------------|
  | `ALLOCATE_SINGLETON` | program | process |
  | `ALLOCATE_CONTAINER_SCOPE` | fiber | spawned task (or main, treated as one container) |
  | `ALLOCATE_CALL_SCOPE` | call | method activation |
  | `ALLOCATE_TRANSIENT` | injection | injection site |

  - **`ALLOCATE_SINGLETON`** — one shared instance per process, lazily constructed on first inject, lives until program exit. `@PreDestroy` fires at exit.
  - **`ALLOCATE_CONTAINER_SCOPE`** — one instance per fiber (or per main thread, when not in a fiber). Created on first inject within the fiber, shared by every other inject in that fiber, dropped when the fiber completes. Stored in a per-fiber cache on the `cajeta_fiber` struct. The natural fit for "this should be one-per-task" patterns: a `RequestContext`, a per-task tracing span, a connection-per-task lease.
  - **`ALLOCATE_CALL_SCOPE`** — one instance per call activation, shared across the entire transitive call graph of one method invocation. Created on first inject within the call, dropped when the activation's drop chain runs (i.e., at the method's return or throw). The natural fit for "this single operation needs its own context" patterns: a transaction handle, a per-request audit trail, an in-progress message-builder. Implemented via the existing implicit function-body scope (R5-A').
  - **`ALLOCATE_TRANSIENT`** — a fresh instance at every injection site. Owned by the receiver and dropped via the normal drop chain when the receiver goes out of scope. No caching at all.

```cajeta
@Component public class Database {
    public Database() { ... }
}

@Component public class UserService {
    @Inject Database db;
    @Inject Logger log;
}

@Component public class ReportGenerator {
    @Inject UserService users;
    @Inject Database db;
}

// Multiple impls of an interface — disambiguate by name.
@Component(name = "disk") public class DiskPersister implements Persister { ... }
@Component(name = "memory") public class MemoryPersister implements Persister { ... }

// Transient: every @Inject yields a fresh instance.
@Component(allocate = ALLOCATE_TRANSIENT)
public class RequestContext { ... }
```

`@Repository` is a sibling annotation. Same parameters, same DI behavior — but the name signals "writes to / reads from a system of record" (the data-access layer). Cajeta doesn't add Spring's persistence-exception-translation behavior at this layer; the differentiation is documentary plus future hook (e.g., later we can attach transaction defaults or repository-specific lint to `@Repository` without touching `@Component`).

```cajeta
@Repository public class UserRepository {
    @Inject Database db;
    public User findById(int64 id) throws SqlException { ... }
}
```

### Compile-time graph resolution

The compiler scans all source for `@Component` classes, builds a dependency graph, and:

- **Missing implementation** → compile error. `Foo needs Bar, but no @Component class implements Bar`.
- **Circular dependency** → compile error. `Cycle: Foo -> Bar -> Foo`.
- **Ambiguous resolution** (two `@Component` classes implementing the same interface, none of them name-less, and the consumer's `@Inject` lacks a name) → compile error. `Two implementations of Persister: DiskPersister(name="disk"), MemoryPersister(name="memory"). Add @Inject(name = "...") at the consumer.`

### Generated bootstrap

For `ALLOCATE_SINGLETON` components, the compiler synthesizes module-level lazy singletons:

```cajeta
// Compiler-generated for @Component class Database:
static Database __singleton_Database = null;
static Database get_Database() {
    if (__singleton_Database == null) {
        __singleton_Database = new Database();
    }
    return __singleton_Database;
}

static UserService __singleton_UserService = null;
static UserService get_UserService() {
    if (__singleton_UserService == null) {
        UserService u = new UserService();
        u.db = get_Database();
        u.log = get_Logger();
        u.__postConstruct();   // synthesized; calls @PostConstruct if present
        __singleton_UserService = u;
    }
    return __singleton_UserService;
}
```

For `ALLOCATE_TRANSIENT` components, no singleton storage. Each injection site emits a fresh constructor call:

```cajeta
// Compiler-generated for @Component(allocate = ALLOCATE_TRANSIENT) class RequestContext:
static RequestContext make_RequestContext() {
    RequestContext r = new RequestContext();
    r.__postConstruct();
    return r;          // caller owns it; drops at the receiver's scope end
}
```

`@Inject` field reads become `get_X()` (singleton) or `make_X()` (transient) calls. The receiver's compiler-synthesized `__postConstruct` runs the assignments after the receiver itself is allocated, before any user-defined `@PostConstruct` method. Constructor parameters and setter methods are not valid injection sites — `@Inject` is field-only.

### Qualifying an injection

When more than one `@Component` of the same type or interface is registered, the consumer disambiguates with `@Inject(name = "...")`:

```cajeta
@Component public class Service {
    @Inject(name = "disk") Persister p;     // selects DiskPersister
    @Inject(name = "memory") Persister mp;  // selects MemoryPersister
}
```

The match is by component name. Compile error if no `@Component` with the requested name is registered, or if multiple components match a name-less `@Inject` and none of the candidates is unqualified.

### Selecting a context (`@Profile`)

`@Component` and `@Repository` can carry one or more `@Profile("name")` annotations. A profile is a label for a deployment context — `"prod"`, `"staging"`, `"ci"`, `"test"`, or any name the developer chooses.

- **One active profile per compilation.** Set via the compiler CLI flag `--profile=<name>`; defaults to `"prod"`. Tests compiled through the test driver default to `"test"`.
- **Profile filtering.** During graph resolution, a `@Component` annotated with one or more profiles is included only if at least one matches the active profile. A `@Component` with no `@Profile` annotation is profile-neutral and always included.
- **Resolution rules carry over.** The `name = "..."` qualifier and ambiguity-error rules from the previous section apply across the profile-filtered set. A component filtered out for the active profile is invisible to name resolution — exactly as if it weren't declared.
- **Missing implementation.** If filtering leaves no candidate for a required type, the compiler emits the standard "needs X, but no @Component implements X" error and names the active profile.

```cajeta
@Component @Profile("prod")    public class PostgresDatabase implements Database { ... }
@Component @Profile("staging") public class StagingDatabase  implements Database { ... }
@Component @Profile("test")    public class InMemoryDatabase implements Database { ... }
```

A component shared across contexts carries multiple profile annotations:

```cajeta
@Component @Profile("prod") @Profile("staging") public class S3Storage     implements Storage { ... }
@Component @Profile("test")                     public class TempDirStorage implements Storage { ... }
```

Profile names are arbitrary strings; there's no fixed enumeration. The convention is `"prod"` as the default and `"test"` as the test driver's default, but a project that needs `"ci"` or `"e2e"` or `"localdev"` simply uses those labels.

### Test-target overrides (`@TestComponent`)

The profile mechanism covers multi-environment wiring, but most test code only needs one targeted swap: "for these tests, replace this one component with a fake; leave the rest alone." `@TestComponent` is the per-class shorthand for that case.

```cajeta
@TestComponent public class FakeClock implements Clock { ... }
```

Rules:

- **Test-only.** `@TestComponent` classes are visible to test compilations only. Non-test builds ignore them entirely — no compiled code, no entry in the dependency graph.
- **Overrides by type.** A `@TestComponent` replaces any profile-neutral or profile-matched `@Component` of the same declared type at test compile time. The overridden component is invisible to the test graph; only the `@TestComponent` remains.
- **Name qualifier still applies.** Multiple `@TestComponent`s of the same declared type still need disambiguation via `name = "..."` at the consumer site, on the same rules as regular `@Component`s.
- **Doesn't combine with `@Profile`.** `@TestComponent` is implicitly test-scoped; carrying both is a compile error (use one or the other). A test build that needs a richer wiring than per-class overrides allows still uses full `@Component @Profile("test")` declarations.

`@TestComponent` is the right tool when a test wants to mock or stub a single dependency without owning the entire `"test"` profile's wiring. When a test suite needs a coherent alternate graph (an in-memory replacement of half the stack), use `@Profile("test")` on full `@Component` declarations.

### Lifecycle

- **`@PostConstruct void init()`** — called after all `@Inject` fields are populated, before the singleton is exposed (or before a transient instance is returned from `make_X`).
- **`@PreDestroy void cleanup()`** — called on the instance's drop. For singletons, fires at program exit via the runtime's at-exit chain. For transients, fires when the receiver's scope ends and the drop chain runs.

---

## Composition with the rest of the language

### Drop chain

Advice that owns local resources drops them on every exit path. The advice body is a normal Cajeta method; the drop chain wraps it naturally.

### Exceptions

`@AfterThrowing` IS the catch arm in the generated wrapper. `@After` IS the after-everything cleanup (matches the existing `__cajeta_scope_exit_to` semantics, in spirit). Cause chains and stack traces (from `ErrorModel.md`) propagate through aspects unchanged.

### Fibers + async

Async methods can be advised. The wrapper is itself async. The advice methods can themselves contain `await` — they'll suspend on the same carrier as any other async code.

### Inheritance

When a subclass overrides an advised parent method, the override **inherits the advice** by default. The compiler-generated wrapper on the parent dispatches through the vtable, which lands on the subclass's body — which is itself wrapped if the subclass's method also matches a pointcut. Avoids the Spring-AOP pitfall where calling `this.method()` skips the proxy.

Opt-out with `@NoAdvice` on the subclass override if needed (rare).

---

## Examples

### Example 1: logging via marker annotation

```cajeta
public @interface Logged { String level = "info"; }

@Aspect public class LoggingAspect {
    @Inject Logger log;

    @Before(Logged.class)
    void logCall(JoinPoint jp, Logged annot) {
        log.at(annot.level).write("→ " + jp.methodName);
    }
}

class PaymentProcessor {
    @Logged(level = "warn")
    public void chargeCard(Card card, int cents) { ... }
}
```

### Example 2: transactional boundary via @Around

```cajeta
public @interface Transactional {}

@Aspect public class TransactionalAspect {
    @Inject TransactionManager txm;

    @Around(Transactional.class)
    int execute(@Original Function<int> proceed, JoinPoint jp) {
        Transaction tx = txm.begin();
        try {
            int result = proceed();
            tx.commit();
            return result;
        } catch (Throwable t) {
            tx.rollback();
            throw t;
        }
    }
}
```

### Example 3: DI with field injection + lifecycle

```cajeta
@Component public class OrderService {
    @Inject Database db;
    @Inject MetricsClient metrics;
    @PostConstruct void init() { metrics.gauge("order_service_initialized", 1); }
}
```

### Example 4: composing AOP + DI

The aspect itself is a `@Component` and can be `@Inject`-wired:

```cajeta
@Component @Aspect public class CachingAspect {
    @Inject CacheStore cache;
    @Around(Cached.class)
    Object lookup(@Original Function<Object> proceed, JoinPoint jp) {
        Object cached = cache.get(jp.methodName);
        if (cached != null) return cached;
        Object fresh = proceed();
        cache.put(jp.methodName, fresh);
        return fresh;
    }
}
```

The compiler instantiates `CachingAspect` once at DI bootstrap (with `cache` injected), and the woven `@Around` calls dispatch on that singleton.

---

## Rationale (why not the alternatives)

- **Spring runtime proxies.** JDK/CGLIB proxies are slow to construct, opaque under debugger, and have the well-known "self-invocation bypass" footgun (`this.method()` skips the proxy). Our compile-time weaving rewrites the method body itself — `this.method()` IS the wrapped call.
- **AspectJ bytecode weaving at deploy time.** Requires a load-time agent or post-compile weaver. Slow build, awkward tooling. We do it at codegen — one pass, no separate tool.
- **Annotation processors generating proxy classes** (Micronaut, Quarkus). Better than runtime proxies — works with native-image — but still creates an extra layer (one proxy class per advised target). We skip that: the wrapper IS the public method's body. Zero proxy classes.
- **Manual decorator pattern** (write your own wrappers by hand). Verbose, doesn't centralize, doesn't compose. AOP exists because manual decoration doesn't scale.

The Cajeta-native wins:
1. **Type-safe everything.** JoinPoint is parameterized; @Around proceed is typed; annotation parameters are typed at the advice. Compile errors catch mismatched aspects.
2. **No proxies anywhere.** Methods are their wrapped selves. `this.method()` is the wrapped version.
3. **Compile-time DI graph.** Missing impls, cycles, ambiguity all caught before the binary is built.
4. **Composes with existing model.** Drop chain, fibers, exceptions, scope — the wrapper is a Cajeta method, not a synthetic proxy with its own rules.

---

## Implementation phases

This is a substantial feature surface. A reasonable rollout:

- **A1.** Annotation parsing: extend the existing `Annotatable` infrastructure to capture annotation parameter values (`@Order(2)`, `@Component(name = "disk")`, `@Inject(name = "primary")`). Today we capture annotation names plus the `@SuppressLint` string-arg special case (see `LintRules.md`).
- **A2.** `@Aspect` class registration during compile. Compiler builds a list of aspect classes.
- **A3.** Pointcut matching pass — for each method, check which aspects match. Per-method aspect list cached on the Method.
- **A4.** Codegen wrapping for `@Before` / `@After` (simplest forms). Single advice; no `@Order` chaining yet.
- **A5.** `@Around` + `@Original` typed proceed. Requires the method-extraction infrastructure (original body → private helper).
- **A6.** `@AfterReturning` + `@AfterThrowing`. Both compose with the existing try/catch codegen.
- **A7.** Multiple-aspect chaining via `@Order`.
- **A8.** `@Component` registration + DI graph build.
- **A9.** `@Inject` codegen: field reads in `__postConstruct` resolve to `get_X()` (singleton) or `make_X()` (transient) calls. Reject `@Inject` on constructor parameters and setter methods with a clear "field-only injection" error.
- **A10.** Name qualifier support: `@Component(name = "...")` registration + `@Inject(name = "...")` consumer-side selection.
- **A11.** `@PostConstruct` / `@PreDestroy` lifecycle hooks.
- **A12.** Composition tests: aspect-on-aspect-class, DI'd aspects, inheritance + advice, fiber-spanning advice.

Each phase is roughly session-sized. A1-A2 are foundation; A3-A4 deliver minimal AoP (logging-style aspects); A5+ extend.

---

## Deferred / known gaps

- **Expression-pattern pointcuts.** String-DSL `execution(...)` etc. Hold off until a use case really demands it.
- **Aspect-on-aspect / advising the advice.** Spring forbids; we follow.
- **Allocation modes beyond Singleton and Transient.** Request / session scopes that key the cache by something other than identity (e.g., HTTP request id) — add if the runtime ever grows request-context infrastructure that justifies it.
- **Optional injection.** `@Inject(optional=true)` to silently null-out unsatisfiable dependencies. Today: every `@Inject` must resolve or compile error.
- **Lazy injection.** `Lazy<T>` to defer instantiation. Possible after singleton bootstrap stabilizes.
- **Cross-module DI.** When Cajeta gains a package/module system, the DI graph spans modules. v1 assumes single-compilation-unit DI.
- **Dynamic registration / overriding for tests.** Test code may want to swap a real DB with a mock. Today: define the mock as `@Component(name = "test")` and use a test-only build flag to control which name resolves. A first-class test-overrides mechanism is post-v1.
