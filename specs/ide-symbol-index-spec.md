# ide-symbol-index — a semantic symbol layer for the IntelliJ plugin

Status: draft (2026-07-12)
Consumed by: `ide-features-spec.md`

## 1. Definition

### 1.1 Purpose
Give the Cajeta IntelliJ/CLion plugin a semantic model of the code it already
parses: named declarations, resolvable references, and an index that answers
"where is X declared", "who references X", "who extends X", and "who calls X" —
across the open project, its dependencies, and the stdlib.

### 1.2 The problem
The plugin has a real, full-fidelity PSI tree. `CajetaParserDefinition` bridges
the canonical compiler grammar (`antlr4/CajetaParser.g4`, staged in at build time)
through `antlr4-intellij-adaptor`, with correct offsets and error recovery.

It has no semantics on top of it. `CajetaParserDefinition.kt:86` reads:

```kotlin
override fun createElement(node: ASTNode): PsiElement = ANTLRPsiNode(node)
```

Every node is an untyped `ANTLRPsiNode`. There is no `PsiNamedElement`, no
`PsiReference`, no stub index, no `FileBasedIndex`, no `LineMarkerProvider`, no
go-to-definition and no find-usages. Consumers resort to string-matching ANTLR
rule names and BFS-ing for the first `IDENTIFIER` leaf — which is exactly what
`CajetaStructureViewFactory` does today, and it only works within one file.

Every feature in `ide-features-spec.md` — refactoring, call graph, inheritance
hierarchy, gutter navigation — is blocked on this gap. None of them can be built
on an untyped tree. This spec closes it once, so the four features become
consumers rather than four separate re-inventions.

### 1.3 Where symbol truth lives
A hybrid, decided 2026-07-12:

- **The IDE resolves.** Named PSI elements, `PsiReference`s, and the index are
  built in Kotlin. This is not a preference — IntelliJ's refactoring, find-usages,
  hierarchy, and gutter extension points bind to `PsiReference` and
  `PsiNamedElement`. They cannot be driven from a JSON blob. Feature 2 forces this.
- **The compiler is the reference.** The compiler is the only authority on Cajeta
  semantics, and a second resolver written in Kotlin will drift from it. Rather
  than call the compiler per-keystroke (too slow for an editor), we pin the Kotlin
  resolver against the compiler's own exports **in tests** (§7). Drift becomes a
  failing test, not a silent wrong answer in the editor.
- **The compiler supplies what the IDE cannot see.** Stdlib source lives inside
  the compiler binary and must be extracted (§6.3).

### 1.4 Constraints
- 1.4.1 The grammar is not forked. The plugin keeps staging `antlr4/*.g4` from the
  repo root, so the PSI stays in lockstep with the compiler by construction.
- 1.4.2 No new runtime dependency on the compiler binary for editing. Typing must
  not block on a subprocess. Indexing dependency source may.
- 1.4.3 Indexing must be incremental. A keystroke reindexes one file.
- 1.4.4 Resolution must degrade, not throw. Unresolved references are normal in a
  buffer mid-edit; they annotate, they do not except.

### 1.5 Non-goals
- 1.5.1 Reimplementing type inference, overload resolution, or the borrow checker
  in Kotlin. The index resolves *names to declarations*. Where a precise answer
  needs full type analysis (overload selection among same-arity candidates), the
  index may return a candidate set, and the consumer presents it.
- 1.5.2 A language server. This is in-process IntelliJ PSI.
- 1.5.3 Semantic highlighting, completion, or inspections. They become possible;
  they are not in this spec.

## 2. Named PSI elements

Replace the blanket `ANTLRPsiNode` with typed elements for declarations, so they
carry a name and can be targeted.

**Requirements**

- 2.0.1 `createElement()` returns a typed element for each declaring rule; all
  other rules keep returning `ANTLRPsiNode`.
- 2.0.2 Declaring rules: `classDeclaration`, `interfaceDeclaration`,
  `enumDeclaration`, `recordDeclaration`, `viewDeclaration`,
  `annotationTypeDeclaration`, `methodDeclaration`, `constructorDeclaration`,
  `fieldDeclaration`, `localVariableDeclaration`, `formalParameter`,
  `typeParameter`, `enumConstant`.
- 2.0.3 Each implements `PsiNameIdentifierOwner`: `getName()`, `getNameIdentifier()`,
  `setName()` (rename depends on `setName`).
- 2.0.4 Each exposes a canonical FQN (`package.Outer.Inner`) and its `SourceRef`
  (file, line, col) so it can be matched against compiler output.
- 2.0.5 Operator declarations are named elements too. (The cajetadoc model omits
  operator overloads — `cajetadoc-model-fidelity-spec.md` §2.1 — so they cannot be
  recovered from that export and must come from PSI.)

**Use cases**

- 2.1 As a developer, when I open a `.cajeta` file, the structure view lists its
  types and members — built from named elements, not identifier BFS.
- 2.2 As a developer, when I invoke Rename on a class name, IntelliJ offers its
  standard rename dialog, because the element is a `PsiNameIdentifierOwner`.
- 2.3 As a plugin developer, when I ask a PSI element for its FQN, I get one
  without walking the tree by rule-name string.

## 3. Reference resolution

**Requirements**

- 3.0.1 `PsiReference` implementations for: type references (in `typeType`,
  `extends`, `implements`, parameter and field types, type arguments), method call
  targets, field/property access, and variable reads.
- 3.0.2 Resolution order mirrors the language: local scope → enclosing method →
  enclosing class (incl. inherited members) → imports → same package → stdlib.
- 3.0.3 A reference that cannot be resolved returns `null` (or an empty
  multi-resolve), never throws.
- 3.0.4 Ambiguous method references (same name, same arity, distinct types) resolve
  to a candidate set via `PsiPolyVariantReference`, rather than guessing.
- 3.0.5 References resolve into dependency and stdlib source once mounted (§5).

**Use cases**

- 3.1 As a developer, when I Ctrl-click a type name, I land on its declaration —
  whether it is in my project, in a dependency, or in the stdlib.
- 3.2 As a developer, when I Ctrl-click a method call, I land on the method; if the
  call is ambiguous, I get a chooser rather than a wrong jump.
- 3.3 As a developer, when I Find Usages on a field, I get its reads and writes
  across the project.
- 3.4 As a developer, when I reference a type that does not exist, the reference is
  simply unresolved; the editor does not error out or hang.

## 4. The index

Three indexed relations. Names and forward declarations come from a **stub index**
(so cross-file resolution never parses whole files); the two reverse relations come
from a `FileBasedIndex`.

**Requirements**

- 4.0.1 **Stub index** over declarations: FQN → declaration, simple name → FQN(s).
  Stubs are built without a full PSI tree, so resolution does not force-parse.
- 4.0.2 **Subclass index** (reverse hierarchy): parent FQN → direct subtypes.
  The compiler holds parent links only (`CajetaClass::superClasses`) and has no
  reverse edge anywhere; the index owns this.
- 4.0.3 **Call-site index** (reverse calls): callee key → call sites. The compiler
  resolves a callee `MethodPtr` during codegen but records nothing, so this is new.
  Needed by *both* callers-of and callees-of, so it is foundation, not a feature.
- 4.0.4 A callee key survives overloads: FQN + method name + arity (+ signature
  hash where derivable), not a bare name.
- 4.0.5 Indexes cover project source, mounted dependency source, and stdlib source
  alike, and are versioned so a grammar change invalidates them.
- 4.0.6 Incremental: editing one file reindexes that file.

**Use cases**

- 4.1 As a developer, when I ask for a type by name from anywhere, the index answers
  without parsing the files it did not need.
- 4.2 As a developer, when I ask "who extends `Shape`", I get every direct subtype
  in project, deps, and stdlib.
- 4.3 As a developer, when I ask "who calls `Shape.area`", I get the call sites, and
  the overload I asked about is not conflated with its siblings.
- 4.4 As a developer, when I edit a file, the index reflects it without a project
  rescan.

## 5. Library and stdlib source

The premise that library source must be fetched from GitHub is largely false, and
this section records why.

**Every dependency `.cja` already embeds its full `.cajeta` source.**
`Compiler.cpp:3320-3345` writes the original source bytes for every user module as
a `ClassSource` entry (`CajetaArchive.h:56`). Verified against a real cached
archive (`dev.cajeta.codec`): 48 classes, each with both a `class_bitcode` and a
`class_source` entry. `cajeta archive extract` already gets them out.

**The stdlib source is embedded in the compiler binary** (`src/CMakeLists.txt:595`
globs `runtime/src/**/*.cajeta` into the binary). It is the one thing with no way
out — there is no `cajeta` subcommand that will emit it (§6.3).

So the local path is complete, offline, and — unlike a GitHub tag — guaranteed to
be the exact source the artifact was built from. GitHub is a fallback, not the
mechanism.

**Requirements**

- 5.0.1 On dependency resolution, extract each `.cja`'s `ClassSource` entries into a
  cached source root and attach it to the IntelliJ project model as **library
  sources** for that dependency.
- 5.0.2 Extraction is lazy and cached: done on first need (index, navigation, or a
  debug stop in that library), keyed by the archive's content hash, and not redone.
- 5.0.3 Attach the stdlib source root the same way, sourced via §6.3.
- 5.0.4 Mounted library sources are indexed (§4) and resolvable (§3), but read-only.
- 5.0.5 **GitHub fallback**: for a dependency whose archive carries no
  `ClassSource` entries, offer to fetch source from its origin repository. Applies
  to a `git`-type repository entry (`GitRepository.h` already models clone URL +
  ref) and to an explicit repo coordinate. Fetch is opt-in, cached, and never
  blocks the editor.
- 5.0.6 A GitHub-fetched source tree is marked as *unverified* against the built
  artifact — the tag may not match the bitcode — and the IDE says so, rather than
  presenting it as authoritative.

**Use cases**

- 5.1 As a developer, when I Ctrl-click into a dependency's class, I see its real
  source, offline, with no download.
- 5.2 As a developer, when the debugger stops in a stdlib frame
  (e.g. `ArrayList.cajeta:183`), the editor opens that source at that line.
- 5.3 As a developer, when the debugger stops in a dependency frame, the editor
  opens the dependency's embedded source at that line.
- 5.4 As a developer, when a dependency ships no source, I am offered a fetch from
  its repository, and told the result is unverified against the built artifact.
- 5.5 As a developer, when I am offline and every dependency ships source, nothing
  in this feature touches the network.
- 5.6 As a developer, when I edit a mounted library source file, the IDE refuses —
  it is a read-only view of a built artifact.

## 6. Compiler interop

**Requirements**

- 6.0.1 No compiler subprocess on the typing path (1.4.2).
- 6.0.2 The plugin may invoke the compiler during indexing/attachment:
  `cajeta archive list --json` and `cajeta archive extract` (both exist).
- 6.0.3 **New compiler capability — stdlib source extraction.** A subcommand that
  emits the embedded stdlib `.cajeta` sources to a directory (and lists them), so
  the plugin can mount them. This is the only compiler change this spec requires.
- 6.0.4 The plugin degrades if the compiler binary is missing or too old: project
  source still indexes and resolves; library/stdlib sources simply do not mount.

**Use cases**

- 6.1 As a developer, when I have no `cajeta` binary configured, the plugin still
  gives me navigation within my own project.
- 6.2 As a developer, when I run the new stdlib-source command, I get the stdlib
  sources on disk, and the version matches the binary that emitted them.

## 7. Anti-drift: the compiler as reference

A Kotlin resolver that silently disagrees with the compiler is worse than none —
it sends you to the wrong declaration with full confidence. This is the primary
risk of the hybrid, and it gets a mechanism rather than a hope.

**Requirements**

- 7.0.1 A differential test: for a corpus of Cajeta source (the stdlib and
  `samples/tour` are the obvious bodies), extract the declaration set the plugin's
  index produces, and compare it against the compiler's own structural export
  (`cajeta doc <root> --emit-model-json`). Every declaration the compiler reports
  must be present in the index.
- 7.0.2 Known and accepted divergences are enumerated, not hidden: the doc model
  omits operator overloads and leaves `extends`/`implements` as raw unresolved
  names (`cajetadoc-model-fidelity-spec.md` §2.1, §2.2). The test asserts around
  these explicitly, so a *new* divergence fails rather than blending in.
- 7.0.3 The grammar is staged, not copied (1.4.1), so a grammar change that the PSI
  layer does not handle surfaces as a build or test failure.

**Use cases**

- 7.1 As a maintainer, when I change the language grammar, a stale PSI layer fails a
  test rather than silently mis-resolving in users' editors.
- 7.2 As a maintainer, when the plugin's index misses a declaration the compiler
  sees, CI tells me which one.

## 8. Performance and correctness

- 8.0.1 Resolution on a warm index is fast enough to run on the EDT-adjacent paths
  IntelliJ expects (Ctrl-click, gutter render). Concretely: no subprocess, no full
  reparse of unrelated files.
- 8.0.2 A cold index of a project plus its dependencies completes in the background
  without blocking the editor, with progress visible.
- 8.0.3 Indexing a mid-edit buffer with syntax errors does not corrupt the index or
  throw; the error-recovery strategy already in `CajetaErrorStrategy` applies.

**Use cases**

- 8.1 As a developer, when I open a project cold, I can type immediately; navigation
  lights up as indexing completes.
- 8.2 As a developer, when my file has a syntax error, navigation elsewhere in the
  project still works.

## 9. Deliverable

Named PSI elements, references, a three-relation index, mounted library and stdlib
sources, one new compiler subcommand, and a differential test pinning the plugin's
view of the language to the compiler's. On completion, `ide-features-spec.md` is
unblocked.
