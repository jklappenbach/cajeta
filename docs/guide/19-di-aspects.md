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
ambiguous unqualified match is a compile error. The lifetime is chosen per
injection site (`allocate = ...`): singleton (the default), per-owner, or
transient. `@PostConstruct` / `@PreDestroy` hook the lifecycle.

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
