# ide-symbol-index — a compiler-authoritative symbol layer for the IDE

Status: draft (2026-07-12, revised)
Consumed by: `ide-features-spec.md`

## 1. Definition

### 1.1 Purpose
Make the compiler export what it already knows — which name binds to which
declaration, who extends whom, who overrides what, who calls what — and have the
IntelliJ plugin present it. This gives the plugin go-to-definition, find-usages,
hierarchy, call graph, and refactoring without a second implementation of Cajeta's
semantics.

### 1.2 The problem
The plugin parses but does not understand. `CajetaParserDefinition` bridges the
canonical compiler grammar (`antlr4/CajetaParser.g4`, staged in at build time)
through `antlr4-intellij-adaptor` into a real, full-fidelity PSI tree with correct
offsets and error recovery. Then `CajetaParserDefinition.kt:86` throws the
semantics away:

```kotlin
override fun createElement(node: ASTNode): PsiElement = ANTLRPsiNode(node)
```

Every node is an untyped `ANTLRPsiNode`. There is no `PsiNamedElement`, no
`PsiReference`, no stub index, no `FileBasedIndex`, no `LineMarkerProvider`, no
go-to-definition, no find-usages. Today's structure view string-matches ANTLR rule
names and BFS-es for the first `IDENTIFIER` leaf, and works only within one file.

Every feature in `ide-features-spec.md` is blocked on this.

### 1.3 Where semantic truth lives — the compiler
**The compiler resolves. The IDE presents.** Decided 2026-07-12 after rejecting a
hybrid in which the plugin would reimplement resolution in Kotlin and be pinned
against the compiler in tests.

Two things killed the hybrid:

- **There is no oracle to pin it against.** The obvious candidate,
  `cajeta doc --emit-model-json`, *parses but does not resolve* — its own contract
  says `extends`/`implements` are "raw declared names as parsed; link resolution
  and the inheritance graph happen later"
  (`tools/cajetadoc/skills/cajetadoc-emit-model-json.md:117`). It could validate
  declarations and nothing else, leaving overload selection, override detection,
  and callee binding — exactly what features 3, 4 and 5 rest on — unguarded. A
  Kotlin resolver that disagrees with the compiler is worse than no resolver: it
  sends you to the wrong declaration with total confidence.
- **`PsiReference` does not force resolution into Kotlin.** `resolve()` must
  *return* a `PsiElement`; it does not have to *compute* one. Given the compiler's
  answer as `(useSite → declaration FQN + SourceRef)`, `resolve()` is an adapter:
  find the target file, `findElementAt(offset)`, done. The reference object lives
  in Kotlin; the semantics do not have to.

If the compiler must export its resolved view anyway to serve as an oracle, use it
directly rather than to grade a second resolver you also maintain. This is the
clangd/Kythe model rather than the rust-analyzer model, and the precedent is the
argument: a reimplemented resolver drifts, a reused frontend cannot.

### 1.4 Why this is cheap
- Every AST node already carries its position — `AbstractSyntaxNode.h:48` holds
  `sourceLine` and `sourceColumn`, populated from the ANTLR token.
- The compiler already resolves each call site's callee to a `MethodPtr` and each
  class's parents to `CajetaClass::superClasses` / `interfaces`. It records none of
  it. Emitting the export is a walk over data already computed, not a new analysis.
- The RTTI metadata already defines a signature hash (`CajetaMethodDesc::sigHash`,
  FNV-1a) — the overload key exists.
- **The plugin already runs the compiler on every edit.** `CajetaLintAnnotator` is
  an `ExternalAnnotator` (off-EDT, debounced) that shells out to
  `cajeta --lint <buffer> --diag-format=json --source-root <root> --shadow <orig>`
  per buffer. The usual objection — "you cannot put the compiler on the typing
  path" — is already disproven in this codebase. The xref export rides that same
  invocation.

### 1.5 Constraints
- 1.5.1 The grammar is not forked. The plugin keeps staging `antlr4/*.g4` from the
  repo root, so the PSI stays in lockstep with the compiler by construction.
- 1.5.2 One subprocess per edit, not two. The xref export extends the existing lint
  invocation rather than adding a second compiler round-trip.
- 1.5.3 The editor never blocks on the compiler. Export is off-EDT; a slow or
  missing compiler degrades navigation, it does not freeze typing.
- 1.5.4 Resolution must degrade, not throw. Unresolved references are normal in a
  buffer mid-edit.

### 1.6 Non-goals
- 1.6.1 Reimplementing type inference, overload resolution, or the borrow checker in
  Kotlin. The **only** semantics the IDE computes for itself are locals and
  parameters within a single method body (§4.3) — syntactic, unambiguous, and
  incapable of disagreeing with the compiler.
- 1.6.2 A language server. This is in-process IntelliJ PSI over a compiler export.
- 1.6.3 Semantic completion and inspections. They become possible; not in scope.

## 2. The compiler cross-reference export

The core of this spec. A new machine-readable export of the compiler's resolved
view of a source root, source-mapped so the IDE can tie it back to PSI offsets.

**Requirements**

- 2.0.1 A new mode emitting a cross-reference index as JSON. It carries five
  relations, all with `SourceRef` (file, line, col) on both ends:
  - **declarations** — FQN, kind, name range, signature, modifiers, and for methods
    an overload key (name + arity + `sigHash`).
  - **references** — each use site → the declaration it binds to. Covers type refs,
    method calls, field/property access, and variable reads.
  - **inheritance** — `extends` / `implements` edges as **resolved FQNs**, not raw
    declared names. (This is the gap cajetadoc cannot fill.)
  - **overrides** — method → the method it overrides.
  - **calls** — call site → resolved callee, keyed by overload.
- 2.0.2 The export is available in **lint mode**, for one file against a source
  root, so it rides the existing per-edit invocation (1.5.2, 1.4). It emits on the
  same channel as diagnostics, distinguished by kind.
- 2.0.3 The export is also available for a **whole source root**, for cold indexing
  of a project, a dependency, or the stdlib.
- 2.0.4 Emission is opt-in via a flag; a build that does not ask for it pays nothing.
- 2.0.5 A partially-broken buffer still exports what resolved. Syntax errors
  suppress the affected region, not the file.
- 2.0.6 The JSON contract is versioned and lives in `specs/schemas/`. The plugin
  refuses an unknown major version rather than misreading it.
- 2.0.7 **What lint mode carries, and what it does not.** 2.0.2 and 2.0.3 say the
  export is *available* in lint mode; they do not promise it is identical to a
  build's. Originally lint carried no `calls` and no `references[kind=field]` at
  all, because body resolution ran only inside codegen — the defect
  `xref-lint-emission-gap` closed. It now carries every relation in 2.0.1, under
  two stated limits:
  - **Unresolvable receivers yield no edge, never a guessed one.** Callee
    resolution under lint is unique-or-nothing. A receiver it cannot resolve —
    chiefly a chained generic (`xs.stream().fold(...)`, whose receiver type is an
    unsubstituted template return) — produces nothing. Measured over
    `samples/tour`: lint carries 2367 of the build's 2466 own-file call edges.
  - **Stdlib bodies are not resolved**, so edges *within* the stdlib come from a
    build, not from lint.

  The consumer-facing guarantee is therefore directional, and consumers should
  rely on this rather than on equality: lint may carry FEWER edges than a build
  of the same root, but never different ones, and every edge it emits resolves
  within its own export (2.1.2's no-dangling rule holds on both paths).
- 2.0.7 Determinism: the same input yields byte-identical output (the project
  already holds this line — see `verify-reproducible`).

**Use cases**

- 2.1 As the plugin, when I lint a buffer, I receive the diagnostics *and* the
  resolved references for that buffer from one subprocess.
- 2.2 As the plugin, when I index a dependency's extracted source, I get its
  declarations, inheritance, and call edges in one pass.
- 2.3 As the plugin, when I meet an export whose schema major version I do not know,
  I disable xref-backed features and say so, rather than resolving wrongly.
- 2.4 As a maintainer, when I add a language construct, the export covers it or the
  compiler's own tests fail — there is no second resolver to also update.

## 3. Named PSI elements (syntax only)

The IDE owns *naming*, which is syntax, and nothing beyond it. This is what rename
targets and what the gutter decorates.

**Requirements**

- 3.0.1 `createElement()` returns a typed element for each declaring rule; every
  other rule keeps returning `ANTLRPsiNode`.
- 3.0.2 Declaring rules: `classDeclaration`, `interfaceDeclaration`,
  `enumDeclaration`, `recordDeclaration`, `viewDeclaration`,
  `annotationTypeDeclaration`, `methodDeclaration`, `constructorDeclaration`,
  `fieldDeclaration`, `localVariableDeclaration`, `formalParameter`,
  `typeParameter`, `enumConstant`.
- 3.0.3 Each implements `PsiNameIdentifierOwner` — `getName()`,
  `getNameIdentifier()`, `setName()`. Rename depends on `setName`.
- 3.0.4 Each exposes the FQN and offset the export keys on, so a declaration in the
  export maps to exactly one PSI element.
- 3.0.5 Operator declarations are named elements. (Independently necessary: the
  cajetadoc model omits operator overloads entirely —
  `cajetadoc-model-fidelity-spec.md` §2.1 — which is one more reason it could never
  have been the oracle.)

**Use cases**

- 3.1 As a developer, when I open a file, the structure view lists its types and
  members from named elements, not identifier BFS.
- 3.2 As a developer, when I invoke Rename, IntelliJ offers its standard dialog
  because the target is a `PsiNameIdentifierOwner`.

## 4. Reference resolution

**Requirements**

- 4.0.1 `PsiReference` implementations for type references, method calls,
  field/property access, and variable reads. Each is an **adapter**: it looks its
  own offset up in the index, takes the target `(file, offset)`, and returns the
  PSI element there.
- 4.0.2 No scope-walking, import-resolution, or inheritance-lookup logic in Kotlin.
  Those answers come from the export (§2.0.1).
- 4.0.3 Ambiguity is reported, not guessed. Where the compiler resolves to a
  candidate set, the reference is a `PsiPolyVariantReference` and the consumer
  presents a chooser.
- 4.0.4 References resolve into dependency and stdlib source once mounted (§6).
- 4.0.5 An offset with no export entry resolves to `null` — never throws.

- 4.3 **The one local fallback.** Locals, parameters, and **type parameters**
  resolve from PSI alone, without the compiler. Each is scoped to a single
  declaration — a local and a parameter to one method body, a type parameter to the
  class or method that declares it — so each is visible in its own PSI subtree.
  They are syntactic and unambiguous, so they cannot drift from the compiler; and
  they are what keeps navigation responsive in a buffer the compiler has not
  re-seen yet. Nothing else is resolved IDE-side.

  The test of whether something belongs here is not "is it small" but **"can the
  IDE be wrong about it?"** A name whose binding depends on imports, inheritance,
  overload selection, or another file can be got wrong, and must come from the
  compiler. A name bound within the subtree it is written in cannot.

**Use cases**

- 4.1 As a developer, when I Ctrl-click a type, method, or field, I land on its
  declaration — in my project, a dependency, or the stdlib.
- 4.2 As a developer, when a call is genuinely ambiguous, I get a chooser rather
  than a confident wrong jump.
- 4.4 As a developer, when I Ctrl-click a local I declared two lines ago in a buffer
  the compiler has not re-linted, it still resolves instantly.
- 4.5 As a developer, when I reference a type that does not exist, the reference is
  unresolved and the editor carries on.

## 5. The index (storage)

The index is **cache, not authority** — a queryable local store of what the export
said, so the plugin answers navigation without re-running the compiler.

**Requirements**

- 5.0.1 Store the five relations (§2.0.1), keyed for the queries the features make:
  FQN → declaration; simple name → FQN(s); use site → declaration; declaration →
  use sites; parent → **direct subtypes**; callee → **call sites**.
- 5.0.2 The two reverse relations — subtypes and call sites — are computed by
  inverting the export on ingest. The compiler holds parent links only
  (`CajetaClass::superClasses`) and there is no reverse edge anywhere in the
  codebase.
- 5.0.3 Overload keys (name + arity + `sigHash`) are preserved end to end, so
  callers of one overload are never conflated with another's.
- 5.0.4 Backed by IntelliJ's `FileBasedIndex` so it is incremental and persistent
  across restarts.
- 5.0.5 Covers project, mounted dependency, and stdlib source alike.
- 5.0.6 Versioned by the export schema version *and* the grammar, so either changing
  invalidates it.

**Use cases**

- 5.1 As a developer, when I ask who extends `Shape`, I get every direct subtype
  across project, deps, and stdlib.
- 5.2 As a developer, when I ask who calls `Shape.area(int32)`, I do not get callers
  of `Shape.area()`.
- 5.3 As a developer, when I restart the IDE, navigation works without a full
  reindex.
- 5.4 As a developer, when I edit one file, only that file is reindexed.

## 6. Library and stdlib source

The premise that library source must be fetched from GitHub is largely false, and
this section records why.

**Every dependency `.cja` already embeds its full `.cajeta` source.**
`Compiler.cpp:3320-3345` writes the original source bytes for every user module as
a `ClassSource` entry (`CajetaArchive.h:56`). Verified against a real cached archive
(`dev.cajeta.codec`): 48 classes, each carrying both `class_bitcode` and
`class_source`. `cajeta archive extract` already gets them out.

**The stdlib source is embedded in the compiler binary** — `src/CMakeLists.txt:595`
globs `runtime/src/**/*.cajeta` into it. It is the one thing with no way out: no
subcommand emits it.

The local path is therefore complete, offline, and — unlike a GitHub tag —
guaranteed to be the exact source the artifact was built from. GitHub is a fallback,
not the mechanism.

**Requirements**

- 6.0.1 On dependency resolution, extract each `.cja`'s `ClassSource` entries into a
  cached source root and attach it to the project model as **library sources**.
- 6.0.2 Extraction and indexing are lazy and cached, keyed by the archive's content
  hash, done on first need (index, navigation, or a debug stop in that library).
  Dependencies are immutable, so this is a once-per-version cost.
- 6.0.3 **New compiler capability — stdlib source extraction.** A subcommand
  emitting the embedded stdlib sources to a directory, so the plugin can mount and
  index them. With §2, this is the second and last compiler change this spec needs.
- 6.0.4 Mounted library and stdlib sources are indexed (§5) and resolvable (§4), but
  read-only.
- 6.0.5 **GitHub fallback**: for a dependency whose archive carries no `ClassSource`
  entries, offer to fetch source from its origin repository. `GitRepository.h`
  already models clone URL + ref. Opt-in, cached, never blocks the editor.
- 6.0.6 GitHub-fetched source is marked **unverified** against the built artifact —
  the tag may not match the bitcode — and the IDE says so rather than presenting it
  as authoritative.

**Use cases**

- 6.1 As a developer, when I Ctrl-click into a dependency's class, I see its real
  source, offline, with no download.
- 6.2 As a developer, when the debugger stops in a stdlib frame (e.g.
  `ArrayList.cajeta:183`), the editor opens that source at that line.
- 6.3 As a developer, when the debugger stops in a dependency frame, the editor opens
  the dependency's embedded source at that line.
- 6.4 As a developer, when a dependency ships no source, I am offered a fetch from
  its repository and told the result is unverified.
- 6.5 As a developer, when I am offline and every dependency ships source, nothing
  here touches the network.
- 6.6 As a developer, when I try to edit mounted library source, the IDE refuses.

## 7. Freshness and degradation

The cost of compiler-authoritative resolution is that the answer can be stale or
absent. That is a design problem to solve, not a footnote.

**Requirements**

- 7.0.1 Xref for the **edited buffer** is refreshed by the same debounced lint pass
  that already runs, against the in-memory buffer (`--shadow`), so the file you are
  typing in is the freshest thing in the index.
- 7.0.2 While a refresh is in flight, the previous export remains queryable.
  Navigation gives a slightly stale answer rather than none.
- 7.0.3 A stale-but-plausible answer must not become a *wrong* one: a resolution
  whose target offset no longer holds a matching declaration is discarded rather
  than followed to whatever moved into that offset.
- 7.0.4 With no compiler configured, or one too old for the schema, xref-backed
  features (go-to-definition across files, hierarchy, call graph, gutter nav) are
  **disabled and visibly so**. Locals still resolve (§4.3); syntax highlighting,
  structure view, and folding are unaffected. The plugin never silently guesses.
- 7.0.5 Cold indexing runs in the background with visible progress and never blocks
  the editor.

**Use cases**

- 7.1 As a developer, when I type, my buffer's references stay current without a
  second compiler run beyond the lint I already pay for.
- 7.2 As a developer, when I Ctrl-click while indexing is in flight, I get the last
  known answer or an honest "not indexed yet" — never a jump to the wrong place.
- 7.3 As a developer, when I have no compiler configured, the editor still opens,
  highlights, folds, and navigates locals, and tells me why the rest is off.
- 7.4 As a developer, when I open a large project cold, I can type immediately and
  navigation lights up as indexing completes.

## 8. Performance

- 8.0.1 Navigation on a warm index does not spawn a subprocess and does not reparse
  unrelated files.
- 8.0.2 The per-edit xref export must not measurably slow the lint pass the plugin
  already runs. It is a walk over resolved data, not a new analysis (1.4).
- 8.0.3 Cold-indexing a dependency happens once per version, keyed by content hash.

**Use cases**

- 8.1 As a developer, when I Ctrl-click, it is instant.
- 8.2 As a developer, when I type, linting is no slower than it is today.

## 9. Deliverable

Two compiler changes — a versioned cross-reference export (§2) and stdlib source
extraction (§6.0.3) — plus, in the plugin: named PSI elements, reference adapters, a
persistent five-relation index, mounted library and stdlib sources, and honest
degradation when the compiler is absent. No Cajeta semantics are reimplemented in
Kotlin. On completion, `ide-features-spec.md` is unblocked.
