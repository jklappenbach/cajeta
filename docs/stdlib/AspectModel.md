# Aspect-Oriented Programming + Dependency Injection — moved to cazo

> **This framework has moved out of the Cajeta standard library.**
>
> The component model (`@Component` / `@Repository` / `@TestComponent`),
> aspect weaving (`@Aspect` / `@Before` / `@After` / `@AfterReturning` /
> `@AfterThrowing` / `@Around` / `@Original` / `@Order`), lifecycle
> (`@PostConstruct` / `@PreDestroy`), profiles (`@Profile`), and the
> allocation/scope model are **policy**, not core language. They now live in
> **cazo**, Cajeta's opinionated enterprise framework:
>
> **https://github.com/jklappenbach/cajeta-cazo** — see `docs/AspectModel.md`.

## Why it moved

Cajeta the *language* ships **mechanism** — `@Inject` (the one DI primitive that
stays in the language), `cajeta.concurrent.FiberLocal` (ambient per-request
state), and reflection. An opinion about how to assemble those into enterprise
services — a component container, scopes, AOP — is **policy**. Baking one
framework's policy into the core would (a) push it on developers who'd rather
use a different pattern, and (b) discourage others from innovating their own
frameworks on the same primitives. So the policy lives in a swappable library;
the core stays small.

## Interim status (important)

This is a **documentation/ownership** move, made ahead of the implementation
move. The annotations above are **still recognized by the compiler today**
(`CajetaModule.cpp`, `CajetaLlvmVisitor.h`, `ComponentInjectMethod.cpp`) — DI
graph build, the four allocation modes, profiles, aspect weaving, and lifecycle
hooks all still work as before. What changed is that their **authoritative
spec, examples, and roadmap now live in cazo**, not here.

The long-term aim is to re-home the *implementation* behind a language-level
annotation-processing / codegen extension point, so the core compiler carries no
framework-specific policy and cazo (or any third-party framework) plugs in. That
extraction is tracked in the cazo roadmap (`plan/cazo-plan.md`) and the
cajeta-two compiler work.

## What stays in the language

- **`@Inject`** — the dependency-injection *point*. Documented in
  [`Annotations.md`](Annotations.md). cazo's container resolves it; so could any
  other framework.
- **`cajeta.concurrent.FiberLocal` / `FiberContext`** — see
  [`FiberLocal.md`](FiberLocal.md). The neutral primitive cazo's request/session
  scope is built on.
- **Reflection** — see [`Reflection.md`](Reflection.md).
