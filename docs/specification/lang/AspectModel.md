# Aspect-Oriented Programming + Dependency Injection — core language

> **The DI substrate and aspect weaving are CORE language, in `package
> cajeta.aot`.** `@Component`, `@Inject`, `@Factory`, the compile-time DI graph,
> the identity-based scopes, lifecycle hooks, and aspect weaving are mechanism
> the compiler owns. Only the opinion layer on top is framework policy, and
> that lives in primavera (`org.cajeta.primavera`, the enterprise framework).
> The opinion layer is request/session scope, the web model (`@RestServer`),
> stereotypes, and deployment profiles.

This reverses the earlier "the DI container moved to cazo" extraction for the
substrate. The reason is structural. `@Inject` is the consumption end of a graph
edge and `@Component` / `@Factory` are the production end, so a core `@Inject` with
library-defined producers it cannot see or check is incoherent, and the AoT
guarantees below (compile-time graph, ownership soundness, dead-code reachability)
are all compiler-resident. So the substrate is indivisible and lives in the
core. cazo was renamed primavera, and the policy layer it owns is unchanged.

## What is core vs what is primavera

| Concern | Home |
|---|---|
| `@Component` — declare an injectable node | **core** (`cajeta.aot`) |
| `@Inject` — consume a node; `@Scope` — publish a component lifetime at the method or class that bounds it | **core** |
| `@Factory` — produce a node (third-party types, assisted args, init beyond ctor) | **core** |
| Compile-time graph resolution, generated bootstrap, ownership integration | **core** |
| `@PostConstruct` / `@PreDestroy` lifecycle | **core** |
| Aspect weaving (`@Aspect` / advice / `@Order` / `@Original`) | **core** |
| Test override seam (`@Inject` runtime override) | **core** — see [`../../DI-override-hook.md`](DI-override-hook.md) |
| `@Profile` — deployment-profile component filtering (`--profile=<name>`) | **core** (`cajeta.aot`) |
| `@TestComponent` — compile-time test-double masking (`--profile=test`) | **core** (`cajeta.aot`) |
| Request / session scope | **primavera** policy |
| Web request/response model, `@RestServer`, handler API, pluggable executor | **primavera** policy |
| Stereotypes (`@Repository`, `@Service`) | **primavera** policy |

## Goals

- **AoT compilation, period.** Aspects and DI are resolved at compile time by the
  compiler. There are no runtime proxies, no load-time weaving, and no separate
  annotation processor. The compiler reads the annotations and emits the wired/woven code.
- **Zero overhead for unaffected code.** A method with no matching aspect and no
  `@Inject` dependency compiles unchanged.
- **Type-safe, compile-checked.** The DI graph is checked at compile time.
  Missing implementations, cycles and ambiguous resolution are compile errors.
- **Composes with the memory model.** Construction is canonical and
  compiler-owned, so the single-owner borrow discipline is provable rather than
  taken on faith.

---

## Dependency-injection substrate

### `@Component` — declare a node

A class annotated `@Component` joins the compile-time DI graph. An optional
`name = "..."` qualifier disambiguates multiple implementations of one type. The
component declares no lifetime of its own. That is a per-`@Inject`-site decision
(see scopes).

```cajeta
@Component public class Database { public Database() { ... } }

@Component public class UserService {
    @Inject Database db;                 // field injection
    @Inject Logger log;
}

@Component public class ReportGenerator {
    private UserService users;
    public ReportGenerator(@Inject UserService users) { this.users #= users; }  // ctor injection
}
```

Field injection and constructor injection both compose. Setter injection is
rejected, since it adds a third, implicitly-mutable surface for no gain.

### `@Inject` and `@Scope` — lifetimes belong to the component

`@Inject` marks an injection site, and `name = ...` qualifies it. The lifetime
belongs to the component rather than the site (`specs/component-scopes-spec.md`).
A component names a scope with `@Component(scope = "...")`, default `"Singleton"`,
and every site that injects it gets that lifetime. A site may repeat the declared
scope but not change it. For a component that declares none, a site may still ask
for `"Owner"` (one per injecting object) or `"Transient"` (one per injection),
and the holder owns the result.

A scope other than the built-in `"Singleton"` and `"Transient"` is published
by the code that bounds it, with `@Scope("Name")`:

| Placed on | The scope is | It ends |
|---|---|---|
| a method, or a method it overrides or implements | one activation | on return or throw |
| a class | one instance, current while a non-private instance method runs | when the instance is dropped |

The compiler emits both ends: an anchored method pushes a frame onto the drop
chain on entry, so every return and every throw ends it, and an anchor class
ends its component table in its drop. No framework code starts or stops a scope.
A field may hold only a component that outlives its holder, and otherwise the
site injects a `Scoped<T>` handle that resolves at each `get()`. Values made at
run time enter a scope through `Components.provide(#v)`.

Request, session and connection lifetimes are therefore not core concepts.
primavera publishes them on its own pipeline and session types.

### `@Factory` — produce a node

A `@Factory` class holds one method per provided type. It covers the three cases
`@Component` construction cannot: third-party / unowned types (the body calls
a constructor you cannot annotate), assisted arguments (caller-supplied rather
than graph-resolved), and initialization beyond the constructor. Four rules keep
it sound:

- **R1. Resolution keys on the method signature, never the body.** The return
  type is the provided type, and `@Inject`-marked params are dependency edges.
  The graph stays fully analyzable though the body is arbitrary.
- **R2. Fresh-owned return, and the framework owns caching.** The method returns a
  freshly-`heap`-allocated owned `#T` (ordinary owned-return discipline). The body
  must not self-cache. The generated accessor memoizes per the declared scope and
  owns the drop.
- **R3. Injected vs assisted parameters.** `@Inject`-marked params are
  graph-resolved, and unmarked params are assisted (caller-supplied). Any assisted
  param ⇒ consumers inject the factory and call it. All-injected ⇒ it is a
  provider and consumers inject the product.
- **R4. Scope on the method** (`@Singleton` / `@Transient`, or
  `@Component(scope = "Name")` for a published scope), in one legible place. A
  product in a published scope is built and dropped like a scoped component
  (`specs/component-scopes-spec.md` §11).

```cajeta
@Factory class ConnectionFactory {
    @Inject Pool pool;                                  // factory collaborator
    @Singleton Connection make(@Inject Clock clk, String tenantId) {
        Connection c = heap Connection(pool, clk, tenantId);  // injected + assisted
        c.open();                                       // init beyond ctor
        return c;                                       // fresh, owned
    }
}
```

`@Factory` coexists with `@Inject` constructors. The terse `@Inject` path
stays the default for fully graph-resolvable, owned classes, and a factory is
required only for the three cases above. A type provided by both a `@Component`
ctor and a `@Factory` method is an ambiguity compile error. `@Bean`-style
free-floating producers and construction-via-cast are rejected. See primavera
`docs/Factory.md` for the full rationale.

### Graph resolution

The compiler scans all `@Component`/`@Factory` sources, builds the graph, and
errors on:

- **Missing implementation.** `Foo needs Bar, but no @Component/@Factory provides Bar`.
- **Circular dependency.** `Cycle: Foo -> Bar -> Foo`.
- **Ambiguous resolution.** Multiple unqualified providers of one type for a
  name-less `@Inject`. The unqualified (name-less) provider is the implicit
  default (`@Primary`-equivalent). Name the alternatives and qualify the site.

### Generated bootstrap

For each provided type the compiler synthesizes `get_X()` (lazy process
singleton) and `make_X()` (fresh, caller-owns). `@Inject` reads route to one or
the other by the site's `allocate` mode. Field assignments run in the
compiler-synthesized `__postConstruct`, and constructor-param injection threads
the resolved instance at the receiver's construction site. There is no reflection
and no runtime container, only direct calls.

### Lifecycle

- **`@PostConstruct`** runs after all `@Inject` fields are populated, before the
  instance is exposed.
- **`@PreDestroy`** runs on drop (at process exit for singletons, and on the
  receiver's drop chain otherwise).

### Test override seam

`@Inject` resolution is statically a direct call. A test-build-only runtime
seam (`--profile=test`) lets a harness substitute a mock per type, keyed on the
type's `reflect.Class` pointer identity, with production builds emitting the
unchanged zero-cost path. This seam is core, so any framework or test harness can
use it. The full design is in [`../../DI-override-hook.md`](DI-override-hook.md).

### Profiles and test-component masking

`@Profile("name", ...)` scopes a `@Component` to one or more deployment profiles
(any-of). The resolver includes it only when `--profile=<name>` matches one of them.
A component with no `@Profile` is profile-neutral and always included.

`@TestComponent` declares a test double. Under `--profile=test` it is included and
masks every non-test `@Component` that implements an interface the double also
implements. The double takes over that interface's `@Inject` sites. Masking is
interface-scoped. A double with no interface masks nothing (it is injectable only by
its own concrete type), and a `@Component` injected by concrete type is not masked.
Outside test mode, `@TestComponent`s are dropped. Both annotations are declared in
`cajeta.aot`, and the compiler recognizes them by short name. A worked end-to-end
example is `samples/profile-di/` (one source built under `--profile=prod`/`dev`/`test`).

---

## Aspect weaving (core)

### Pointcuts

A pointcut is a predicate over methods. Two forms ship, marker-annotation (methods
tagged with a user annotation) and type-based (every method on a class / interface
implementer). String-expression pointcuts (`execution(...)`) are deferred.

### Advice

- `@Before` runs before the matched body.
- `@After` runs on every exit path (return or throw).
- `@Around` wraps the call. It receives a typed `@Original` proceed function and
  chooses if/when to invoke it.
- `@AfterReturning` runs on normal return and receives the return value.
- `@AfterThrowing` runs on throw and receives the `Throwable`.

`@Order(n)` chains multiple matching aspects, and `@NoAdvice` opts a method out.
Advice lives in an `@Aspect` class, which is itself a `@Component`, so it can
`@Inject` collaborators.

```cajeta
public @interface Audited { String reason() default ""; }

@Component @Aspect public class AuditAspect {
    @Inject AuditLog log;
    @Around(Audited.class)
    int32 timed(@Original Function<int32> proceed, JoinPoint jp) {
        int64 t = nanos();
        try { return proceed(); } finally { log.metric(jp.methodName, nanos() - t); }
    }
}
```

### Codegen

`@Around` extracts the original body into a private helper and replaces the public
method with a wrapper that calls the advice, passing the helper as `proceed`. The
wrapper is the method, so `this.method()` is the woven version, with no proxy
and no self-invocation bypass. Overrides inherit advice via the vtable, and
`@NoAdvice` opts out.

> **Not yet lowered:** the typed `JoinPoint<R, A...>` parameter and the
> matched-annotation capture (`Audited annot` advice parameter). Advice runs today
> but cannot yet receive that typed context (`Method.cpp:445`). This is the single
> largest open AOP piece.

---

## What primavera adds (policy)

The enterprise opinion built on this substrate lives in `org.cajeta.primavera`:

- **Request / session scope.** `@Scope("Request")` and `@Scope("Session")`
  published on primavera's pipeline and session types, over the core scope
  mechanism.
- **Web model.** HTTP request/response, a handler API, `@RestServer`, and a
  pluggable executor (fiber-per-request vs threadpool-over-completion-ports).
- **Stereotypes.** `@Repository`, `@Service` (named roles over `@Component`).
- **Deployment profiles.** `@Profile("prod"|"test"|...)` selection.
- **Test harness.** `@TestComponent` overrides and request-scope seeding, over
  the core override seam, on cajeta-unit.

A project that dislikes primavera's opinion keeps the core substrate and builds
its own framework on the same `@Component` / `@Inject` / `@Factory` graph. That
is exactly why the substrate must be core. Every library speaks one DI vocabulary,
with no framework dependency, instead of fragmenting like Java's
`@Inject`/`@Autowired`/`@Resource`.

---

## Interim status

The compiler implements all of the above today (`CajetaModule.cpp`,
`CajetaLlvmVisitor.h`, `ComponentInjectMethod.cpp`, `FactoryProviderMethod.cpp`).
That covers graph build, the implemented allocation modes, lifecycle hooks, aspect
weaving, and the full `@Factory` substrate. The `@Factory` substrate is discovery
(R1/R3/R4), graph resolution (provider vs factory-injection, the
`@Component`/`@Factory` ambiguity, factory-param cycles), provider-accessor codegen
(`@Singleton` memo / `@Transient` fresh, R2 `#T` fresh-owned), and
assisted-injection call-site threading (the consumer passes only the assisted
args). The long-term annotation-processing / codegen extension point goal applies
to the policy layer, so primavera or any framework plugs its stereotypes, scopes,
and web model in without core changes. It does not apply to the DI substrate,
which stays core.

> Known v1 gap: a `@Transient` factory product stored in a singleton owner's field
> is not yet drop-tracked per holder, so it leaks at owner destruction. This is
> consistent with the existing transient/owner-scope handling. Revisit it with
> owning-collections / lean-linker drop synthesis.
