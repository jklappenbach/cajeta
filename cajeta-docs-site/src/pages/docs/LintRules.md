---
title: 'Cajeta Lint Rules — Catalog v1'
layout: '~/layouts/MarkdownLayout.astro'
category: 'Process'
description: 'Lint rules are compile-time warnings, never errors. The compiler reports them; the build still succeeds. This is by design — the language defers to runtime semantics for everything that affects correc...'
---

Lint rules are compile-time **warnings**, never errors. The compiler reports them; the build still succeeds. This is by design — the language defers to runtime semantics for everything that affects correctness, and uses lint to surface things the author should *consider* but isn't forced to act on.

## Foundational principles

- **No lint warning becomes a compile error, ever.** Even if every lint warning would be wrong, the binary still builds. This is non-negotiable. A future build flag (`--lint-as-errors` or similar) could opt projects in, but the default is always warning.
- **Every rule has a stable ID.** IDs are kebab-case strings (`"uncaught-throws"`). They're stable across compiler versions — renaming a rule's ID would break every `@SuppressLint("…")` annotation in the world's code.
- **Suppression is by ID via `@SuppressLint`.** A method (or class) annotated `@SuppressLint("uncaught-throws", "unused-local")` silences only those specific rules. Other lints still fire.
- **No catch-all suppression.** There's no `@SuppressLint("*")` or bare `@SuppressLint()`. Suppression must name what you're suppressing — keeps grep'ability and forces conscious choice.
- **The runtime is the safety net.** Lint warnings have no operational consequence. An uncaught exception at the top of a fiber or main thread is caught by the system default catch (`ErrorModel.md` § System default catch), logged, and the process or fiber exits cleanly. The lint exists to make the *path* visible at compile time; the runtime ensures the failure mode is well-defined regardless.

## Suppression syntax

```cajeta
@SuppressLint("uncaught-throws")
public Response wrapper(Url u) {
    return http.get(u);   // declared throws IOException; warning silenced
}

@SuppressLint("uncaught-throws", "unused-local")
public void multi() {
    int32 unused = 7;
    return riskyOp();
}
```

Argument forms:
- `@SuppressLint("id")` — single rule
- `@SuppressLint({"id-a", "id-b"})` — multiple rules via array initializer

Scope:
- **Method-level** (v1): the annotation on a method silences the listed rules for that method's body.
- **Class-level** (future): annotation on a class silences for every method in the class. Useful for generated code or DSL classes where the noise vs. signal ratio is upside-down.

## Rule catalog

### `uncaught-throws`

**What it checks:** A call site invokes a method whose `throws` clause declares one or more `RecoverableException` subtypes. The enclosing method neither (a) catches those types in a surrounding `try/catch`, nor (b) declares them in its own `throws` clause.

**Why it exists:** The `throws` clause is the author's stated set of failure modes a caller should consider. When a caller silently ignores that list, the documentation chain breaks — readers downstream don't know what can flow out.

**Doesn't force handling, just visibility.** Per the language's "no enforced checked exceptions" position, the lint doesn't make the build fail. The runtime system catch handles whatever escapes.

**Example warning output:**
```
warning: [uncaught-throws] call to fetch can throw IOException but
  enclosing run() neither catches nor declares it
```

**Suppression:** `@SuppressLint("uncaught-throws")` on the enclosing method.

**When to suppress:**
- Thin pass-through wrappers (logging, metrics, transactions) where the wrapper is transparent to the failure flow.
- Lambda bodies in higher-order calls where the lambda type can't carry a `throws` clause.
- During refactors when the throws clauses are being reorganized.

**When NOT to suppress:**
- Public API methods. Hides the failure contract from callers.
- Methods doing real work where the throws set is meaningful.

---

*The catalog has one entry today. Future rules slot in here following the same template: ID, what it checks, why, example output, suppression, when (not) to suppress.*

## Future rules (sketches)

These aren't implemented yet — listed to seed the catalog as new lint checks land:

- **`unused-local`** — A local variable is declared and assigned but never read.
- **`unused-import`** — An imported type is never referenced in the compilation unit.
- **`unused-parameter`** — A method parameter is never read in the body.
- **`dead-code`** — Code after a `return`/`throw`/`continue` that can't execute.
- **`shadowed-variable`** — A local declaration shadows a name from an outer scope.
- **`empty-catch`** — A `catch (X e) {}` arm has no body. Almost always a bug; explicit ignore should be written `catch (X _) { /* explicit ignore */ }` or similar marker.
- **`unnecessary-cast`** — `(T) x` where x is already T.
- **`narrow-conversion-no-cast`** — Implicit narrowing (e.g. assigning int64 to int32) without a cast. Most languages catch this; whether Cajeta forces a cast or warns is TBD.
- **`string-concat-in-loop`** — Performance hint: `result = result + str` in a loop allocates repeatedly. Should use a builder or join.
- **`aspect-pointcut-no-match`** — `@Before(Audited.class)` in an aspect class doesn't match any method in the compilation unit. Usually indicates a renamed annotation or unreachable aspect.

## Implementation notes

The lint pass runs during codegen (per-call-site for `uncaught-throws`, per-declaration for the future structural ones). Each emit:

1. Compose the warning message with the rule ID prefix: `warning: [rule-id] message`.
2. Walk the enclosing method's `@SuppressLint` argument list. If the rule's ID appears, skip the emit.
3. Otherwise write to stderr (or whatever the build's diagnostic sink is).

Suppression args are captured on `Annotatable` at parse time: when the visitor sees a `@SuppressLint(...)` annotation, it parses the string literal arguments and stores them in a side list (`suppressedLints`) keyed by the holding declaration. The lint emitter queries that list before printing.

When the throws clause grammar gains structural args (task A1 from `AspectModel.md` — annotation parameter capture), `@SuppressLint` consolidates onto that generic infrastructure. Today it's special-cased.

## Why not Java's `@SuppressWarnings("...")`?

Java's pattern is the same idea with a different name. The reasons we picked `@SuppressLint`:

- `Warnings` is a broad term. The Cajeta compiler's emit-to-stderr noise is specifically a *lint* category — author-facing advice. Other categories (deprecation notices, compatibility warnings) might warrant their own suppressors (`@SuppressDeprecation`, etc.) without conflating with lint.
- Android's `@SuppressLint` already establishes the precedent — Cajeta users coming from that ecosystem land in familiar territory.
- `@SuppressLint` makes the *kind* of thing being silenced explicit.

## Caveats

- **No `--lint-as-errors` flag in v1.** When projects mature and want stricter enforcement, the flag can be added without changing any rule semantics — it just promotes warning output to non-zero exit.
- **No `// lint:ignore[rule-id]` comments yet.** Annotation-based suppression covers method/class scope; expression-level suppression via comments would be useful for one-line cases but isn't in v1.
- **Selective suppression has no granularity below the rule.** `@SuppressLint("uncaught-throws")` silences ALL uncaught-throws warnings in the method, not just one specific exception type. If granularity is needed, the rule could grow `@SuppressLint("uncaught-throws:IOException")` later — not in v1.
