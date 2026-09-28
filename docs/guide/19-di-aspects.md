# 19 — DI & aspects

Dependency injection and aspect weaving are core language, resolved entirely
at compile time. The compiler builds the DI graph, wires it as direct calls,
and weaves advice into the binary. No runtime container, no proxies, no
reflection. Unaffected code compiles unchanged.

Tour demos: [AspectsDiDemo](../../samples/tour/src/main/cajeta/tour/lang/AspectsDiDemo.cajeta),
[FactoryDemo](../../samples/tour/src/main/cajeta/tour/lang/FactoryDemo.cajeta).

## Components

`@Component` registers a class with the DI graph. `@Inject` fields are
populated before any user code observes the instance:

```cajeta
@Component
public class Registry {
    public int32 entries;
    public Registry() { this.entries = 100; return; }
}

@Component
public class AuditTrail {
    @Inject Registry registry;
    public AuditTrail() { return; }
    public int32 capacity() {
        return this.registry.entries;
    }
}
```

`Type.__cajeta_inject()` returns the lazily-constructed process singleton;
resolution is transitive, so requesting `AuditTrail` builds `Registry` first:

```cajeta
AuditTrail t = AuditTrail.__cajeta_inject();
int32 spare = t.capacity();
```

The graph is checked at compile time: a missing provider, a cycle, or an
ambiguous unqualified match is a compile error. `@PostConstruct` and
`@PreDestroy` hook the lifecycle.

## Scopes

A component's lifetime is a named scope. It is `"Singleton"` unless the
component says otherwise with `@Component(scope = ...)`. A scope other than
the two built-in ones, `"Singleton"` and `"Transient"`, is published by the
code that bounds it, with `@Scope("Name")`. Where the annotation goes decides
what kind of scope it is:

- on a method, the scope is one activation of that method, ended when it
  returns or throws.- on a class, the scope is one instance's lifetime, ended when the instance
  is dropped.

A method scope:

```cajeta
public final class Pipeline {
    @Scope("Request")
    public static int32 run(int32 n) {
        Basket b = Basket.__cajeta_inject();
        b.items = b.items + n;
        Basket same = Basket.__cajeta_inject();
        return same.items;
    }
}

@Component(scope = "Request")
public class Basket {
    public int32 items;
    public Basket() { this.items = 0; return; }
    @PreDestroy
    public void close() { return; }
}
```

Every injection of `Basket` inside one activation of `Pipeline.run` gets the
same instance, and the next activation gets a fresh one. When `run` returns
or throws, the scope ends: `close` runs and the basket is freed. An injection
outside every activation throws `ScopeNotActiveException`, naming the
component and the scope.

An instance scope:

```cajeta
@Scope("Session")
public class Session {
    public Session() { return; }
    public int32 remember(int32 n) {
        Preferences p = Preferences.__cajeta_inject();
        p.count = p.count + n;
        return p.count;
    }
}

@Component(scope = "Session")
public class Preferences {
    public int32 count;
    public Preferences() { this.count = 0; return; }
}
```

An instance is current while one of its non-private instance methods runs,
from the end of its constructor on. Its components survive between those
calls, so successive `remember` calls on one session add up, and they are
freed with the session.

The consumer writes only the name. The anchor behind it can move in a later
release without touching any component. Several methods in one package may
open the same scope, and a clash between packages is resolved by qualifying
the name, as in `"dev.cajeta.primavera.Request"`.

Anchor on the method that runs once per unit of work, not on an object that
is pooled or reused. A reused object's instance scope lasts as long as the
reuse.

### Lifetimes between components

A field is filled once, when its holder is built, so it may hold only a
component that outlives the holder: one in the same scope, a singleton, or a
scope the holder's scope is provably inside. Anything else is a compile error
that names `Scoped<T>`. A handle resolves in the scope active at each `get()`:

```cajeta
import cajeta.aot.Scoped;

@Component
public class Checkout {
    @Inject Scoped<Basket> basket;
    public Checkout() { return; }
    public int32 items() {
        Basket b = this.basket.get();
        return b.items;
    }
}
```

A scope is provably inside another when its publication says so with
`@Scope(value = "Message", within = "Connection")`, or when it is a method
scope published on a class that publishes the other as an instance scope.
Entering a `within` scope while the outer one is inactive throws on entry.

A site can still ask for a fresh instance of a component that declares no
scope: `@Inject(scope = "Owner")` builds one per injecting object, and
`@Inject(scope = "Transient")` one per injection. The holder owns and frees
it. A site may repeat a declared scope but not change it.

### Values produced at run time

Some values are produced by the program rather than built by the graph. A
component with no no-argument constructor is only ever provided:

```cajeta
import cajeta.aot.Components;

@Component(scope = "Request")
public class Caller {
    public int32 subject;
    public Caller(int32 subject) { this.subject = subject; return; }
}

public final class Auth {
    @Scope("Request")
    public static int32 serve(int32 subject) {
        Caller c = heap Caller(subject);
        Components.provide(#c);
        Caller who = Caller.__cajeta_inject();
        return who.subject;
    }
}
```

`Components.provide` places the value in the active scope its type declares,
and the scope owns it from there. Injecting it before it is provided throws
`NotProvidedException`, and a second provide in one scope throws.

### Scopes and fibers

A task spawned inside a scope, and joined by its `scope { }` block, sees the
spawner's scoped components. A `detach`ed task sees none of them, and gets
what it needs as owned arguments. When two fibers inject a component that is
not built yet, one builds it and the other waits for it. If the build throws,
the builder gets its exception and each waiter gets a
`ScopedBuildFailedException`, and the next injection builds again.

## Multibinding

A site typed `ArrayList<T>` receives every active component assignable to
`T`, in canonical-name order. A site typed `HashMap<String, T>` receives the
same set keyed by component name, with an unnamed component keyed by its
simple class name. Nothing assignable means an empty container, not an
error. The elements are the graph's singletons and the container holds
them as borrows, so clearing or dropping it frees nothing.

```cajeta
import cajeta.collection.ArrayList;
import cajeta.collection.HashMap;

public interface Codec {
    public int32 level();
}

@Component(name = "gzip")
public class Gzip implements Codec {
    public Gzip() { return; }
    public int32 level() { return 6; }
}

@Component
public class Brotli implements Codec {
    public Brotli() { return; }
    public int32 level() { return 11; }
}

@Component
public class Codecs {
    @Inject ArrayList<Codec> all;
    @Inject HashMap<String, Codec> byName;
    public Codecs() { return; }
}
```

Which implementations exist is decided at build, by profiles. Which one a
program uses is ordinary selection over the set, by index, by name or by
any predicate, and that is how configuration picks a provider:

```cajeta
Codecs c = Codecs.__cajeta_inject();
int32 present = c.all.count();                 // 2, Brotli before Gzip
int32 chosen = c.byName.get("gzip").level();  // 6
```

## Factories

`@Factory` covers what constructor injection can't: unowned third-party
types, caller-supplied arguments, and setup beyond the constructor. A
provider method whose parameters are all `@Inject` makes its return type
injectable. A method with unmarked ("assisted") parameters works the other
way: consumers inject the factory and pass only the assisted arguments —
the compiler threads the injected ones.

```cajeta
public class Conn {
    public int32 id;
    public Conn() { this.id = 0; return; }
}

@Factory
public class ConnFactory {
    #Conn shared(@Inject Registry registry) {
        Conn c = heap Conn();
        c.id = registry.entries;
        return c;
    }

    @Transient #Conn make(@Inject Registry registry, int32 id) {
        Conn c = heap Conn();
        c.id = id;
        return c;
    }
}
```

Provider methods return a fresh owned `#T`; the framework owns the caching.
`@Singleton` (the default) memoizes; `@Transient` builds a fresh product per
call. The tour's FactoryDemo shows the consumer side of both paths.

## Profiles

A component annotated `@Profile("test")` joins the graph only when the
active profile matches. `--profile=<name>` sets it at compile time; the
default is `prod`. The `test` profile also enables the runtime override
seam test harnesses use to substitute mocks.

## Aspects

An `@Aspect` class holds advice; a marker annotation is the pointcut.
Only annotated methods get wrapped.

```cajeta
annotation Timed { }

@Aspect
public class TimingAspect {
    @Before(Timed.class)
    public static void enter() {
        System.stdout.println("entering a @Timed method");
    }
}

public class Worker {
    @Timed public static int32 work() {
        return 7;
    }
}
```

`@Before` runs before the body, `@After` on every exit path,
`@AfterReturning` / `@AfterThrowing` on the specific one. `@Around` wraps the
call: its first parameter is a typed `proceed` function bound to the original
body, so it can transform arguments and the return value. `@Order(n)` chains
multiple aspects. The woven wrapper *is* the method — there is no proxy to
bypass on self-invocation.

Full model: [the aspect specification](../specification/lang/AspectModel.md).

Next: [Error handling](20-error-handling.md).
