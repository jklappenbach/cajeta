---
id: language-annotations
applies-to: [cajeta/language/annotations, cajeta/language/synthesis, cajeta/language/di, cajeta/language/aspects]
title: Annotations, code synthesis, compile-time DI, and aspects
description: The `annotation` keyword (not @interface), the Lombok-style synthesis family, and DI/aspects resolved entirely at compile time — no container, no proxies, no reflection.
---

# Annotations, synthesis, DI & aspects

Apply with `@Name` / `@Name(args)` before a declaration. Any identifier is a
valid annotation; one the compiler doesn't recognize is **inert** — no code,
no overhead — until an aspect pointcut or reflection reads it back.

**Declare your own with the `annotation` keyword** — there is no
`@interface`:

```cajeta
annotation Timed { }
```

## The synthesis family

Each synthesized member is an ordinary method (reflection-visible,
debuggable) and is **skipped if you declare the same signature by hand**.

| Annotation | Synthesizes |
|---|---|
| `@Getter` / `@Setter` | Accessors, `size()`-style naming: `name()`, `name(v)` |
| `@ToString` | `toString()` as `ClassName(field=value, …)` |
| `@AutoHash` | A structural `hash()` (pairs with `operator==` — see `cajeta/language/classes`) |
| `@NoArgsConstructor` / `@AllArgsConstructor` / `@RequiredArgsConstructor` | Constructors |
| `@Builder` | Fluent builder + static `builder()`; `@Builder.Default` supplies the field initializer as fallback |
| `@With` | Per-field copy-with (`withX(...)`) |
| `@NonNull` | Null checks at entry / construction / return |
| `@Data` | getters, setters, `toString`, hash, required-args ctor |
| `@Value` | Immutable bundle: getters, `toString`, hash, all-args ctor — no setters |

`@Cleanup` is planned, not implemented — the drop chain already releases
owned locals (`cajeta/language/errors`).

## DI — compile-time, no container

`@Component` registers a class; `@Inject` fields are populated before any
user code sees the instance; `Type.__cajeta_inject()` returns the lazily
constructed singleton, resolving transitively. **The graph is checked at
compile time** — a missing provider, a cycle, or an ambiguous unqualified
match is a compile *error*. Lifetime is per injection site
(`allocate = …`: singleton default, per-owner, transient);
`@PostConstruct` / `@PreDestroy` hook the lifecycle.

`@Factory` covers what constructor injection can't (unowned third-party
types, caller-supplied arguments, setup beyond the constructor): a provider
method whose parameters are all `@Inject` makes its return type injectable;
unmarked "assisted" parameters are supplied by the caller while the compiler
threads the injected ones. Providers return a fresh owned `#T`; the framework
owns caching (`@Singleton` memoizes, `@Transient` builds per call).

`@Profile("test")` joins the graph only under `--profile=test`, which also
enables the runtime override seam test harnesses use for mocks.

## Aspects

An `@Aspect` class holds advice; **a marker annotation is the pointcut**, so
only annotated methods are wrapped. `@Before` / `@After` /
`@AfterReturning` / `@AfterThrowing` / `@Around` (whose first parameter is a
typed `proceed` bound to the original body); `@Order(n)` chains aspects. The
woven wrapper **is** the method — there is no proxy to bypass on
self-invocation.

## Worked example (verified: returns 357, advice fires)

```cajeta
package dev.cajeta.skills;

import cajeta.lang.System;

annotation Timed { }

@Builder
@ToString
public class Book {
    public String title;
    @Builder.Default public int32 pages = 100;
}

@Value
public class Rgb {
    int32 r;
    int32 g;
    int32 b;
}

@Aspect
public class TimingAspect {
    @Before(Timed.class)
    public static void enter() {
        System.stdout.println("entering a @Timed method");
    }
}

@Component
public class Registry {
    public int32 entries;
    public Registry() { this.entries = 20; return; }
}

@Component
public class AuditTrail {
    @Inject Registry registry;
    public AuditTrail() { return; }
    public int32 capacity() { return this.registry.entries; }
}

public class Worker {
    @Timed public static int32 work() { return 7; }
}

public class AnnotationsDemo {
    public static int32 run() {
        Book d = Book.builder().title("Untitled").build();
        int32 pages = d.pages;              // 100 — @Builder.Default fallback
        Rgb c = stack Rgb(230, 126, 34);
        int32 red = c.r();                  // @Value getter
        int32 worked = Worker.work();       // advice fires first — 7
        AuditTrail t = AuditTrail.__cajeta_inject();
        int32 cap = t.capacity();           // 20 — Registry injected
        return pages + red + worked + cap;  // 357
    }
}
```

## Sharp edge

Keep annotation *types* out of the structure map — declare `annotation Foo {}`
and apply it; don't model annotations as classes. Reflection reads argument
values back (`method.hasAnnotation(...)`) — see the `cajeta.reflect` skills.
