# Cajeta Lint Rules — Catalog v1

Lint rules are compile-time **warnings**, never errors. The compiler reports them; the build still succeeds. This is by design — the language defers to runtime semantics for everything that affects correctness, and uses lint to surface things the author should *consider* but isn't forced to act on.

## Foundational principles

- **No lint warning becomes a compile error, ever.** Even if every lint warning would be wrong, the binary still builds. This is non-negotiable. A future build flag (`--lint-as-errors` or similar) could opt projects in, but the default is always warning.
- **Every rule has a stable ID.** IDs are kebab-case strings (`"uncaught-throws"`). They're stable across compiler versions — renaming a rule's ID would break every `@SuppressLint("…")` annotation in the world's code.
- **Suppression is by ID via `@SuppressLint`.** A method (or class) annotated `@SuppressLint("uncaught-throws", "unused-local")` silences only those specific rules. Other lints still fire.
- **No catch-all suppression.** There's no `@SuppressLint("*")` or bare `@SuppressLint()`. Suppression must name what you're suppressing — keeps grep'ability and forces conscious choice.
- **The runtime is the safety net.** Lint warnings have no operational consequence. An uncaught exception at the top of a fiber or main thread is caught by the system default catch (`ErrorModel.md` § System default catch), logged, and the process or fiber exits cleanly. The lint exists to make the *path* visible at compile time; the runtime ensures the failure mode is well-defined regardless.

## Notes — informational diagnostics (below `warning`)

Alongside lint **warnings** the compiler emits a lighter severity, `note:`, for
things the author should be *aware* of but that are neither a problem nor a code
smell — there is nothing to fix. A note is **not** a lint warning:

- It is emitted `note: [id] message` (vs. `warning: [id] message`). The severity
  word is the signal: a warning invites action, a note simply informs.
- It is **not suppressible** — `@SuppressLint` covers warnings. A note has no
  remedy to opt out of; it is reporting a fact about how your code was compiled.
- It is **sticky**: emitted every build (deduplicated per distinct subject), so
  the information stays visible without ever nagging.

Notes exist so a capability or path choice can "make itself known" without being
framed as something to avoid — the deliberate middle ground between silence and a
warning.

### `mma-tiering`

**What it reports:** a `CooperativeMatrix<T,…>` (the matrix-core tile-MMA type)
took the **portable software tile-matmul** on a backend that exposes no native
cooperative-matrix config for its dtype — e.g. `bfloat16` on Vulkan, or anything
on the CPU. Emitted once per GEMM (on the A operand).

**Why a note, not a warning:** the software path is *correct* and bit-identical;
it just isn't matrix-core accelerated, and it **auto-promotes** to the hardware
cores on a backend that does expose the config (f16/int8 on Vulkan, bf16 WMMA on
AMD). Nothing is wrong and nothing should be "fixed" — the note simply tells you
which tier you got so throughput is never a surprise. See
`stdlib/xpu/core/CooperativeMatrix` § Runs on every backend.

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

### `wildcard-materialize-in-loop`

**What it checks:** An element-producing call (`next()`, `get()`) on a wildcard-typed receiver (`Stream<?>`, `Optional<?>`, etc.) inside a loop body. Trigger requires (a) `module->hasLoopContext()` true at the call site and (b) the resolved receiver's `CajetaClass::isWildcardInstantiation()` true.

**Why it exists:** Wildcard dispatch goes through the template-relative vtable hash. Inside a loop that's one indirect call per iteration, the inliner can't see through it, and LLVM loses the unroll/vectorize opportunity. The "10–20× larger loop body" framing in `TemplateWildcard.md` § Performance.

**Example warning output:**
```
warning: [wildcard-materialize-in-loop] call to 'next' on wildcard-typed
  receiver cajeta.lang.stream.Stream<?> inside a loop in worker — dispatch
  goes through the template-relative vtable hash on every iteration;
  downcast to the concrete type at the loop boundary, or suppress with
  @SuppressLint("wildcard-materialize-in-loop").
```

**Suppression:** `@SuppressLint("wildcard-materialize-in-loop")` on the enclosing method.

**When to suppress:** The wildcard cursor is chain-walker code that only calls type-erased `unwrap` / `splittableSize` / `trySplitRoot` (those don't materialize T).

**When NOT to suppress:** The loop is the actual element-processing hot path. Downcast at the worker boundary first (per the chain-walker pattern in `TemplateWildcard.md` Step 5b) so the hot loop stays specialized.

**Known limitation.** The emit is skipped when the enclosing method is a method-level template (`<T> void foo(...)`) or its instantiation — the wildcard sentinel is reused as a stand-in for both "real `?`" and "uninstantiated T placeholder", and we can't distinguish without capture conversion (P2-2 item 1). False negatives on real wildcard receivers inside method-template bodies are accepted in v1.

---

### `wildcard-crosses-hot-boundary`

**What it checks:** A call inside a loop whose target method returns a wildcard-typed result (`Stream<?>`, `Optional<?>`, etc.). Trigger requires `hasLoopContext()` and `targetMethod->getReturnType()->isWildcardInstantiation()`.

**Why it exists:** Wildcard returns can't be specialized at the call site — every receive site that wants the concrete type pays a downcast and loses the inline opportunity.

**Example warning output:**
```
warning: [wildcard-crosses-hot-boundary] call to 'next' inside a loop
  in worker returns wildcard-typed cajeta.lang.Optional<?> — the receive
  site can't be specialized at the call boundary; restructure to concrete
  or suppress with @SuppressLint("wildcard-crosses-hot-boundary").
```

**Suppression:** `@SuppressLint("wildcard-crosses-hot-boundary")` on the enclosing method.

**When to suppress:** The boundary is a one-shot per stream (chain-walker setup, splittable-root pick) rather than per-element.

**When NOT to suppress:** The wildcard return is the per-element value itself; restructure to either return concrete or keep the wildcard inside the abstraction.

**Known limitation.** Same method-template false-negative as `wildcard-materialize-in-loop`.

---

### `wildcard-field-in-small-class`

**What it checks:** A class declares a wildcard-typed field (`Stream<?> source`, `Optional<?> cached`). Trigger walks `propertyList` at the end of `generatePrototype` and fires per wildcard-instantiation property.

**Why it exists:** Wildcard fields drop through `__cajeta_class_virtual_drop` — every drop is an indirect call plus a vtable load, where a concrete-field equivalent would emit a direct call (inlinable). For a class allocated millions of times the difference is measurable.

**Example warning output:**
```
warning: [wildcard-field-in-small-class] class test.Holder declares
  wildcard-typed field 'source' of type cajeta.lang.stream.Stream<?> —
  every drop of an instance routes through virtual-drop dispatch; if
  instances of this class are constructed in a hot path, pick a concrete
  element type or push the wildcard outward. Suppress with
  @SuppressLint("wildcard-field-in-small-class").
```

**Suppression:** `@SuppressLint("wildcard-field-in-small-class")` on the class declaration.

**When to suppress:** The class is allocated infrequently (cached, pooled, long-lived).

**When NOT to suppress:** The class is in the construction hot path; pick a concrete element type or push the wildcard outward.

**Known exclusion.** Views (`CajetaView`) are skipped — they don't use the virtual-drop path. Wildcard-instantiation classes themselves are also skipped — those are compiler-generated erased proxies, not author-written.

---

### `discarded-wildcard-next`

**What it checks:** An MCE at statement position with an element-producing method name (`next` / `get`) on a wildcard-typed receiver where the returned `#Optional<?>` isn't bound. Trigger fires in `ExpressionStatement::generateCode`.

**Why it exists:** The boxed Optional allocates on the heap, the wildcard dispatch costs an indirect call, and nothing observes the result — both costs are wasted.

**Example warning output:**
```
warning: [discarded-wildcard-next] call to 'next' on wildcard-typed
  receiver cajeta.lang.stream.ArrayStream<?> in statement position in
  run — the result (a heap Optional<?>) is discarded; remove the call
  if you don't need its value, or bind the result and act on it.
  Suppress with @SuppressLint("discarded-wildcard-next").
```

**Suppression:** `@SuppressLint("discarded-wildcard-next")` on the enclosing method.

**When to suppress:** The call is intentionally used for its side effect (uncommon — `peek` exists for that).

**When NOT to suppress:** The result is genuinely discarded; remove the call or restructure.

**Known limitation.** Same method-template false-negative as the loop-site rules.

---

*Future rules slot in below following the same template: ID, what it checks, why, example output, suppression, when (not) to suppress.*

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
- **`equals-hash-pair`** — A class declares one of `operator==` / `hash()` but inherits the other from `Object`. Inherited identity-`hash()` paired with custom value-`operator==` (or the reverse) silently breaks HashMap / HashSet: equal values produce different bucket indices and stored entries become un-findable. The class either overrides both or neither. **Suppress when:** intentionally implementing value-equality on a class that will never be a HashMap key (rare; usually a code smell suggesting the class wants a different name). **Don't suppress when:** the class is a likely map / set key (entity types, value types, identifiers). Demoted from compile-error to lint 2026-05-18 — see `stdlib/lang/Object.md` § Open questions for the design pass.
- **`transfer-on-view-string`** — `#expr` where `expr` is a view-mode `String` (string literal `"abc"`, or any String constructed via `String.viewOf(...)`). The `#` operator transfers ownership; view-mode Strings don't own their bytes (literals point at static storage; views point at memory owned elsewhere), so the transfer is a no-op. The lint surfaces the meaningless `#` so the user notices. **Suppress when:** writing template code that takes ownership generically and happens to receive a literal in one specialization. **Don't suppress when:** the `#` is a misunderstanding about what literals are. See `stdlib/lang/String.md` § Memory model.
- **`format-template-arg-mismatch`** — `String.format(template, args...)` or `String.printf(template, args...)` where `template` is a literal string AND the placeholder count / types don't match the args. Python-style: `{}` count mismatch or named `{foo}` reference not satisfied. Printf-style: `%s` / `%d` / `%f` / etc. count mismatch or type mismatch (e.g., `%d` with a String arg). Catches the entire bug class of "format string drift" without runtime cost. **Suppress when:** intentionally passing extra args for future template revisions, or when the template is built up at compile time in a way the validator can't see through. **Don't suppress when:** the mismatch is a typo. See `stdlib/lang/String.md` § `String.format`.

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
