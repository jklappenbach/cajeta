# Aspect-Oriented Programming + Dependency Injection — core language

> **The DI *substrate* and aspect weaving are CORE language, in `package
> cajeta.aot`.** `@Component`, `@Inject`, `@Factory`, the compile-time DI graph,
> the identity-based scopes, lifecycle hooks, and aspect weaving are mechanism
> the compiler owns. Only the *opinion layer* on top — request/session scope,
> the web model (`@RestServer`), stereotypes, and deployment profiles — is
> framework **policy**, and that lives in **primavera**
> (`org.cajeta.primavera`, the enterprise framework).

This reverses the earlier "the DI container moved to cazo" extraction *for the
substrate*. The reason is structural: `@Inject` is the consumption end of a graph
edge and `@Component` / `@Factory` are the production end — a core `@Inject` with
library-defined producers it cannot see or check is incoherent, and the AoT
guarantees below (compile-time graph, ownership soundness, dead-code reachability)
are all compiler-resident. So the substrate is **indivisible** and lives in the
core. (cazo was renamed primavera; the policy layer it owns is unchanged.)

## What is core vs what is primavera

| Concern | Home |
|---|---|
| `@Component` — declare an injectable node | **core** (`cajeta.aot`) |
| `@Inject` — consume a node; identity scopes (`SINGLETON` / `OWNER_SCOPE` / `CALL_SCOPE` / `TRANSIENT`) | **core** |
| `@Factory` — produce a node (third-party types, assisted args, init beyond ctor) | **core** |
| Compile-time graph resolution, generated bootstrap, ownership integration | **core** |
| `@PostConstruct` / `@PreDestroy` lifecycle | **core** |
| Aspect weaving (`@Aspect` / advice / `@Order` / `@Original`) | **core** |
| Test override seam (`@Inject` runtime override) | **core** — see [`../../DI-override-hook.md`](../../DI-override-hook.md) |
| Request / session scope | **primavera** policy |
| Web request/response model, `@RestServer`, handler API, pluggable executor | **primavera** policy |
| Stereotypes (`@Repository`, `@Service`), deployment `@Profile`, `@TestComponent` | **primavera** policy |

## Goals

- **AoT compilation, period.** Aspects and DI are resolved at compile time by the
  compiler. No runtime proxies, no load-time weaving, no separate annotation
  processor. The compiler reads the annotations and emits the wired/woven code.
- **Zero overhead for unaffected code.** A method with no matching aspect and no
  `@Inject` dependency compiles unchanged.
- **Type-safe, compile-checked.** The DI graph is checked at compile time —
  missing implementations, cycles, ambiguous resolution are compile errors.
- **Composes with the memory model.** Construction is canonical and
  compiler-owned, so the single-owner borrow discipline is *provable*, not taken
  on faith.

---

## Dependency-injection substrate

### `@Component` — declare a node

A class annotated `@Component` joins the compile-time DI graph. Optional
`name = "..."` qualifier disambiguates multiple implementations of one type. The
component declares no lifetime of its own — that is a per-`@Inject`-site decision
(see scopes).

```cajeta
@Component public class Database { public Database() { ... } }

@Component public class UserService {
    @Inject Database db;                 // field injection
    @Inject Logger log;
}

@Component public class ReportGenerator {
    private UserService users;
    public ReportGenerator(@Inject UserService users) { this.users = users; }  // ctor injection
}
```

Field injection and constructor injection both compose; setter injection is
rejected (it adds a third, implicitly-mutable surface for no gain).

### `@Inject` — consume a node, with an identity scope

`@Inject` marks an injection site. `allocate = ...` picks the lifetime **at the
site** (default `ALLOCATE_SINGLETON`); `name = ...` qualifies. The four
identity-based scopes:

| Mode | One instance per | Status |
|---|---|---|
| `ALLOCATE_SINGLETON` | process (application) | implemented |
| `ALLOCATE_OWNER_SCOPE` | injecting object | implemented |
| `ALLOCATE_CALL_SCOPE` | method activation | **stubbed** — inject path throws `CAJETA_ERROR_NOT_IMPLEMENTED` |
| `ALLOCATE_TRANSIENT` | every read of the site | implemented |

Because the lifetime is declared at the site, one component can serve different
roles in different consumers. Non-identity scopes (request/session, keyed on the
logical request rather than an object identity) are **primavera policy** built
over `cajeta.concurrent.FiberLocal`, not core.

### `@Factory` — produce a node

A `@Factory` class holds one method per provided type. It covers the three cases
`@Component` construction cannot: **third-party / unowned types** (the body calls
a constructor you can't annotate), **assisted arguments** (caller-supplied, not
graph-resolved), and **initialization beyond the constructor**. Four rules keep
it sound:

- **R1 — resolution keys on the method *signature*, never the body.** Return type
  = provided type; `@Inject`-marked params = dependency edges. The graph stays
  fully analyzable though the body is arbitrary.
- **R2 — fresh-owned return; the framework owns caching.** The method returns a
  freshly-`heap`-allocated owned `#T` (ordinary owned-return discipline). The body
  must not self-cache; the generated accessor memoizes per the declared scope and
  owns the drop.
- **R3 — injected vs assisted parameters.** `@Inject`-marked params are
  graph-resolved; **unmarked params are assisted** (caller-supplied). Any assisted
  param ⇒ consumers inject the *factory* and call it; all-injected ⇒ it is a
  provider and consumers inject the *product*.
- **R4 — scope on the method** (`@Singleton` / `@Transient` …), in one legible
  place.

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

`@Factory` **coexists** with `@Inject` constructors — the terse `@Inject` path
stays the default for fully graph-resolvable, owned classes; a factory is required
only for the three cases above. A type provided by both a `@Component` ctor and a
`@Factory` method is an ambiguity compile error. (`@Bean`-style free-floating
producers and construction-via-cast are rejected; see primavera `docs/Factory.md`
for the full rationale.)

### Graph resolution

The compiler scans all `@Component`/`@Factory` sources, builds the graph, and
errors on:

- **Missing implementation** — `Foo needs Bar, but no @Component/@Factory provides Bar`.
- **Circular dependency** — `Cycle: Foo -> Bar -> Foo`.
- **Ambiguous resolution** — multiple unqualified providers of one type for a
  name-less `@Inject`. The **unqualified (name-less) provider is the implicit
  default** (`@Primary`-equivalent); name the alternatives and qualify the site.

### Generated bootstrap

For each provided type the compiler synthesizes `get_X()` (lazy process
singleton) and `make_X()` (fresh, caller-owns). `@Inject` reads route to one or
the other by the site's `allocate` mode; field assignments run in the
compiler-synthesized `__postConstruct`; constructor-param injection threads the
resolved instance at the receiver's construction site. No reflection, no runtime
container — direct calls.

### Lifecycle

- **`@PostConstruct`** runs after all `@Inject` fields are populated, before the
  instance is exposed.
- **`@PreDestroy`** runs on drop (at process exit for singletons; on the
  receiver's drop chain otherwise).

### Test override seam

`@Inject` resolution is statically a direct call. A **test-build-only** runtime
seam (`--profile=test`) lets a harness substitute a mock per type, keyed on the
type's `reflect.Class` pointer identity, with production builds emitting the
unchanged zero-cost path. This seam is core (any framework or test harness can use
it). Full design: [`../../DI-override-hook.md`](../../DI-override-hook.md).

---

## Aspect weaving (core)

### Pointcuts

A predicate over methods. Two forms ship: **marker-annotation** (methods tagged
with a user annotation) and **type-based** (every method on a class / interface
implementer). String-expression pointcuts (`execution(...)`) are deferred.

### Advice

- `@Before` — before the matched body.
- `@After` — on every exit path (return or throw).
- `@Around` — wraps the call; receives a typed `@Original` proceed function and
  chooses if/when to invoke it.
- `@AfterReturning` — on normal return; receives the return value.
- `@AfterThrowing` — on throw; receives the `Throwable`.

`@Order(n)` chains multiple matching aspects; `@NoAdvice` opts a method out.
Advice lives in an `@Aspect` class, which is itself a `@Component` (so it can
`@Inject` collaborators).

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
wrapper **is** the method — `this.method()` is the woven version (no proxy,
no self-invocation bypass). Overrides inherit advice via the vtable; `@NoAdvice`
opts out.

> **Not yet lowered:** the typed `JoinPoint<R, A...>` parameter and the
> matched-annotation capture (`Audited annot` advice parameter). Advice runs today
> but cannot yet receive that typed context (`Method.cpp:445`). The single largest
> open AOP piece.

---

## What primavera adds (policy)

The enterprise opinion built on this substrate lives in `org.cajeta.primavera`:

- **Request / session scope** — non-identity scopes keyed on the logical request,
  over `cajeta.concurrent.FiberLocal`; the request-scoped `@Component` lifetime.
- **Web model** — HTTP request/response, a handler API, `@RestServer`, and a
  pluggable executor (fiber-per-request vs threadpool-over-completion-ports).
- **Stereotypes** — `@Repository`, `@Service` (named roles over `@Component`).
- **Deployment profiles** — `@Profile("prod"|"test"|...)` selection.
- **Test harness** — `@TestComponent` overrides and request-scope seeding, over
  the core override seam, on cajeta-unit.

A project that dislikes primavera's opinion keeps the core substrate and builds
its own framework on the same `@Component` / `@Inject` / `@Factory` graph — which
is exactly why the substrate must be core: every library speaks one DI vocabulary,
with no framework dependency, instead of fragmenting like Java's
`@Inject`/`@Autowired`/`@Resource`.

---

## Interim status

The compiler **implements all of the above today** (`CajetaModule.cpp`,
`CajetaLlvmVisitor.h`, `ComponentInjectMethod.cpp`, `FactoryProviderMethod.cpp`):
graph build, the implemented allocation modes, lifecycle hooks, aspect weaving, and
the full **`@Factory`** substrate — discovery (R1/R3/R4), graph resolution
(provider vs factory-injection, the `@Component`/`@Factory` ambiguity, factory-param
cycles), provider-accessor codegen (`@Singleton` memo / `@Transient` fresh, R2 `#T`
fresh-owned), and assisted-injection call-site threading (the consumer passes only
the assisted args). The long-term annotation-processing / codegen **extension point**
goal applies to the **policy** layer (so primavera or any framework plugs its
stereotypes, scopes, and web model in without core changes) — **not** to the DI
substrate, which stays core.

> Known v1 gap: a `@Transient` factory product stored in a singleton owner's field
> is not yet drop-tracked per holder (leaks at owner destruction) — consistent with
> the existing transient/owner-scope handling; revisit with owning-collections /
> lean-linker drop synthesis.
