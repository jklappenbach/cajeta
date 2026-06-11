# cajetadoc — Tool Implementation Plan (TDD)

> Status: **Plan / design.**
> Scope: implement `cajeta doc`, the documentation generator that turns the
> `/** … */` doc comments in cajeta source into a **hierarchical, browsable
> website organized by package** — JavaDoc in capability, **plus Markdown bodies**,
> **plus** a themeable, React-adoptable HTML output.
> Authoritative spec: **`docs/Documentation.md`** (comment syntax, tag
> catalog, processor goals, CLI, output layout). This plan is the **build plan**
> for that spec. Site integration: **`plans/site/cajeta-site-plan.md` §18 / §21**.

---

## 0. Goals, parity targets & TDD methodology

**What we are building.** A static-site documentation generator that:

1. **Walks the package hierarchy** of a cajeta source tree
   (`runtime/src/cajeta/**` is the reference corpus: 14 packages — `lang`,
   `lang/stream`, `collection`, `collection/ltm`, `codec/json`, `error`, `hash`,
   `io`, `io/file`, `threading`, `time`, `wire`, plus the XPU packages owned by a
   separate session) and **emits one page per type, one index per package**,
   mirroring the directory tree — exactly the JavaDoc model.
2. **Achieves feature parity with the JavaDoc standard doclet** — every page kind,
   every block tag, every inline tag, the A–Z index, the class-hierarchy tree, the
   "uses of", deprecated, constant-values, serialized-form, and search pages.
3. **Recognizes and preserves Markdown** inside doc comments (CommonMark + GFM),
   rendering it to HTML — the one thing JavaDoc does *not* do natively.
4. **Recognizes and emits every JavaDoc `@`-tag** (block + inline) as styled HTML
   widgets, plus the cajeta-specific structured tags (`@Borrows`, `@Moves`,
   `@Owns`, `@Drops self`, `@FiberSafe`, `@Complexity`, …).
5. **Emits themeable HTML using the latest CSS** (cascade layers, custom-property
   design tokens, `:where()` zero-specificity selectors, container queries, logical
   properties) so the output **adopts the theme/styles of a parent React site**
   (cajeta.dev: Next.js 15 + Fumadocs + Tailwind v4) when embedded, while still
   rendering as a self-contained static site when run standalone.

**Parity target — JavaDoc standard doclet (the checklist this plan must satisfy):**

- *Page kinds:* overview/index, per-package summary, per-package tree, per-type
  page, overview tree (full hierarchy), `class-use` ("Uses of X"), deprecated
  list, constant-field values, serialized form, A–Z member/type index, help,
  search, source view.
- *Type page anatomy:* signature header, type-parameter list, `extends`/
  `implements`, "All Known Subinterfaces/Implementing Classes/Subclasses",
  nested-type summary, field summary + detail, constructor summary + detail,
  method summary + detail, **inherited-members** sections, deprecation/stability
  badges, `@Since`.
- *Block tags:* `@Param` (incl. `@Param <T>` type params), `@Return`, `@Throws`/
  `@Exception`, `@See`, `@Since`, `@Deprecated`, `@Author`, `@Version`, `@Serial`,
  `@SerialData`, `@SerialField`, `@Hidden`, `@ApiNote`, `@ImplSpec`, `@ImplNote`.
- *Inline tags:* `{@Link}`, `{@LinkPlain}`, `{@Code}`, `{@Literal}`,
  `{@Value}`, `{@InheritDoc}`, `{@DocRoot}`, `{@Index}`, `{@Summary}`,
  `{@SystemProperty}`, `{@Snippet}`.
- *Doclint validation:* missing-comment, missing/extra `@Param`, missing
  `@Return`, broken reference, malformed tag — the `cajeta test --check-docs` gate.

**TDD methodology (binding on every section below).** This plan is
**test-first**. For each major section *N*:

- Subsection **N.1 (Tests)** is authored **before** **N.2 (Deliverables)**. Tests
  are committed **red** (failing/compiling-against-stubs), then code is written to
  turn them **green**, then refactored.
- HTML/Markdown output is verified by **golden-file (snapshot) tests**: a fixture
  source file → an expected rendered artifact checked into the repo; the runner
  diffs generated vs. golden. Golden files are reviewed on change, never
  auto-blessed in CI.
- The model layers (parse → doc-AST → type-graph) are verified by **unit
  assertions on the structured model**, independent of HTML, so rendering and
  semantics are tested separately.
- A section is **Done** only when **every item in N.1 passes** and **every item in
  N.3 (Acceptance criteria)** is demonstrably met.
- Implementation language: **C++ alongside the compiler** for v1 (per
  `Documentation.md`), reusing the cajeta lexer/parser front end; tests live in the
  existing C++ test harness (golden fixtures under `test/cajetadoc/`).

**Section order reflects dependency** — model before tags, tags before pages,
pages before theming, theming before integration.

### Progress-tracking protocol (how to maintain this plan)

Every line item in §1–§16 (Tests / Deliverables / Acceptance criteria) carries a
Markdown checkbox. Keep this plan as the **live source of truth** for progress —
update it in the same change that does the work. Status conventions:

- **`- [ ]` — open.** Not started or in progress.
- **`- [x]` — done.** Mark a line item done **only when it is actually
  complete**: its tests pass (for `N.1`), the artifact exists and is wired in (for
  `N.2`), or the criterion is demonstrably met (for `N.3`). Do not pre-check.
- **`- [~]` — deferred / blocked.** When an item can't proceed because it depends
  on another item, mark it `- [~]` and append a dependency reference of the form
  **`⟂ depends on <id>`** (e.g. `⟂ depends on 9.2.2`). Multiple deps:
  `⟂ depends on 5.2.1, 9.2.2`. The `<id>` is the dotted line-item id (or a section
  id like `§19` for cross-plan deps).

**Unblock-on-completion rule.** Whenever you mark a line item `- [x]`, **scan the
plan for any `- [~]` items whose `⟂ depends on` list names the just-completed
id**. For each, remove the satisfied id from its dependency list; if nothing
remains blocking it, flip it back to `- [ ]` (open) so it's eligible to start.
Record non-obvious unblocks in the commit message.

**Section roll-up.** A whole section is **Done** only when every `N.1` and `N.3`
item under it is `- [x]` (per the TDD rule above). Deferred (`- [~]`) items keep a
section open.

### Implementation status — first pass (2026-06-05)

A working vertical slice now lives in **`tools/cajetadoc/`**: the doc engine is
the `cajetadoc_core` OBJECT library, consumed by a standalone `cajetadoc` binary
**and** by the `cajeta` compiler as the **`cajeta doc` subcommand** (forwarder in
`src/cajeta/cli/DocCommand.cpp` → `cajetadoc::runCli`; dispatched in
`src/main.cpp` alongside `archive`/`build`/`dap`). Both entry points share one
implementation and emit byte-identical output. Wired into the root CMake behind
`-DCAJETA_BUILD_CAJETADOC=ON`; the engine links into the cajeta **executable
only** (not `cajeta_lib`), so it adds no recompile cost to the compiler. It
**reuses the real front end** by compiling the checked-in antlr-generated
lexer/parser (`generated/`) and linking only the antlr4 runtime — no LLVM — so it
builds independently of the compiler's heavy link graph. Doc comments are
recovered from the hidden token channel by source position (the grammar lexes
all `/* */` to `channel(HIDDEN)`; there is no separate DOC token). This lands the
first half of §15.1.1 (`cajeta doc` generates docs; `cajeta build` does not);
the remaining §15 items (publish bundling, site prebuild, token contract) stay
open.

Delivered this pass: source ingestion + declaration model (§2), doc-comment
parsing into summary/body/block-tags with diagnostics (§3), a CommonMark+GFM
Markdown subset incl. pipe tables / fenced code / heading-demotion / inline-tag
expansion (§4, partial), package-hierarchical HTML page generation with a
themeable `:where()`/cascade-layer/host-token-fallthrough stylesheet (§10/§11,
partial), structured-tag badges + JavaDoc block-tag widgets (§6/§8, partial), a
deterministic model-JSON dump for snapshots (§1), and a gtest shard (`cajetadoc`,
23 tests green incl. a byte-exact golden). Verified end-to-end on the real
`runtime/src/cajeta/**` corpus (xpu excluded per §17): 94 files parsed clean,
101 pages generated, byte-identical across runs. Checkboxes below reflect what is
actually done; cross-reference resolution (§5), inheritance graph (§9), search
(§12), linter (§13), incremental/watch (§14), `.car` ingestion, and the cajeta
code-fence highlighter remain open.

---

## 1. Test harness, fixtures & TDD scaffold

**Description.** Stand up the testing infrastructure first so every later section
is test-driven. Create the `cajetadoc` module skeleton, a fixture corpus of small
`.cajeta` files exercising each feature, a golden-file diff runner, and CI wiring.
Nothing renders yet — this section exists to make red/green possible.

**1.1 Tests**
- [x] **1.1.1** Golden-file runner: given `(fixture.cajeta, expected/)`, run the
  generator and assert byte-equal (normalized) output; on mismatch print a unified
  diff and fail.
- [ ] **1.1.2** Fixture loader test: confirm the fixture corpus is discovered and each
  fixture compiles under the cajeta front end (so fixtures can't rot).
- [ ] **1.1.3** Model-snapshot harness: assert a serialized (JSON) dump of the doc
  model against a golden JSON — the channel for testing semantics without HTML.
- [x] **1.1.4** "Empty input" smoke test: a tree with no doc comments produces stub
  index pages and exits 0.

**1.2 Deliverables**
- [x] **1.2.1** `cajeta doc` subcommand stub registered in the CLI (returns
  "unimplemented" but parses flags).
- [x] **1.2.2** `test/cajetadoc/fixtures/` corpus skeleton (one dir per feature area,
  seeded with at least `lang/String`-style and `hash/`-style samples).
- [x] **1.2.3** Golden-file + model-snapshot test runners integrated into the C++ test
  suite (new shard/label `cajetadoc`).
- [ ] **1.2.4** CI job that runs the `cajetadoc` shard on every push touching the
  compiler or fixtures.

**1.3 Acceptance criteria**
- [x] **1.3.1** `ctest`/harness discovers and runs the `cajetadoc` shard; an
  intentionally-wrong golden fails loudly with a readable diff.
- [ ] **1.3.2** All fixtures parse; a broken fixture is caught by 1.1.2.
- [x] **1.3.3** A developer can add a fixture + expected dir and have it picked up with
  no runner changes.

---

## 2. Source ingestion & declaration model

**Description.** Walk a source tree (or `.car` archive) and build the *declaration
model*: packages → types (class/interface/enum/struct/view/annotation) → members
(fields/constructors/methods), each carrying its signature, modifiers, visibility,
type parameters, source location, and the raw text of its attached `/** */` block.
This is pure structure — no comment parsing, no HTML. Reuse the compiler front end
so signatures and the doc/declaration pairing can never drift from the grammar.

**2.1 Tests**
- [ ] **2.1.1** Package discovery: a nested tree yields the correct package list with
  parent/child links; `package.cajeta` files are recognized as package docs (no
  type emitted).
- [x] **2.1.2** Doc-pairing: each `/** */` binds to the *immediately following*
  declaration; `//` and plain `/* */` comments are ignored; a `/** */` with no
  following declaration is flagged.
- [x] **2.1.3** Member enumeration: fields, constructors, methods, nested types, and
  their visibility/modifiers (`public`/`protected`/`private`, `static`, `final`)
  are captured for representative fixtures.
- [x] **2.1.4** Signature fidelity: generics (`Box<T>`), parameter ownership markers
  (`#name`), return-by-value vs. `#`-return, default args are represented exactly.
- [~] **2.1.5** ⟂ depends on 2.2.4 — Archive path: ingesting a `.car` yields the same model as ingesting its
  source (per `Documentation.md` "Input").
- [x] **2.1.6** Visibility filtering: private/internal excluded by default; flags
  re-include them.

**2.2 Deliverables**
- [x] **2.2.1** `DeclModel` data structures (Package, Type, Member, TypeParam, Param,
  SourceRef) with a stable JSON serialization for snapshot tests.
- [x] **2.2.2** Tree walker over `src/main/cajeta` honoring the `Compilation.md` layout;
  `package.cajeta` handling.
- [x] **2.2.3** Front-end binding that attaches the preceding doc block to each
  declaration.
- [ ] **2.2.4** `.car`-archive ingestion reading doc text from class metadata.

**2.3 Acceptance criteria**
- [x] **2.3.1** The full `runtime/src/cajeta/**` corpus ingests into a model with zero
  unpaired-comment surprises; counts match `find … -name '*.cajeta'`.
- [x] **2.3.2** Model JSON snapshots are stable across runs (deterministic ordering).
- [~] **2.3.3** ⟂ depends on 2.2.4 — Source-tree and equivalent-archive ingestion produce identical models.

---

## 3. Doc-comment parsing — body / block-tag / inline-tag split

**Description.** Parse the raw `/** */` text into a *doc-AST*: strip the leading
`*` gutter, separate the **summary** (first sentence) from the **body**, split the
trailing contiguous **block-tag** section (`@Param …`) from the body, and tokenize
**inline tags** (`{@Link …}`) within both. This is the lexical contract every later
tag/markdown/render section builds on.

**3.1 Tests**
- [x] **3.1.1** Gutter stripping: leading `*` and one following space removed; content
  indentation inside code fences preserved.
- [x] **3.1.2** Summary extraction: first sentence (ends at `.`/`!`/`?` + space, or
  first blank line) becomes `summary`; respects `{@Summary …}` override.
- [x] **3.1.3** Body vs. tag block: prose before the first block tag is the body;
  everything from the first `@tag` at line-start onward is parsed as tags.
- [x] **3.1.4** Block-tag tokenizing: multi-line tag bodies (a `@Param` with a list or
  code block underneath) attach to the right tag; a new `@tag` at line-start ends
  the previous one.
- [x] **3.1.5** Inline-tag tokenizing: `{@Link}`, `{@Code}`, `{@Literal}` etc. are
  extracted with correct brace-nesting and escape handling; `{@Code a > b}`
  preserves `>` literally.
- [x] **3.1.6** Malformed input: unterminated `{@Link`, unknown `@tag`, empty comment —
  produce diagnostics, not crashes.

**3.2 Deliverables**
- [x] **3.2.1** `DocComment` AST (summary, body-nodes, ordered block-tags,
  inline-tag spans) with JSON snapshot support.
- [x] **3.2.2** Gutter/summary/body/tag splitter.
- [x] **3.2.3** Inline-tag tokenizer with brace-nesting + escape rules.
- [ ] **3.2.4** Diagnostic objects (code, message, source span) surfaced to the linter
  (§13).

**3.3 Acceptance criteria**
- [ ] **3.3.1** Every `Documentation.md` example comment parses into the expected AST
  (snapshot-locked).
- [ ] **3.3.2** Summary/body boundary matches JavaDoc behavior on a parity fixture set.
- [x] **3.3.3** No panics on the malformed-comment fixtures; each yields a precise
  diagnostic.

---

## 4. Markdown rendering (CommonMark + GFM + cajeta highlighting)

**Description.** Render doc-comment bodies and tag bodies as **CommonMark with GFM
extensions** to HTML — the capability beyond JavaDoc. Tables, task lists,
strikethrough, footnotes, fenced code blocks with language tags, images resolved
against the resource tree, and headings demoted appropriately (`##` in a class doc
→ `<h3>`). `cajeta`-tagged code fences are highlighted with the **compiler's own
tokenizer** so highlighting can't drift from the grammar.

**4.1 Tests**
- [x] **4.1.1** Core CommonMark: paragraphs, bold/italic, inline code, lists, block
  quotes, links, images render to expected HTML (golden).
- [ ] **4.1.2** GFM extensions: pipe tables, task lists, strikethrough, footnotes.
- [ ] **4.1.3** Code fences: ` ```cajeta ` blocks are tokenized by the cajeta lexer and
  emitted with per-token spans; ` ```text/bash/json ` use the generic highlighter;
  unfenced indented code handled.
- [ ] **4.1.4** Heading demotion: comment `##` → `<h3>`; class-doc owns `<h2>`; anchors
  generated and unique per page.
- [ ] **4.1.5** Image/resource resolution: `![](images/x.png)` resolves against
  `src/main/resources` and the asset is copied to output.
- [x] **4.1.6** HTML-injection safety: raw HTML in comments is escaped/sanitized per
  policy (no `<script>` passthrough).

**4.2 Deliverables**
- [x] **4.2.1** Markdown→HTML renderer (CommonMark + GFM) wired to the doc-AST body
  nodes.
- [ ] **4.2.2** Cajeta code-fence highlighter bridging the compiler tokenizer to token
  spans + CSS classes.
- [ ] **4.2.3** Resource resolver + asset-copy pass.
- [ ] **4.2.4** Heading-demotion + slug/anchor generator.

**4.3 Acceptance criteria**
- [ ] **4.3.1** A "kitchen-sink" Markdown fixture matches its golden HTML.
- [ ] **4.3.2** `cajeta` snippets in docs are highlighted identically to the same code
  elsewhere in the toolchain (shared tokenizer).
- [ ] **4.3.3** No unsafe HTML escapes the sanitizer; images resolve and copy.

---

## 5. Cross-reference resolution & auto-linking

**Description.** Resolve internal references — bracket links (`[String]`,
`[String.length]`, `[Hash.identity(pointer)]`), the `#` member form
(`[String#length]`), fully-qualified names, and **bare-type auto-linking** (the
word `String` in prose links when unambiguous in import scope) — into stable
hyperlinks to the generated pages. Overload disambiguation by parameter type list.
Unresolved references degrade to plain text **and** emit a linter diagnostic.

**5.1 Tests**
- [ ] **5.1.1** Bracket forms: `[Type]`, `[Type.member]`, `[Type#member]`,
  `[pkg.Qualified]` resolve to correct page+anchor.
- [ ] **5.1.2** Overload selection: `[Hash.identity(pointer)]` picks the matching
  overload by parameter types; ambiguous form errors with candidates listed.
- [ ] **5.1.3** Auto-link: a bare `String` in prose links when one type fits scope;
  stays plain text when ambiguous or unknown; never links inside code spans.
- [ ] **5.1.4** Scope rules: resolution honors the documented declaration's import
  scope and package.
- [ ] **5.1.5** Cross-package + external: fully-qualified refs across packages link;
  refs to configured external doc sets (`--link`-style) resolve to external URLs.
- [ ] **5.1.6** Unresolved: produces plain text + a `broken-reference` diagnostic with
  source span.

**5.2 Deliverables**
- [ ] **5.2.1** Symbol table / index built from the §2 model keyed by FQN, simple name
  (per scope), and member signature.
- [ ] **5.2.2** Reference resolver covering all bracket forms + `#` + overload
  matching.
- [ ] **5.2.3** Bare-type auto-linker with ambiguity guards (skips code spans, keywords,
  shadowed names).
- [ ] **5.2.4** External link-map loader (offline cross-linking to other cajetadoc
  sites).

**5.3 Acceptance criteria**
- [ ] **5.3.1** All cross-references in the `Documentation.md` examples resolve to live
  links in generated output.
- [ ] **5.3.2** Overload disambiguation matches the documented `(pointer)` example.
- [ ] **5.3.3** Every unresolved reference is reported exactly once with location.

---

## 6. JavaDoc block-tag parity

**Description.** Recognize and render **every JavaDoc block tag** into styled HTML
widgets: the parameter table (`@Param`, including `@Param <T>` type-parameter
docs), `@Return`, the throws list (`@Throws`/`@Exception`), "See Also"
(`@See`), `@Since`, `@Deprecated` (with banner + strikethrough treatment),
`@Author`, `@Version`, the serialization tags (`@Serial`, `@SerialData`,
`@SerialField`), `@Hidden` (suppress), and the documentation-note tags
(`@ApiNote`, `@ImplSpec`, `@ImplNote`) as labeled callout blocks. Tag bodies are
Markdown (so §4 applies inside them).

**6.1 Tests**
- [ ] **6.1.1** `@Param` ↔ parameter matching: every param has exactly one tag in
  source order; missing/extra/misnamed → diagnostic; renders a parameter table.
- [ ] **6.1.2** `@Param <T>` type-parameter docs render in a type-parameter table.
- [ ] **6.1.3** `@Return` renders; absent on a non-void method → diagnostic; present on
  void → diagnostic.
- [ ] **6.1.4** `@Throws`/`@Exception`: multiple entries render a throws list; the
  exception type links via §5; synonyms treated identically.
- [ ] **6.1.5** `@See` (multiple) renders a "See Also" list with resolved links.
- [ ] **6.1.6** `@Deprecated` renders a deprecation banner, adds a strikethrough/badge,
  and feeds the deprecated-list page (§10) and the lint warning.
- [ ] **6.1.7** `@Since`, `@Author`, `@Version` render in metadata positions.
- [ ] **6.1.8** `@Serial`/`@SerialData`/`@SerialField` render into the serialized-form
  page (§10).
- [ ] **6.1.9** `@Hidden` removes the declaration from output entirely.
- [ ] **6.1.10** `@ApiNote`/`@ImplSpec`/`@ImplNote` render as three distinctly-styled
  callout blocks, in spec order.
- [ ] **6.1.11** Markdown inside a tag body (a list in `@Param`) renders correctly.

**6.2 Deliverables**
- [ ] **6.2.1** Block-tag registry mapping each tag → parser + HTML widget renderer.
- [ ] **6.2.2** Parameter / type-parameter / throws table components.
- [ ] **6.2.3** Deprecation, since/author/version, and note-callout (`@ApiNote` etc.)
  renderers.
- [ ] **6.2.4** Serialization-tag collectors feeding the serialized-form page.
- [ ] **6.2.5** `@Hidden` suppression pass.

**6.3 Acceptance criteria**
- [ ] **6.3.1** Every block tag in the §0 parity checklist renders, golden-locked.
- [ ] **6.3.2** `@Param`/`@Return`/`@Throws` correctness checks fire the right
  diagnostics (drives §13).
- [ ] **6.3.3** The `encodeHex` and `push` examples from `Documentation.md` render
  pixel-for-structure to their goldens.

---

## 7. JavaDoc inline-tag parity

**Description.** Recognize and emit **every JavaDoc inline tag** as inline HTML:
`{@Link}`/`{@LinkPlain}` (cross-ref, styled vs. plain), `{@Code}`/`{@Literal}`
(verbatim, no Markdown/HTML interpretation), `{@Value}` (constant inlining),
`{@InheritDoc}` (pull text from the overridden/implemented member), `{@DocRoot}`
(relative root for asset links), `{@Index}` (searchable index term),
`{@Summary}` (explicit summary), `{@SystemProperty}`, and `{@Snippet}` (external/
inline code snippet with region + highlighting).

**7.1 Tests**
- [ ] **7.1.1** `{@Link Type#member}` → styled cross-ref via §5; `{@LinkPlain …}` →
  unstyled link text.
- [ ] **7.1.2** `{@Code a < b}` / `{@Literal …}` render verbatim, escaping `<`,`>`,`&`,
  and suppressing Markdown inside.
- [ ] **7.1.3** `{@Value Const}` inlines the constant's value; `{@Value}` on a constant
  field inlines its own value.
- [ ] **7.1.4** `{@InheritDoc}` copies the resolved supertype/interface doc (body and/or
  specific tag context) into the override.
- [ ] **7.1.5** `{@DocRoot}` resolves to the correct relative path from any page depth.
- [ ] **7.1.6** `{@Index term}` registers a search term and renders an anchor.
- [ ] **7.1.7** `{@Summary …}` overrides first-sentence extraction (interacts with
  §3.1.2).
- [ ] **7.1.8** `{@Snippet}` renders an external or inline snippet with region
  selection + highlighting; `{@SystemProperty}` renders + indexes.

**7.2 Deliverables**
- [ ] **7.2.1** Inline-tag registry + renderers for all tags above.
- [ ] **7.2.2** `{@InheritDoc}` resolver tied to the §9 inheritance graph.
- [ ] **7.2.3** `{@Value}` evaluator reading constant initializers from the §2 model.
- [ ] **7.2.4** `{@Snippet}` loader (inline body + external file/region) with
  highlighting reuse from §4.

**7.3 Acceptance criteria**
- [ ] **7.3.1** Every inline tag in the §0 checklist renders, golden-locked.
- [ ] **7.3.2** `{@Code}`/`{@Literal}` never leak Markdown/HTML.
- [ ] **7.3.3** `{@InheritDoc}` produces the supertype text on an override fixture.

---

## 8. Cajeta-specific structured tags

**Description.** Recognize the cajeta-only structured tags from
`Documentation.md` and render them with **distinct visual treatment** (header
badges, dedicated sections) and **aggregate index pages**: ownership/lifetime
(`@Borrows`, `@Moves`, `@Owns`, `@Drops self`), concurrency (`@FiberSafe`,
`@FiberUnsafe`, `@Blocks`, `@NonBlocking`), and cost (`@Complexity O(…)`). These
also drive generated views (fiber-safe index, complexity-sortable view).

**8.1 Tests**
- [ ] **8.1.1** Ownership tags render as parameter-row badges and a method note (e.g.
  `@Owns value` annotates the `value` row).
- [ ] **8.1.2** Concurrency tags render a header badge; `@FiberSafe` members are
  collected onto the fiber-safe index page.
- [ ] **8.1.3** `@Complexity O(log n)` renders a badge and the member appears in the
  complexity-sortable view; multiple (time+space) allowed.
- [ ] **8.1.6** Ordering: cajeta-specific tags render **before** standard JavaDoc tags
  (per style guide).

**8.2 Deliverables**
- [ ] **8.2.1** Structured-tag registry + badge/section renderers.
- [ ] **8.2.2** Aggregate-page generators: fiber-safe index, complexity view.
- [ ] **8.2.4** Apply the kept semantic tags
  (`@Borrows`/`@Moves`/`@Owns`/`@Drops`/`@FiberSafe`/`@FiberUnsafe`/`@Blocks`/
  `@NonBlocking`/`@Complexity`) to the actual stdlib doc comments in
  `runtime/src/cajeta/**` where appropriate (ties to the stdlib comment review).
  Currently only `@Complexity` is applied (`Hash.cajeta`, `Object.cajeta`); the
  rest are not yet used in code.

**8.3 Acceptance criteria**
- [ ] **8.3.1** The `push(#T value)` example renders all its structured tags
  (`@Owns`, `@Complexity`, `@FiberUnsafe`) golden-locked.
- [ ] **8.3.2** Fiber-safe and complexity aggregate pages list the right members.

---

## 9. Type model — inheritance graph, nested types, inherited members

**Description.** Build the *type graph*: `extends`/`implements` edges, transitive
supertypes/superinterfaces, **known direct subtypes/implementors**, nested-type
containment, and per-type **inherited-member** resolution (which methods/fields a
type inherits, and from where, with override links). This powers the type-page
relationship sections, the overview tree (§10), and `{@InheritDoc}` (§7).

**9.1 Tests**
- [ ] **9.1.1** Supertype lists: "All Implemented Interfaces" / "All Superclasses" are
  transitive and de-duplicated.
- [ ] **9.1.2** Known-subtypes: "Direct Known Subclasses / Known Implementing Classes"
  are computed across the corpus.
- [ ] **9.1.3** Inherited members: a subclass page lists inherited (non-overridden)
  members grouped by declaring type, with links; overrides are marked.
- [ ] **9.1.4** Multiple inheritance (cajeta `MultiClassing`): behavior interfaces +
  single-state inheritance resolve without duplicate/ambiguous member rows.
- [ ] **9.1.5** Nested types: inner classes get their own page and back-link to the
  enclosing type.
- [ ] **9.1.6** Cycle/diamond safety: diamond inheritance does not double-count or loop.

**9.2 Deliverables**
- [ ] **9.2.1** Type-graph builder (edges, transitive closures, subtype reverse-index).
- [ ] **9.2.2** Inherited-member resolver with override detection.
- [ ] **9.2.3** Nested-type containment model + page linkage.

**9.3 Acceptance criteria**
- [ ] **9.3.1** A multi-level fixture hierarchy produces correct supertype, subtype, and
  inherited-member sections (snapshot).
- [ ] **9.3.2** `MultiClassing`-style fixtures resolve cleanly (no duplicate rows).
- [ ] **9.3.3** The graph feeds a correct overview tree (verified in §10).

---

## 10. Hierarchical page generation (JavaDoc page-kind parity)

**Description.** Emit the full **package-hierarchical site** — the directory tree
mirrors the package tree, exactly like JavaDoc — with every JavaDoc page kind.
This section wires the model (§2/§9), parsed comments (§3), rendered Markdown
(§4), resolved links (§5), and all tags (§6–§8) into pages.

**Page kinds (parity):** project landing/overview, per-package summary, per-package
tree, per-type page (full anatomy from §0), overview tree (whole hierarchy),
`class-use`/"Uses of X", deprecated list, constant-field values, serialized form,
A–Z type+member index, help, and the syntax-highlighted source viewer. Output
layout follows `Documentation.md` "Output structure".

**10.1 Tests**
- [x] **10.1.1** Directory mirroring: `cajeta/hash/XXHash3.cajeta` →
  `cajeta/hash/XXHash3.html`; package index at `cajeta/hash/index.html`.
- [ ] **10.1.2** Type-page anatomy: header/signature/badges, type params,
  extends/implements, nested/field/constructor/method summary **and** detail,
  inherited-members sections all present and ordered (visibility groups, then
  alpha or source per config).
- [ ] **10.1.3** Package summary lists types with first-sentence summaries; uses
  `package.cajeta` prose when present, stub when absent.
- [ ] **10.1.4** Overview tree + per-package tree render the §9 hierarchy.
- [ ] **10.1.5** `class-use` page lists references to a type from across the corpus.
- [ ] **10.1.6** Deprecated-list, constant-values, serialized-form pages aggregate the
  right members.
- [ ] **10.1.7** A–Z index covers every type + member; source viewer links each
  declaration to the right file+line (or external `--source-link`).
- [x] **10.1.8** Determinism: two runs produce byte-identical output (stable ordering,
  no timestamps in content).

**10.2 Deliverables**
- [x] **10.2.1** Page-generator pipeline (model → page intermediate → HTML emitter,
  per §11).
- [ ] **10.2.2** Each page-kind generator from the parity list.
- [ ] **10.2.3** Source-viewer generator (highlighted source + line anchors) with
  external-source-link mode.
- [ ] **10.2.4** Navigation chrome (breadcrumbs, package/type nav, prev/next) shared
  across pages.

**10.3 Acceptance criteria**
- [ ] **10.3.1** Running on `runtime/src/cajeta/**` produces a complete site whose
  directory tree mirrors the package tree, every type reachable.
- [ ] **10.3.2** Every JavaDoc page kind from §0 is generated and golden-locked on the
  fixture corpus.
- [ ] **10.3.3** Output is deterministic across runs.

---

## 11. HTML emission & themeable, React-adoptable CSS

**Description.** Emit semantic HTML5 styled with **the latest CSS** so the output
**adopts the theme/styles of a parent React site** when embedded, yet renders
standalone otherwise. Two output modes share one emitter:

- **(A) Standalone static site** — self-contained CSS, ships `cajeta-default`
  (light) + `cajeta-dark` themes with an OS-preference toggle (per
  `Documentation.md`).
- **(B) Embeddable / parent-themed** — for cajeta.dev (Next.js 15 + Fumadocs +
  Tailwind v4, `plans/site/cajeta-site-plan.md`). The emitter can also produce **MDX/
  React components** consumed by the site's `content/docs/reference/**` pipeline
  (§18 of the site plan).

**Modern-CSS theming architecture (the core technique):**
- **All markup under a single root** (`.cajetadoc`) using **low/zero-specificity
  selectors** via `:where()` / `:is()`, so a host site's styles win without
  `!important` wars.
- **CSS Cascade Layers** (`@layer cajetadoc.reset, cajetadoc.base,
  cajetadoc.components, cajetadoc.theme`) so the parent can slot its own layer
  above ours and override predictably.
- **Design-token custom properties with host fallthrough** — every color/space/
  font reads `var(--cajetadoc-x, var(--host-token, <default>))`, so when dropped
  into cajeta.dev the tokens resolve to the **Tailwind v4 `@theme` / Fumadocs /
  shadcn** caramel variables automatically; standalone, they fall back to the
  built-in theme.
- **Container queries** (`@container`) for layout that adapts to the *parent's
  column width*, not the viewport — essential for embedding inside a docs column.
- **Logical properties** (`margin-inline`, `padding-block`) for RTL/theme
  robustness; light DOM only (no Shadow DOM, no CSS-in-JS runtime) so parent styles
  cascade in.
- **Highlighting via classes**, not inline styles, so code-block colors retheme
  with the parent.

**11.1 Tests**
- [ ] **11.1.1** Specificity: generated selectors are zero/near-zero specificity
  (`:where()`); a host rule of normal specificity overrides cajetadoc styling in a
  DOM test.
- [ ] **11.1.2** Token fallthrough: with host tokens defined, computed styles use host
  values; with none, they use built-in defaults (assert via a headless
  computed-style check).
- [ ] **11.1.3** Cascade layers present and correctly ordered; a parent layer override
  wins.
- [ ] **11.1.4** Container-query layout: the same page reflows by container width, not
  viewport (snapshot at two container sizes).
- [ ] **11.1.5** Standalone mode ships valid self-contained light+dark themes; toggle
  respects OS preference.
- [ ] **11.1.6** Embeddable mode emits valid MDX/React components that mount in a React
  test renderer and inherit provided tokens.
- [ ] **11.1.7** HTML validity + a11y: pages pass an HTML validator and axe-core checks
  (landmarks, headings order, contrast against default theme, focus order).

**11.2 Deliverables**
- [x] **11.2.1** HTML emitter producing semantic, `.cajetadoc`-scoped markup.
- [x] **11.2.2** CSS architecture: cascade layers + `:where()` selectors + token
  custom-property system with host fallthrough + container queries + logical
  properties.
- [ ] **11.2.3** Built-in `cajeta-default` / `cajeta-dark` themes + OS-preference
  toggle script (standalone mode).
- [ ] **11.2.4** Embeddable MDX/React component emitter + a documented token contract
  mapping cajetadoc tokens → cajeta.dev (Tailwind/Fumadocs) tokens.
- [ ] **11.2.5** `--custom-css` / `--theme` / `--custom-logo` hooks (per spec).

**11.3 Acceptance criteria**
- [ ] **11.3.1** Dropping embeddable output into a cajeta.dev page adopts the caramel
  palette with **zero per-page CSS edits** (token fallthrough only).
- [ ] **11.3.2** Standalone output is a self-contained, themeable static site (light/
  dark) with no external CSS dependencies.
- [ ] **11.3.3** Pages validate as HTML and pass axe-core a11y checks in both themes.
- [ ] **11.3.4** A host override changes appearance without touching generated files.

---

## 12. Client-side search

**Description.** Generate a **pre-computed, client-side search index** (no backend)
matching type names, member names, field names, `{@Index}` terms, and first-
sentence summaries — JavaDoc-search parity. Scoring favors exact-name over prose
matches; ties broken by shorter name. Partition the index by top-level package for
large corpora, fetched lazily.

**12.1 Tests**
- [ ] **12.1.1** Index contents: every type/member/`{@Index}` term appears; summaries
  are searchable (typing "rate limit" finds a method whose summary mentions it).
- [ ] **12.1.2** Ranking: exact name > prefix > prose; shorter name wins ties.
- [ ] **12.1.3** Partitioning: per-package index files generated; the loader fetches
  only needed partitions.
- [ ] **12.1.4** `--no-search` disables generation; pages still build.
- [ ] **12.1.5** Search UI: keyboard nav, no-results state, links resolve to the right
  page+anchor (DOM/integration test).

**12.2 Deliverables**
- [ ] **12.2.1** Search-index builder (JSON) + optional per-package partitioning.
- [ ] **12.2.2** Client search script + UI (light-dom, themeable per §11).
- [ ] **12.2.3** Embeddable mode: expose the index so cajeta.dev's Orama search (site
  plan §8.1) can ingest reference pages instead of shipping a second box.

**12.3 Acceptance criteria**
- [ ] **12.3.1** Search over the corpus returns correct, well-ranked results
  client-side with no backend.
- [ ] **12.3.2** In embeddable mode, reference pages are discoverable via the parent
  site's search.

---

## 13. Validation linter — `cajeta test --check-docs`

**Description.** A doc-lint pass (JavaDoc `-Xdoclint` parity) usable as a CI gate:
flag undocumented public declarations, `@Param` name mismatches / missing / extra,
missing `@Return` on non-void, broken cross-references, malformed tags, and bad
`{@InheritDoc}` targets. Strictness configurable via `cajeta.toml [docs.lint]`,
default "every public declaration has a comment."

**13.1 Tests**
- [ ] **13.1.1** Missing-comment on a public declaration → finding; private exempt
  unless configured.
- [ ] **13.1.2** `@Param` mismatch/missing/extra/misordered → findings with spans.
- [ ] **13.1.3** Missing `@Return` on non-void → finding; present on void → finding.
- [ ] **13.1.4** Broken cross-reference (from §5) surfaces as a lint finding.
- [ ] **13.1.5** Malformed tag / unterminated inline tag (from §3) surfaces.
- [ ] **13.1.6** Config: strictness levels toggle which findings are errors vs.
  warnings; exit code reflects errors for CI.

**13.2 Deliverables**
- [ ] **13.2.1** Linter aggregating diagnostics from §3/§5/§6/§7 plus coverage checks.
- [ ] **13.2.2** `cajeta test --check-docs` wiring + `cajeta.toml [docs.lint]` config.
- [ ] **13.2.3** Machine-readable (JSON) + human report formats; non-zero exit on
  error-level findings.

**13.3 Acceptance criteria**
- [ ] **13.3.1** A deliberately-broken fixture set yields exactly the expected findings.
- [ ] **13.3.2** `--check-docs` fails CI on error-level findings and passes a clean tree.
- [ ] **13.3.3** Strictness is configurable and documented.

---

## 14. CLI, configuration, incremental builds, watch/serve

**Description.** Implement the `cajeta doc` CLI surface and config exactly as
`Documentation.md` specifies, plus **fast incremental builds** (rebuild only
affected pages) and `--watch`/`--serve`. Zero-config from project root reads
`cajeta.toml`, walks `src/main/cajeta`, writes `build/docs`.

**14.1 Tests**
- [ ] **14.1.1** Flag parsing for every option (`--source-root`, `--output`,
  `--theme`, `--include-private/internal/deprecated`, `--member-order`,
  `--source-link`, `--no-search`, `--no-source`, `--base-url`, `--archive`, …).
- [ ] **14.1.2** Zero-config run reads `cajeta.toml` and produces `build/docs`.
- [ ] **14.1.3** Incremental: touching one source file rebuilds only its page(s) +
  dependent index/search entries; unaffected pages are byte-identical and not
  rewritten.
- [ ] **14.1.4** `--watch` rebuilds on change; `--serve` serves the output; `--archive`
  generates from a `.car`.
- [ ] **14.1.5** Config precedence: CLI flag overrides `cajeta.toml [docs]` overrides
  default.

**14.2 Deliverables**
- [ ] **14.2.1** Full CLI per spec + `cajeta.toml [docs]` loader.
- [ ] **14.2.2** Incremental build engine (dependency tracking: source → pages →
  indexes).
- [ ] **14.2.3** `--watch` (file watcher) + `--serve` (local HTTP).

**14.3 Acceptance criteria**
- [ ] **14.3.1** Every documented flag works and is covered by a test.
- [ ] **14.3.2** Incremental rebuild touches only affected outputs (asserted via
  mtime/byte diff).
- [ ] **14.3.3** `--watch`/`--serve` and `--archive` paths function end-to-end.

---

## 15. Toolchain & site integration

**Description.** Wire `cajetadoc` into the cajeta toolchain and **cajeta.dev**.
`cajeta build` does not auto-run docs (opt-in target); `cajeta publish` bundles
`build/docs/` into the archive; the site consumes the **embeddable** output as the
generated **API reference**, sibling to the hand-written guides (site plan
§18/§21). Output lands in `site/content/docs/reference/**` via a prebuild step and
is indexed by the site's search.

**15.1 Tests**
- [ ] **15.1.1** `cajeta build` does not generate docs; `cajeta doc` does.
- [ ] **15.1.2** `cajeta publish` includes `build/docs/` in the produced `.car`
  (round-trip: publish → read docs back from archive, matching §2.1.5).
- [ ] **15.1.3** Prebuild integration: generating into `site/content/docs/reference/**`
  produces pages that render in the Next.js/Fumadocs pipeline and adopt the caramel
  theme (token fallthrough, §11).
- [ ] **15.1.4** Reference pages are picked up by the site's Orama search (§12 / site
  plan §8.1).

**15.2 Deliverables**
- [ ] **15.2.1** `cajeta publish` docs-bundling.
- [ ] **15.2.2** Site prebuild script emitting embeddable reference into
  `content/docs/reference/**` (git-ignored, generated).
- [ ] **15.2.3** Documented token-mapping contract between cajetadoc and cajeta.dev.

**15.3 Acceptance criteria**
- [ ] **15.3.1** Published archives carry browsable docs; archive-sourced generation
  reproduces them.
- [ ] **15.3.2** Reference pages render inside cajeta.dev, themed, searchable, as the
  guide-sibling "Reference" tab (site plan §21).

---

## 16. Performance, end-to-end & CI regression

**Description.** Lock in the spec's performance targets and an end-to-end
regression suite so the tool stays fast and stable as the stdlib grows.

**Targets (from `Documentation.md`):** 1K-class clean build < 5 s (single core);
10K-class < 30 s; incremental rebuild < 200 ms per affected page.

**16.1 Tests**
- [ ] **16.1.1** Benchmark fixtures (synthetic 1K- and 10K-type trees) assert wall-clock
  within targets (with margin), tracked over time.
- [ ] **16.1.2** Incremental micro-benchmark asserts < 200 ms per affected page.
- [ ] **16.1.3** End-to-end: generate the **whole `runtime/src/cajeta/**` corpus**;
  assert no errors, all pages present, links resolve, search built.
- [ ] **16.1.4** Full-site golden: a curated subset is golden-locked end-to-end (HTML +
  search index) and diffed in CI.
- [ ] **16.1.5** Determinism guard in CI (two builds → identical bytes).

**16.2 Deliverables**
- [ ] **16.2.1** Benchmark harness + tracked metrics.
- [ ] **16.2.2** End-to-end corpus test + curated full-site golden.
- [ ] **16.2.3** CI gate: `cajetadoc` shard runs unit + golden + e2e + `--check-docs`
  on the corpus.

**16.3 Acceptance criteria**
- [ ] **16.3.1** All performance targets met on reference hardware (recorded).
- [ ] **16.3.2** The whole stdlib corpus generates clean, link-valid, searchable docs in
  CI.
- [ ] **16.3.3** Any output change requires an intentional golden update (no silent
  drift).

---

## 17. Dependencies, ordering & cross-references

- **Spec:** `docs/Documentation.md` (authoritative for syntax/tags/CLI/
  output). This plan implements it test-first.
- **Site:** `plans/site/cajeta-site-plan.md` — §18 (cajetadoc → generated reference),
  §21 (guides + Tour examples + generated reference compose `/docs`), §16 (doc
  editorial), §8.1 (Orama search), §3 (caramel palette / Tailwind v4 tokens).
- **Prerequisite from the site plan:** run the **stdlib code review (§19)** and
  **comment review (§20)** *before* generating production reference, so cajetadoc
  publishes correct comments (it faithfully renders whatever it reads).
- **XPU packages** (`cajeta/xpu`, `cajeta/xpu/core`) are **owned by a separate
  session** — exclude from the reference corpus until reconciled (site plan §16
  banner). The tool must handle them, but this plan does not review their content.
- **Internal section order:** §1 → §2 → §3 → §4 → §5 → §6/§7/§8 (tags) → §9 →
  §10 → §11 → §12 → §13 → §14 → §15 → §16. Each gate is "N.1 green + N.3 met."
