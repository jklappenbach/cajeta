# How Java structures its specification — research for the cajeta.dev spec reorg

> Research date: 2026-09-07. Purpose: pick a proven model for the curated,
> categorized specification section that will replace the auto-ingested
> `docs/specification/**` tree on cajeta.dev. Sources: JLS SE 24
> (docs.oracle.com/javase/specs/jls/se24, published 2025-02-07) and JVMS SE 24.

## 0. The problem being replaced

The site (`site/`, deploys to cajeta.dev via Cloudflare) renders the repo's
`docs/` tree **in place**: `scripts/build-manifest.mjs` scans `../docs/**/*.md`,
and the sidebar category is literally the first path segment
(`scripts/lib/extract.mjs:sectionFor`). The Specification tab is therefore the
raw internal spec tree: **136 files across 22 directories** (buildtool, codec,
collection, concurrent, debugging, embedded, error, gfx, gpu, hash, io, lang,
math, mcp, nucleo, process, reflect, sorting, time, tooling, xpu, …) — working
documents organized by *how the compiler was built*, not by *how a reader
learns the language*. That is what "not going to work anymore" means: the
public spec needs its own curated chapter list, decoupled from internal file
layout.

## 1. The headline finding: Java splits the problem into documents first

Java never tries to put everything in one categorized tree. The ecosystem is
**four documents with hard boundaries**, and each stays small because of it:

| Document | Scope | Size |
|---|---|---|
| **JLS** (Java Language Specification) | The language: syntax, types, semantics | **19 chapters** |
| **JVMS** (JVM Specification) | The runtime contract: class files, loading, instruction set | **7 chapters** |
| **API specification** (Javadoc) | The stdlib, package by package | generated, unbounded |
| **JEPs** | Evolution / design-in-progress | one doc per proposal |

This is the single most transferable lesson. Cajeta's 22 internal directories
mix all four kinds of content: `lang/` is JLS-kind, `xpu`/`gpu`/`concurrent`
runtime halves are JVMS-kind, `collection`/`io`/`math`/`time`/`hash`/`sorting`
are API-kind, and much of the rest (nucleo, gfx plans) is JEP-kind
(design-in-progress). **A sub-20-category sidebar only works if the language
specification does not try to swallow the stdlib reference and the working
specs.** Java's answer: it doesn't. The categorized chapter list below is the
*language* spec; the stdlib gets a generated per-package reference; drafts
stay out of the normative document entirely.

## 2. The JLS chapter list (SE 24 — 19 chapters, stable shape since SE 9)

| # | Chapter | What it covers | Cajeta-relevant notes |
|---|---|---|---|
| 1 | Introduction | Scope, notation, relationship to JVMS, example conventions | Where cajeta would state the normative/informative split and name companions (runtime, xpu) |
| 2 | Grammars | How to *read* the grammar notation used throughout | Tiny (~4 pages). Pays for itself: every construct chapter then embeds productions inline |
| 3 | Lexical Structure | Unicode, tokens, keywords, literals, comments | Direct analog |
| 4 | Types, Values, and Variables | Primitives, reference types, type variables, where variables live | Cajeta: primitives, classes-as-one-type, views, type parameters |
| 5 | Conversions and Contexts | Every implicit/explicit conversion, organized by *context* (assignment, invocation, casting…) | The trick worth stealing: conversions get ONE chapter instead of being smeared over every construct |
| 6 | Names | Declarations, scoping, shadowing, qualified names, accessibility | Often skipped by young languages, then regretted |
| 7 | Packages and Modules | Compilation units, package structure, module system | Cajeta: packages, imports, `.cja` archives' language-visible surface |
| 8 | Classes | Declarations, members, fields, methods, constructors, enums, records | The biggest chapter; the exemplar of internal structure (§3 below) |
| 9 | Interfaces | Interfaces + annotation types | Cajeta: interfaces, annotations, aspects would live near here |
| 10 | Arrays | Array types, creation, access, covariance | Cajeta: arrays + views/slices |
| 11 | Exceptions | Throw/catch semantics, checked exception analysis | Cajeta: errors + stack traces |
| 12 | Execution | Program lifecycle: startup, loading, init order, finalization, exit | Cajeta: JIT vs AOT lifecycle, drop chains at the language level |
| 13 | Binary Compatibility | What changes break existing binaries | Premature for cajeta v1; becomes vital once `.cja` archives circulate |
| 14 | Blocks, Statements, and Patterns | Every statement form; pattern matching | Direct analog |
| 15 | Expressions | Every expression form, evaluation order, operators | Direct analog; `#=`/`#v` passthrough lands here |
| 16 | Definite Assignment | The flow analysis: every rule for "definitely assigned before use" | The precedent for specifying cajeta's **borrow checker** as its own chapter — Java proves a flow analysis deserves a dedicated chapter rather than footnotes on every statement |
| 17 | Threads and Locks | The memory model, happens-before, synchronization | Cajeta: `async`/`scope`/`spawn` + the (deferred) memory-model protocol |
| 18 | Type Inference | The inference algorithm, applicability, incorporation | Cajeta: template deduction / wildcard resolution |
| 19 | Syntax | The complete grammar, collected in one place | Generated-feel appendix; cajeta has the ANTLR grammar to render |

JVMS chapters for the runtime companion, for reference: 1 Introduction ·
2 Structure of the JVM · 3 Compiling for the JVM · 4 The class File Format ·
5 Loading, Linking, and Initializing · 6 The Instruction Set · 7 Opcode
Mnemonics. Total: 7. (Cajeta analog: runtime layout, drop-chain ABI, the
transfer ABI's hidden flag, `.cja` format, JIT caching, xpu launch ABI.)

## 3. How a chapter is structured internally (Chapter 8 examined)

Verified against JLS SE 24 ch. 8 (Classes):

- **Top-level sections are few and noun-shaped** — 8.1 Class Declarations,
  8.2 Class Members, 8.3 Field Declarations, 8.4 Method Declarations, …,
  8.9 Enum Classes, 8.10 Record Classes. Ten sections for the biggest chapter
  in the book.
- **Grammar productions appear inline**, immediately after each section
  introduction (`ClassDeclaration:`, `FieldDeclaration:` …), in the notation
  defined once in ch. 2. The reader never leaves the section to see the syntax.
- **Nesting is 2–3 levels** (8.4.3.1 abstract methods), never deeper than 4.
- **Examples are first-class and numbered** (`Example 8.1.3-2`), typographically
  distinct from normative text, placed immediately after the rule they
  illustrate, and frequently show *what does not compile* and why.
- **Cross-references are §-dense** — every rule that depends on another names
  it (§5.2, §8.4.8). This is what makes 19 chapters navigable without a
  search box.
- **Normative vs. informative is typographically explicit** — discussion,
  motivation and examples are set off (indented/smaller) from binding rules.

## 4. Alternatives considered (and why JLS is the right model)

- **C# (ECMA-334)**: ~23 chapters — over budget, and it interleaves
  library-adjacent material (attributes, unsafe code, docs comments) that
  bloats the list. Same family as JLS otherwise.
- **Go spec**: one long page, ~20 unnumbered headings. Elegant for a small
  language; Cajeta (templates + ownership + concurrency + views + aspects) is
  closer to Java/C# in surface area than to Go.
- **Rust Reference**: explicitly *not* a spec ("informal"), organized as ~15
  book sections; the actual normative effort (Ferrocene FLS) restructured it
  chapter-style — i.e., Rust converged toward the JLS shape when it needed
  normativity.
- **Kotlin spec**: ~16 chapters, JLS-like; confirms the pattern for a
  Java-family language.

JLS wins on: proven at Cajeta's feature scale, Java-style class semantics is
Cajeta's stated kinship, exactly under the 20-category budget, and 30 years of
evidence the categories don't need renaming (chapter list unchanged in shape
since 2014; one title tweak — "and Patterns" — in 2023).

## 5. Strawman mapping for cajeta.dev (DRAFT — for the chapter-list discussion)

Not a decision; a starting point translating §1's split + §2's list. ~17
language chapters, leaving headroom under 20:

1. Introduction (scope, notation, normative map)
2. Grammar & Lexical Structure (merge JLS 2+3 — cajeta can afford one chapter)
3. Types, Values, and Variables
4. Allocation & Storage (`stack` / `heap` / `view` — cajeta's distinctive ground gets its own chapter, like nothing in Java)
5. Ownership & the Borrow Checker (the ch. 16 precedent: the flow analysis as a chapter — `=` borrows, `#` passthrough, shared state, drop chains)
6. Conversions and Contexts
7. Names, Scopes & Packages (merge JLS 6+7)
8. Classes (incl. multiple behavior inheritance, dispatch)
9. Interfaces, Annotations & Aspects
10. Templates & Wildcards (JLS 18's ground + generic declarations)
11. Arrays, Views & Slices
12. Statements & Patterns
13. Expressions (incl. `#=`, lambdas, method refs)
14. Errors & Stack Traces
15. Concurrency (`async` / `scope` / `spawn`, memory model)
16. Streams (language-adjacent: lambdas→streams→parallel terminals)
17. Execution (program lifecycle, JIT/AOT, drop timing)
18. Complete Grammar (generated appendix)

Everything else the current 22 directories hold moves to its proper document,
per §1: **Runtime & ABI companion** (JVMS-analog: transfer ABI, `.cja`,
runtime layout, xpu launch model), **Stdlib reference** (collection, io, math,
time, hash, sorting, codec, reflect, process — per-package, eventually
generated the Javadoc way), and **Design docs / work specs** (nucleo, gfx,
xpu bring-up logs — clearly labeled non-normative, or kept off the public
site entirely).

## 6. Sources

- https://docs.oracle.com/javase/specs/jls/se24/html/index.html (TOC, 19 chapters, fetched 2026-09-07)
- https://docs.oracle.com/javase/specs/jls/se24/html/jls-8.html (ch. 8 internal structure)
- https://docs.oracle.com/javase/specs/jvms/se24/html/index.html (JVMS TOC, 7 chapters)
- Site build trace: `site/wrangler.jsonc` (cajeta.dev routes), `site/scripts/build-manifest.mjs`, `site/scripts/lib/extract.mjs` (`sectionFor`), `site/src/content.config.ts` (in-place glob of `docs/`)
