# Dependency Injection container — moved to cazo

> **The DI/component container has moved out of the Cajeta standard library.
> Aspect-oriented programming and `@Inject` stay in the core language.**
>
> The component model (`@Component` / `@Repository` / `@TestComponent`),
> lifecycle (`@PostConstruct` / `@PreDestroy`), profiles (`@Profile`), and the
> allocation/scope model are **policy**, not core language. They now live in
> **cazo**, Cajeta's opinionated enterprise framework:
>
> **https://github.com/jklappenbach/cajeta-cazo** — see `docs/AspectModel.md`.
>
> **Aspect weaving** (`@Aspect` / `@Before` / `@After` / `@AfterReturning` /
> `@AfterThrowing` / `@Around` / `@Original` / `@Order`) and **`@Inject`** were
> **not** moved — they remain core-language mechanism. See "What stays" below.

## Why it moved

Cajeta the *language* ships **mechanism** — `@Inject` (the DI primitive that
stays in the language), aspect weaving, `cajeta.concurrent.FiberLocal` (ambient
per-request state), and reflection. An opinion about how to assemble those into
enterprise services — a component container, scopes, profiles — is **policy**.
Baking one framework's policy into the core would (a) push it on developers
who'd rather use a different pattern, and (b) discourage others from innovating
their own frameworks on the same primitives. So the policy lives in a swappable
library; the core stays small.

## Interim status (important)

This is a **documentation/ownership** move, made ahead of the implementation
move. The annotations above are **still recognized by the compiler today**
(`CajetaModule.cpp`, `CajetaLlvmVisitor.h`, `ComponentInjectMethod.cpp`) — DI
graph build, the four allocation modes, profiles, and lifecycle hooks all still
work as before. What changed is that the **container's authoritative spec,
examples, and roadmap now live in cazo**, not here. Aspect weaving and `@Inject`
are unaffected — they were never cazo's to own.

The long-term aim is to re-home the *implementation* behind a language-level
annotation-processing / codegen extension point, so the core compiler carries no
framework-specific policy and cazo (or any third-party framework) plugs in. That
extraction is tracked in the cazo roadmap (`plan/cazo-plan.md`) and the
cajeta-two compiler work.

## What stays in the language

- **Aspect weaving** (`@Aspect` / `@Before` / `@After` / `@AfterReturning` /
  `@AfterThrowing` / `@Around` / `@Original` / `@Order`) — a core-language
  mechanism, recognized and woven by the compiler. It was **not** moved to cazo.
- **`@Inject`** — the dependency-injection *point*. Documented in
  [`Annotations.md`](../reflect/Annotations.md). cazo's container resolves it; so
  could any other framework.
- **`cajeta.concurrent.FiberLocal` / `FiberContext`** — the neutral primitive
  cazo's request/session scope is built on.
- **Reflection** — see [`Reflection.md`](../reflect/Reflection.md).
