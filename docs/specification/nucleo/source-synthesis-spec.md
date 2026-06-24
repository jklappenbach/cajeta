# Source Synthesis (Tier A) — Specification

> Status: draft for review (2026-06-23). A **compiler facility** (Layer-1a foundation):
> the reusable Tier-A source-synthesis mechanism that generalizes the special-cased
> `@Logged` member synthesizer and the codec body synthesizers into a shared helper +
> a registry. Companions: `language-foundations.md` §1.6 (Tier A), `records-spec.md`
> (the schema source synthesized accessors reflect over), `transform-intrinsics-spec.md`
> (Tier-B bounded-IR transforms — the *separate*, trusted sibling).
>
> This is a **spec** — requirements and use cases (the *why/what*). Build decisions (the
> *how*) are deferred as `> **TBD (plan-time):**` markers and collected in §9, to be
> resolved when this spec is turned into a plan. Outline-numbered for addressability.

## 1. Definition

### 1.1 Purpose
**Source synthesis** is the safe, extensible compile-time code-generation tier. A
**synthesizer** is given a typed declaration (and its annotation/template arguments) and
produces **cajeta source text**; the facility parses that text through the *normal* ANTLR
pipeline and injects the resulting AST so it re-enters the full type-check → borrow-check →
codegen path. The synthesizer never emits IR — it emits source that is re-checked exactly
like hand-written code.

The defining safety property: **synthesized output is re-checked exactly like hand-written
code and can only call existing, checked primitives — it cannot emit unsound IR.** This is
"leverage primitives to produce workflow," not "introduce new code." Because the output is
re-validated, this tier is the one that could eventually open to library-provided
synthesizers at low risk (deferred — see §10 [S4]).

### 1.2 Scope
- A **shared synthesis helper**: take generated cajeta source text, parse it via the
  standard `CajetaLexer`/`CajetaParser` path, and inject the resulting members or method
  body into the enclosing declaration so the full downstream pipeline runs over it.
- A **registry** mapping a trigger — an annotation (e.g. `@Einsum`, `@Logged`) or a generic
  instantiation (e.g. `Table<T>`, `Json.parse<T>`) — to a synthesizer, replacing the
  scattered `if (findAnnotation(...))` checks and the `if-else` synthesizer chain at
  `MethodTemplateInstantiator.cpp:343`.
- Two synthesis modes: **member synthesis** (inject new declarations into the enclosing
  type — the `@Logged` model) and **body synthesis** (provide the body of a
  declared-but-bodyless method — the codec / `@Einsum` model).
- A **diagnostics seam** so a synthesizer raises good compile errors against the user's
  declaration, not against generated text the user never wrote.
- **Determinism / purity** guarantees so synthesized output is reproducible.

### 1.3 Non-goals
- **Bounded-IR transforms** (`@Kernel`, `@Grad`, `@Jit`, `@Vmap`) — these are Tier B,
  emit/transform IR by fixed grammar-bounded rules, are trusted and compiler-resident, and
  are specified separately in `transform-intrinsics-spec.md`. The boundary is in §7.
- **A plugin system / library-authored compiler passes.** This spec formalizes two patterns
  the compiler *already* uses; it does not add arbitrary user-supplied logic. (Opening the
  *source* tier to libraries is a low-risk future possibility, deferred — §9.)
- **Macro-style token rewriting or hygiene rules of a general macro system.** Synthesizers
  generate whole declarations/bodies that re-parse as ordinary code; they are not textual
  splice points inside user expressions.
- **New surface syntax.** The triggers reuse Cajeta's existing annotation and template
  machinery; this facility adds no new grammar.

### 1.4 Relationship to existing constructs
- Cajeta already special-cases this behaviour twice: `@Logged` synthesizes a `static Logger`
  member by parsing an inline fragment and injecting the field (`synthesizeLoggerField`); the
  codec annotations drive `synthesizeJsonMethodSource()` / `synthesizeCsvMethodSource()` (and
  protobuf/ion/avro siblings), each generating a method body that is wrapped in a throwaway
  class, re-parsed, and the concrete `Method` extracted. This spec **unifies** those into one
  helper + one registry.
- It reuses the existing reflection surface (`Class<T>`, field enumeration — see
  `records-spec.md` §6) as the *input* a synthesizer reads when generating per-field code.
- Cajeta has **no global functions**; synthesizers generate members and bodies on existing
  classes/static methods, consistent with the language model.

### 1.5 The handler model (resolved 2026-06-23)
- **[S1] Two trigger kinds, one registry, one handler interface.** A synthesizer registers
  against either an **annotation on a declaration** (`@Einsum`, `@Logged`) or a **generic
  instantiation** (`Table<T>`, `Json.parse<T>`). Both resolve through one registry to one handler
  interface. The handler receives a **context object**: the resolved declaration signature, the
  trigger arguments (annotation literals or the concrete type args), a reflection view of
  referenced types, and a diagnostics sink.
- **[S1] Output: source *or* a structured builder (both).** A handler may return raw cajeta
  **source text** (simplest; the `@Logged`/codec path) **or** build the declaration via a
  **structured AST/builder API** (type-safe, no escaping, better error attribution). Both feed
  the same parse → check → inject pipeline; the author picks per synthesizer.
- **Stage — a 2×2.** Synthesis *mode* (**member** vs **body**) is orthogonal to *when* it runs
  (**declaration-time** vs **instantiation-time**); a synthesizer declares its cell:

  | | declaration-time | instantiation-time |
  |---|---|---|
  | **member** | `@Logged` (inject a field before layout) | `Table<T>` accessors (reflect `T`'s fields) |
  | **body** | `@Einsum` (tensor types concrete) | `Json.parse<T>` (per-`T` body) |

  Declaration-time runs before prototype/layout so synthesized members exist for resolution;
  instantiation-time runs at monomorphization when type args are known.
- **[S2] Trigger arbitration & composition.** At most **one body synthesizer per method** (a
  method has exactly one body); if an annotation trigger and an instantiation trigger both claim
  a body, the compiler errors loudly — no silent precedence. **Member synthesizers compose**:
  several may inject into one declaration (e.g. `@Logged` + `@Derive(Json)` + `@Derive(Equals)`),
  and a collision on the same synthesized member name (or with a user-declared member) is a loud
  error, not last-writer-wins.
- **[S3] Diagnostics: validate-first + trigger-span attribution.** A synthesizer validates the
  trigger args against the resolved signature and emits **user-attributed** errors *before*
  generating anything; residual errors in synthesized code are attributed to the **trigger span**,
  not generated text. No full source-map in v1 (a debug dump of generated source covers
  synthesizer bugs).

## 2. Registering a synthesizer

A synthesizer is registered against a **trigger**: an annotation on a declaration, or a
recognized generic/method-template instantiation. At the appropriate compiler stage the
facility looks up the trigger in the registry and dispatches to exactly one synthesizer
(or falls through to the declared/captured source when none matches).

**Use cases**
- **2.1** As a compiler author, when I register a synthesizer keyed to the `@Einsum`
  annotation, then a method carrying `@Einsum` dispatches to that synthesizer at instantiation
  and no `if (findAnnotation("Einsum"))` check is hand-wired into the instantiator.
- **2.2** As a compiler author, when I register a synthesizer keyed to a generic
  instantiation (`Table<T>`, or `Json.parse<T>`), then instantiating that template for a
  concrete `T` dispatches to the synthesizer with `T` available, replacing the manual
  `if-else` chain at `MethodTemplateInstantiator.cpp:343`.
- **2.3** As a compiler author, when more than one synthesizer could match a declaration,
  then the registry resolves to a single synthesizer by a defined precedence (or rejects the
  ambiguity loudly) — no silent first-match-wins scatter.
- **2.4** As a compiler author, when no registered synthesizer matches a declaration, then
  the facility leaves the declaration's existing (captured/declared) source untouched — the
  failsafe behaviour the JSON hook already relies on for unrecognized overloads.
- **2.5** As a compiler author, when I re-express the existing `@Logged` and codec
  synthesizers against the registry, then their observable behaviour is unchanged — the
  registry is a refactor of where the dispatch lives, not of what gets generated.

> **Resolved:** [S2] At most one trigger per declaration; if an annotation trigger and an
> instantiation trigger both match, the compiler errors loudly — no silent precedence (§1.5).

## 3. Member synthesis (the `@Logged` model)

A member synthesizer injects **new declarations** into the enclosing type. The facility
parses the generated declaration source, injects any required imports (a fully-qualified
static call can resolve to null in expression position, so short-name imports are injected
when otherwise unbound), and reparents the resulting members onto the target type.

**Use cases**
- **3.1** As a synthesizer author, when I emit a field declaration as source
  (`{ static Logger log = Log.defaultFor("..."); }`), then the facility parses it, type- and
  borrow-checks the initializer, and the field appears on the enclosing class as if
  hand-written.
- **3.2** As a synthesizer author, when my generated source uses short type names, then the
  facility injects the matching imports into the module only when those names are otherwise
  unbound — so a user's own same-named import wins and is never clobbered.
- **3.3** As a synthesizer author, when the enclosing type already declares a member I would
  synthesize (the user wrote their own `log`), then the facility does not double-add or
  clobber it (the `@Logged` respect-user-declaration rule, preserved).
- **3.4** As a `Table<T>` author, when I synthesize one typed column accessor per reflected
  field of record `T`, then each accessor (`price` → a `float64` column accessor) is injected
  as a member and a typo at the call site (`ticks.prce`) is a compile error, not a runtime
  lookup. *(Reflection over `T`'s fields per `records-spec.md` §6.4 is the input; the
  injected accessors are the output.)*
- **3.5** As a synthesizer author, when I inject members, then they enter the same
  downstream stages (resolution, monomorphization, codegen) as user-written members — there
  is no special-cased member that bypasses checking.

## 4. Body synthesis (the codec / `@Einsum` model)

A body synthesizer provides the **body of a declared-but-bodyless method** from the trigger's
arguments and the method's monomorphized signature — a *parameterized intrinsic* expressed as
source. The facility wraps the generated body in a throwaway class, parses it, and extracts
the concrete `Method`, reparenting it to the original declaration's parent.

**Use cases**
- **4.1** As an `@Einsum` author, when I have the contraction spec string
  (`"bhqd,bhkd->bhqk"`) and the declared signature, then I synthesize a fused contraction
  kernel as the method body — the call site (`attentionScores(q, k)`) stays clean and typed,
  no DSL string visible to callers.
- **4.2** As an `@Einsum` author, when the spec is inconsistent with the declared types
  (input label-groups don't match a parameter's rank; the output labels disagree with the
  declared return type), then I emit a compile error *before* synthesizing a body (§6) — the
  body is only generated for a spec that validates against the signature.
- **4.3** As a codec synthesizer (`Json.parse<T>`, `toBytes<T>`), when the method-template is
  instantiated for a concrete `T`, then I generate a per-`T` body that calls only existing
  checked codec primitives, and it re-parses and re-checks as ordinary code.
- **4.4** As a codec synthesizer, when an overload's parameter signature does not match a
  recognized entry point, then I decline and the method's captured failsafe source is used
  unchanged (the existing convenience-overload-delegation behaviour, preserved).
- **4.5** As a synthesizer author, when the generated body references a primitive that does
  not exist or has the wrong signature, then the failure surfaces as an ordinary
  type/borrow-check error on the synthesized code — the facility cannot route around the
  checker.

## 5. The re-check guarantee (the safety property)

This is the load-bearing requirement: synthesized source is *not* trusted; it is re-checked.

**Use cases**
- **5.1** As a language designer, when any synthesizer runs, then its output passes through
  the same parse → type-check → borrow-check → codegen pipeline as hand-written code — there
  is no path by which a synthesizer's output reaches LLVM lowering without full checking.
- **5.2** As a language designer, when a synthesizer attempts to produce code that would
  violate the borrow checker (e.g. a double-move, an aliased mutable borrow), then the
  borrow checker rejects it exactly as it would reject the same hand-written code — the
  synthesizer gains no checking exemption.
- **5.3** As a language designer, when a synthesizer emits a call, then it can only call
  **existing, already-checked primitives** — synthesis composes existing capability into a
  workflow; it does not introduce new primitive operations (those, where needed, live in
  Tier B and are specified in `transform-intrinsics-spec.md`).
- **5.4** As a security reviewer, when I audit the source-synthesis tier, then I can conclude
  it cannot emit unsound IR **by construction** — the conclusion does not depend on auditing
  each synthesizer's internal logic, only on the invariant that output re-enters the checked
  pipeline.
- **5.5** As a `Table<T>` / `@Einsum` author, when my synthesized output has a bug, then the
  worst outcome is a *rejected compile* (a checker error), never miscompiled or memory-unsafe
  emitted code.

## 6. Diagnostics from a synthesizer

A synthesizer must be able to raise *good* compile errors — and they must point at the user's
declaration and arguments, not at generated text the user never sees.

**Use cases**
- **6.1** As an `@Einsum` author, when the spec demands a rank the argument doesn't have, then
  I raise a diagnostic like *"`@Einsum` spec `'bhqd,bhkd->bhqk'` expects rank-4 inputs;
  `query` is rank-3"* — phrased in the user's terms (parameter name, declared spec).
- **6.2** As a synthesizer author, when I raise a diagnostic, then it is attributed to the
  user's source location (the annotated declaration / the instantiation site), not to the
  throwaway wrapper class or the synthesized fragment.
- **6.3** As a developer, when a *downstream* checker error occurs in synthesized code that
  is the user's fault (a malformed annotation argument), then the message helps me fix the
  declaration — generated-source line numbers are not leaked as the primary error location.
- **6.4** As a developer, when I opt into a synthesizer-output dump (an existing
  `CAJETA_DUMP_IR`-style debug switch), then I can inspect the generated source for
  diagnosis — but it is a debug aid, not the default error surface.

> **Resolved:** [S3] Validate-first (errors raised against the resolved signature, in the user's
> terms, before any body is generated) + trigger-span attribution for residual errors. No full
> source-map in v1; a debug dump of generated source covers synthesizer bugs (§1.5).

## 7. Boundary vs. Tier B (bounded-IR transforms)

Tier A and Tier B are distinct mechanisms; this spec owns Tier A only.

**Use cases**
- **7.1** As a compiler author, when a feature can be expressed as generating source that
  calls existing checked primitives, then it belongs in Tier A (this spec) — `@Einsum` body
  synthesis, `Table<T>` accessors, codec bodies, `@Logged` fields.
- **7.2** As a compiler author, when a feature must emit or rewrite IR by fixed
  grammar-bounded rules — producing operations no source-level primitive expresses (device
  lowering, a fused/differentiated backward pass) — then it belongs in Tier B
  (`transform-intrinsics-spec.md`), is trusted and compiler-resident, and is **never**
  library-pluggable.
- **7.3** As a compiler author, when `@Grad`'s backward pass *can* be delivered as
  source-synthesis over a specialized body (per `language-foundations.md` §1.6b), then it may
  ride this Tier-A facility for the source-expressible portion, with Tier-B IR used only where
  fusion demands it — the two tiers compose, but the boundary (source vs. raw IR) stays sharp.
- **7.4** As a security reviewer, when I classify a synthesizer, then its tier determines its
  trust model: Tier A is safe-by-recheck (candidate for eventual library opening); Tier B is
  trusted-core-only — the classification, not per-synthesizer audit, carries the guarantee.

## 8. Determinism and purity (reproducible builds)

**Use cases**
- **8.1** As a build engineer, when a synthesizer runs twice on the same declaration and
  arguments, then it produces byte-identical source output — deterministic, no dependence on
  iteration order, addresses, or wall-clock.
- **8.2** As a build engineer, when a synthesizer runs, then it performs no I/O and reads no
  mutable global state — its only inputs are the typed declaration, the trigger arguments, and
  the reflection view of referenced types; consistent with the handler-purity requirement in
  `language-foundations.md` §1.4.
- **8.3** As a build engineer, when two declarations would synthesize the same output, then
  generated wrapper/instance names are derived deterministically and uniquely (no collisions
  in the visitor's structure stack / canonical map) without breaking reproducibility.

## 9. Acceptance criteria (spec-level)
- A single shared helper parses generated cajeta source through the standard ANTLR pipeline
  and injects the result as members (member synthesis) or as a method body (body synthesis).
- A registry maps a trigger (annotation or generic instantiation) to one synthesizer,
  replacing the scattered `findAnnotation` checks and the `MethodTemplateInstantiator.cpp:343`
  if-else chain, with the existing `@Logged` and codec synthesizers re-expressed over it and
  behaviour unchanged.
- Synthesized output passes the full parse → type-check → borrow-check → codegen pipeline;
  there is no path to emitted code that skips checking, and a synthesizer can only call
  existing checked primitives.
- A synthesizer can raise compile diagnostics attributed to the user's declaration.
- `@Einsum` (body synthesis validated against the signature) and `Table<T>` typed
  column-accessor generation (member synthesis per reflected record field) are both
  expressible on the facility.
- Synthesizer output is deterministic and pure (reproducible builds).

## 10. Open questions (resolve at plan time)
- **[S1] — RESOLVED:** two named trigger kinds + one registry + one handler interface (context
  object); output is source *or* a structured builder (both); stage is a member/body × decl-time/
  instantiation-time 2×2 (§1.5).
- **[S2] — RESOLVED:** at-most-one trigger per declaration; two matches = loud error (§1.5).
- **[S3] — RESOLVED:** validate-first + trigger-span attribution; no full source-map in v1 (§1.5).
- **[S4] — DIRECTION SET (keep the door open):** v1 ships **compiler-internal (C++)**
  synthesizers; the **library-authored macro tier is a named post-v1 ambition**, gated on the
  **re-check invariant** as its precondition. v1's handler interface (§1.5) is deliberately
  shaped so a future cajeta-authored synthesizer plugs into the same seam. **Revisit before
  building it:** (a) **naming** — `@Macro`/`@Derive` are placeholders, to be reconsidered; (b)
  **reuse vs. new subsystem** — evaluate whether the existing **AoT / JIT** infrastructure can
  host the compile-time evaluation (i.e. *JIT-execute the synthesizer at compile time* over the
  reflection view + emitter) rather than building a separate compile-time evaluator — likely far
  cheaper if it fits.
- **[S5] Build incrementality / dependency tracking (open):** a synthesizer's output depends on
  its **reflected inputs** (a `Table<T>` accessor set depends on `T`'s fields; a `@Derive` body
  on the record's fields). Changing those inputs must invalidate and re-synthesize the dependents.
  v1 may rely on whole-module recompilation; a finer **synthesis-dependency graph** is required
  before incremental builds — and *especially* before library-authored macros (S4) — are sound.
  Pairs with the determinism/purity guarantees (§8).
- **[S6] Synthesizer memoization (plan-time):** caching synthesis results per
  trigger+monomorphized-instantiation so a repeated `Table<Tick>` is synthesized once (keyed
  deterministically per §8.3).
- Whether `@Grad`'s source-expressible backward portion is authored as a Tier-A synthesizer or
  stays wholly in Tier B — resolved jointly with `transform-intrinsics-spec.md` (§7.3).
