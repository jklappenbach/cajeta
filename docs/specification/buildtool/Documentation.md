# Documentation

Specification for cajeta source code documentation. It covers the
comment syntax authors use, the conventions for what gets documented, the
tag set the doc processor understands, and the `cajeta doc`
subcommand that turns annotated source into a browsable static
website.

Cajeta's documentation system follows the JavaDoc tradition for
familiarity, since every developer coming from Java / Kotlin /
Scala / TypeScript already knows it. It allows full Markdown
inside comment bodies for formatting, so there are no XML-style
HTML tags, no `<pre>` abuse, and no nested-table escape sequences. The output is a static
HTML site themed for readability, with client-side search and
cross-reference linking.

> **Status (shipped vs. design).** A working generator ships. The
> `cajeta doc` subcommand (`src/cajeta/cli/DocCommand.cpp`, gated on
> the `-DCAJETA_BUILD_CAJETADOC=ON` build option) delegates to the
> `cajetadoc` tool under `tools/cajetadoc/`, which parses `/** */`
> doc comments, renders Markdown bodies, resolves cross-references,
> and writes a static HTML site. What is real today is the `/**
> */` comment form and the `//` / `/* */` distinction, CommonMark
> bodies, the core JavaDoc tags (`@Param`, `@Return`,
> `@Throws`/`@Exception`, `@See`, `@Since`, `@Author`, `@Version`,
> `@SerialField`), and the CLI flags listed in [`cajeta doc`
> processor → CLI](#cajeta-doc-processor). Everything else here is
> the design target and is not yet all wired up. That includes the
> full structured cajeta-specific tag treatments
> (`@FiberSafe`/`@Complexity`/`@Owns`/…), `.car`-archive input,
> themes, client-side search, `--serve`/`--watch`, `--check-docs`,
> and versioned output. Sections below are spec unless noted.

## Table of contents

1. [Comment syntax](#comment-syntax)
2. [Markdown in doc comments](#markdown-in-doc-comments)
3. [JavaDoc-style `@`-tags](#javadoc-style--tags)
4. [Cajeta-specific `@`-tags](#cajeta-specific--tags)
5. [Package-level documentation](#package-level-documentation)
6. [`cajeta doc` processor](#cajeta-doc-processor)
7. [Style guide](#style-guide)
8. [Migration plan](#migration-plan)

---

## Comment syntax

There are two comment forms in cajeta source code, and only the
first is documentation.

### Documentation block comments — `/** ... */`

A block comment that opens with `/**` (slash, asterisk, asterisk)
and closes with `*/` is a documentation comment. It documents the
declaration that follows it, which may be a class, interface,
enum, struct, view, method, constructor, field, or annotation
type.

```cajeta
/**
 * Heap-allocated immutable character sequence.
 *
 * String storage is UTF-8 internally, with a cached code-point
 * count for O(1) [count()](#count) queries. Construction is via
 * `heap String(...)` for owned heap allocation, or via the
 * [viewOf](#viewof) factory family for borrowed views over
 * existing byte buffers.
 *
 * @See [Encoding] for the supported encodings.
 * @Since 1.0
 */
public final class String implements Collection<int32> {
    ...
}
```

The leading `*` on continuation lines is optional but recommended.
The doc processor strips it during parsing. Blank lines inside the
comment are paragraph separators, and they survive into the
rendered output.

### Regular comments — `//` and `/* ... */`

`//` line comments and plain `/* ... */` block comments are
implementation notes for whoever reads the source. The doc
processor ignores them.

```cajeta
// implementation note — not rendered in cajetadoc
private int64 cachedCount = -1;     // -1 sentinel: not computed yet
```

The distinction is intentional. Documentation comments are part of
the public-facing contract, and regular comments are not.
Cajetadoc walks declarations and pairs each with the immediately
preceding `/** */` block. Any other comment style is invisible to
it.

### Why `/** */` and not `///`

Rust's `///` line-doc syntax is lighter to type, but every doc
line needs the prefix. A 20-line method
description with code blocks and lists becomes a wall of `///`s
that hurts to skim and tempts authors to leave doc paragraphs
short. JavaDoc's block form keeps the prose readable in source, so
the file reads almost like the rendered output. Combined with the
Markdown body (next section), the JavaDoc block form is the right
tradeoff for cajeta.

---

## Markdown in doc comments

The body of a doc comment is CommonMark Markdown with a few
extensions. The doc processor renders the body the same way GitHub
/ GitLab / `pulldown-cmark` would.

### Supported Markdown features

| Feature                          | Syntax                                  |
|----------------------------------|-----------------------------------------|
| Paragraphs                       | Blank lines separate                    |
| Bold / italic / strikethrough    | `**bold**` / `*italic*` / `~~strike~~`  |
| Inline code                      | `` `code` ``                            |
| Code blocks                      | ` ```cajeta ... ``` ` (with language)   |
| Ordered / unordered lists        | `1.` / `-`                              |
| Block quotes                     | `> quote`                               |
| Headings inside doc comments     | `## Heading` (rendered as `<h3>` in the output — class doc owns `<h2>`) |
| Tables                           | GFM pipe tables                          |
| Links — external                 | `[label](url)`                          |
| Links — internal (cross-reference) | `[Foo]` / `[Foo.method]` / `[Foo#method]` |
| Images                           | `![alt](path)` — path resolves against the resource tree |
| Footnotes                        | `[^name]` GFM-style                      |
| Task lists                       | `- [ ]` / `- [x]`                       |
| Auto-linking of bare type names  | A word like `String` in prose, if it matches a known type in scope, renders as a link — no `[String]` brackets needed |

Code blocks carry a language tag (` ```cajeta `, ` ```text `,
` ```bash `, ` ```json `, ...). The cajeta-tagged blocks are
syntax-highlighted in the rendered output using the same tokenizer
the compiler uses, so highlighting can't drift from the actual
grammar. Other languages use a configured highlighter, and the
default is Prism.js or equivalent.

### Cross-reference links

Internal links to other cajeta types and members use a compact
syntax that resolves against the import scope of the documented
declaration:

```
[String]                  → cajeta.lang.String
[String.count]            → cajeta.lang.String.count (static or single overload)
[String#count]            → same; the # form is JavaDoc-familiar
[cajeta.hash.Hash]        → fully qualified
[Hash.identity(pointer)]  → specific overload by parameter type list
```

Bare type names like `String` in prose auto-link if the name is
unambiguous in the current import scope. You don't need to wrap
every type reference in brackets. The processor sees `String` as a
link when one fits and as plain text when one doesn't.

---

## JavaDoc-style `@`-tags

Cajeta supports the full JavaDoc tag set, with their familiar
meanings. The tags are spelled in CamelCase (`@Param`, `@Return`,
`@Throws`, `{@Link}`, …) for consistency with cajeta's annotations
and its cajeta-specific doc tags. Only the spelling differs from
JavaDoc, and the semantics are identical.

| Tag           | Purpose                                                              |
|---------------|----------------------------------------------------------------------|
| `@Param name` | Describe a parameter. One per parameter.                            |
| `@Return`     | Describe the return value. One per method.                          |
| `@Throws T`   | Document an exception this method may throw. Multiple allowed.      |
| `@Exception T`| Synonym for `@Throws`. Either form accepted.                        |
| `@See X`      | "See also" cross-reference. Multiple allowed.                       |
| `@Since X.Y`  | Version this declaration was introduced.                            |
| `@Deprecated` | Mark as deprecated. Body explains the replacement / removal plan.   |
| `@Version`    | Class / file version metadata.                                       |
| `@Author`     | Author attribution. Multiple allowed.                               |
| `@SerialData` | Documents the serialized form of a class.                            |
| `@Serial`     | Field serialization metadata.                                        |
| `@SerialField`| Documents a Serializable field's serialized form.                   |
| `@Hidden`     | Suppresses this declaration from generated docs.                    |
| `@ApiNote`    | Authoritative API usage notes.                                       |
| `@ImplSpec`   | Implementation-required behavior (binding on subclasses).            |
| `@ImplNote`   | Implementation-details note (non-binding, can change).               |

Tag bodies are Markdown, so block elements (lists, code blocks,
tables) inside a `@Param` description work the same way as in the
body prose.

The standard layout is the doc body first (the prose summary +
details), then all `@` tags in a contiguous trailing block:

```cajeta
/**
 * Encode the given bytes as a hex string.
 *
 * The output is lowercase by default; pass [HexCase.UPPER] to switch.
 * Length grows by 2× the input — every byte produces two hex digits.
 *
 * @Param data    bytes to encode, may be empty
 * @Param case    output letter case; defaults to [HexCase.LOWER]
 * @Return        new String holding the hex representation
 * @Throws OutOfMemoryException if the output String can't be allocated
 * @Since 1.0
 * @See decode
 */
public static String encodeHex(int8[] data, HexCase case = HexCase.LOWER);
```

---

## Cajeta-specific `@`-tags

Cajeta has language properties that JavaDoc / Rustdoc don't
describe well. The doc processor recognizes these as structured
tags and renders them with distinct visual treatment (a badge in
the method header, a dedicated section in the parameter list).

| Tag                        | Purpose                                                                       |
|----------------------------|-------------------------------------------------------------------------------|
| `@Borrows name`            | The parameter is borrowed (the default). Documents the lifetime constraint.   |
| `@Moves name`              | The parameter is moved (the user wrote `#name`). Caller loses ownership.       |
| `@Owns name`               | The parameter is fully owned (heap pointer, not a borrow).                    |
| `@Drops self`              | Method may free / destruct the receiver. After this call, `this` is gone.     |
| `@FiberSafe`               | Method is safe to call concurrently from multiple fibers.                     |
| `@FiberUnsafe`             | Method is **not** safe to call concurrently. Default for mutating methods.    |
| `@Blocks`                  | Method may block the calling fiber (I/O, lock acquisition). Scheduler yields. |
| `@NonBlocking`             | Method completes without yielding. Suitable for hot critical sections.        |
| `@Complexity O(...)`       | Asymptotic time complexity. Multiple allowed (e.g. time + space).             |
| `@EntryPoint`              | The method is a package front door — a starting point users call first. Renders a badge and feeds the site-wide Entry Points index (`entry-points.html`). |

```cajeta
/**
 * Push a value onto the heap.
 *
 * Heap-order invariant is maintained — root remains min (or max,
 * depending on the comparator).
 *
 * @Param value          the element to insert
 * @Owns value           value's ownership transfers to the heap; caller can't reuse
 * @Return               nothing
 * @Complexity O(log n)
 * @FiberUnsafe          calling from multiple fibers requires external locking
 * @Since 1.0
 */
public void push(#T value);
```

The structured tags drive doc-tool features beyond just rendering:
- `@EntryPoint` methods get aggregated into the "Entry Points"
  index page (`entry-points.html`), grouped by package, as the map
  of where to start in each package. They are also exposed in
  `--emit-model-json` for tooling, and the reference-doc and tour
  coverage checks key off it.
- `@FiberSafe` declarations get aggregated into a "Fiber-safe API
  index" page in the generated docs.
- `@Complexity` tags fuel a complexity-sortable view (find all
  O(1) operations).
- `@Deprecated` triggers a lint warning at every call site.

---

## Package-level documentation

Each cajeta package may have a `package.cajeta` file at the
package root containing only a doc comment (no class declaration):

```
runtime/src/cajeta/hash/
├── package.cajeta          # package-level docs
├── Hash.cajeta
├── XXHash3.cajeta
├── SipHash.cajeta
└── ...
```

`package.cajeta` contents:

```cajeta
/**
 * # cajeta.hash
 *
 * Non-cryptographic hashing for the cajeta runtime: the algorithms
 * used by Object.hash(), the cajeta.hash.Hash utility namespace, and
 * the user-facing [XXHash3] / [RapidHash] / [SipHash] / [MD5]
 * classes.
 *
 * For cryptographic hashes (SHA-2, SHA-3, BLAKE2 / 3) and the
 * surrounding crypto primitives, see the cajeta.crypto peer library.
 *
 * ## Quick examples
 *
 * Hash bytes with the XXH3-64 algorithm (matches upstream xxhash):
 *
 * ```cajeta
 * int8[] data = ...;
 * int64 h = XXHash3.hash(data);
 * ```
 *
 * Pointer-identity hash for IdentityHashMap-style use:
 *
 * ```cajeta
 * int64 h = Hash.identity(someObject);
 * ```
 *
 * @Since 1.0
 */
package cajeta.hash;
```

The package overview renders as the package's index page in the
generated docs (`cajeta/hash/index.html`). The list of classes,
sub-packages, and their summaries gets appended automatically by
the doc processor.

If a package has no `package.cajeta` file, the doc processor
generates a stub index page listing the package's contents
without any overview prose.

---

## `cajeta doc` processor

The cajeta toolchain ships a subcommand that turns annotated
cajeta source into a static HTML site. No server is required. The
output is plain files served by any web host (Nginx, S3 static
hosting, GitHub Pages, etc.).

### Goals

- **Zero-config common case.** `cajeta doc` from the project root
  reads `cajeta.toml`, walks `src/main/cajeta/`, generates
  `build/docs/`.
- **Fast incremental builds.** Re-running on a small source change
  rebuilds only the affected pages.
- **Browsable single-page-per-class layout.** Click-through
  hierarchy, sticky table of contents, dark / light theme toggle,
  search box.
- **Client-side full-text search.** No backend; ships a
  pre-computed index loaded by JavaScript.
- **Source view.** Each method / class header links to a
  syntax-highlighted view of the source file at the right line.
- **Versioned output.** Multiple versions of the same library can
  live side by side under `/v1.0/`, `/v1.1/`, etc. The site
  defaults to the latest and provides a version-picker dropdown.
- **Theming hooks.** A `cajeta.toml` `[docs]` section overrides
  colors, logo, project metadata.

### Input

The processor reads either:

1. **A source tree**, meaning `src/main/cajeta/` plus
   `src/main/resources/`, which holds resources referenced by
   docs, e.g. `images/diagram.png` embedded via markdown image
   syntax. Discovery follows the
   [Compilation.md](Compilation.md) project layout.
2. **A `.car` archive**, where every class's docs are stored in
   the archive's class-metadata entries (see Compilation.md
   "Archive format"). This is useful for generating docs for a
   downloaded library without its source.

### Output structure

```
build/docs/
├── index.html                       Project landing page (cajeta.toml description)
├── all.html                         Alphabetical index of every type
├── packages.html                    Package tree
├── stability.html                   API stability index (stable / experimental / internal)
├── fiber-safe.html                  Generated index of @FiberSafe declarations
├── search-index.json                Pre-computed search index (client-side)
├── style.css
├── script.js
├── assets/
│   └── logo.png
├── cajeta/
│   ├── index.html                   Top-level package overview
│   ├── error/
│   │   ├── index.html               Sub-package overview
│   │   ├── Throwable.html
│   │   ├── Exception.html
│   │   ├── RecoverableException.html
│   │   └── UnrecoverableException.html
│   ├── hash/
│   │   ├── index.html
│   │   ├── Hash.html
│   │   ├── XXHash3.html
│   │   ├── Hasher.html
│   │   └── ...
│   └── ...
└── source/
    ├── cajeta/error/Throwable.cajeta.html      Syntax-highlighted source
    ├── cajeta/hash/Hash.cajeta.html
    └── ...
```

Each class page contains:

- **Header.** Class name + signature + stability badge + fiber-
  safety badge + `@Since`.
- **Overview prose**, the doc comment's body, Markdown-rendered.
- **Type parameter list** (for templates).
- **Implements / extends / annotations.**
- **Constructors**, each with its full signature, doc, parameter
  table, throws, examples.
- **Methods**, with the same shape, grouped by visibility (public
  → protected → internal) and within each group alphabetically by
  default. The order is configurable to source-order via a `[docs]
  member-order = "source" | "alpha"` setting.
- **Fields.**
- **Inner classes**, where each gets its own page and this one
  links to them.
- **Source link**, which opens the syntax-highlighted source
  viewer at the class declaration.

### CLI

**Shipped today** (`tools/cajetadoc/src/Cli.cpp`). The source root
is a positional argument, and `cajeta doc <source-root> …`
forwards verbatim to the `cajetadoc <source-root> …` form:

```
cajeta doc <source-root> [options]

Options:
  -o, --output <dir>       Output directory (default: build/docs).
  --emit-model-json        Print the declaration model as JSON and exit.
  --include-private        Include private declarations (default: off).
  --include-internal       Include package-private/internal declarations (default: off).
  --exclude-dir <name>     Skip a directory by name (repeatable).
  --project-title <s>      Header title (default: Cajeta).
  --project-version <s>    Header version, shown as v<s>.
  --date-published <s>     Header publish date.
  --project-license <s>    Header license type.
  --project-icon <url>     Header icon (default: built-in placeholder).
  --render-md              Debug helper: render Markdown from stdin to HTML.
```

**Planned (design target, not yet implemented):** the remaining
flags are the intended fuller CLI. They are `--archive` (.car
input), `--theme` / `--custom-css` / `--custom-logo` /
`--base-url`, `--include-deprecated`, `--include-tests`,
`--member-order=alpha|source`, `--source-link`, `--no-search`,
`--no-source`, `--serve`, `--watch`, and `-v/--verbose`.
Source-root / project-name / version are read from CLI args today.
Reading them from `cajeta.toml` defaults is also planned.

### Search

A pre-computed search index (JSON, generated at doc-build time)
gets loaded by the page's JavaScript. Search is client-side, and
typing in the search box matches:

- Type names (full and short)
- Method names
- Field names
- The first paragraph of the doc summary, for natural-language
  queries, so typing "rate limit" finds methods whose summary
  mentions rate limiting

Match scoring favors exact name matches over prose matches. Ties
are broken by name length, and shorter ranks higher.

For large libraries the index can be partitioned by package (one
JSON file per top-level package) to keep individual file sizes
manageable. The JS loader fetches packages lazily as needed.

### Theming

The default theme ships in two variants, light (`cajeta-default`)
and dark (`cajeta-dark`), with a JavaScript toggle that respects
the user's OS preference. Themes are pure CSS with no template
overrides, so customization is `--custom-css <file>` with
overrides on the theme's named CSS variables:

```css
:root {
  --color-bg:           #ffffff;
  --color-fg:           #1a1a1a;
  --color-accent:       #0066cc;
  --color-keyword:      #af00db;
  --color-string:       #008080;
  --color-comment:      #6b7280;
  --color-codeblock-bg: #f4f4f5;
  --font-body:          system-ui, -apple-system, sans-serif;
  --font-code:          ui-monospace, "SF Mono", Consolas, monospace;
  --max-content-width:  72ch;
}
```

A future RFC may add template-level customization (HTML
overrides), but v1 is CSS-only. That keeps the doc-build pipeline
self-contained.

### Integration with the cajeta toolchain

- `cajeta build` does NOT run `cajeta doc` automatically. Docs are
  an opt-in build target, separate from compile.
- `cajeta publish` (when the registry story lands) bundles
  `build/docs/` into the published archive so registries can
  serve docs alongside the artifact.
- `cajeta test` runs an optional `--check-docs` flag that
  validates that every public declaration has a doc comment, all
  `@Param` tags match actual parameter names, and there are no
  broken cross-references. It is useful as a CI gate.

### Performance targets

- 1K-class codebase: under 5 seconds on a single core for a clean
  build.
- 10K-class codebase: under 30 seconds.
- Incremental rebuilds: under 200ms per affected page.

The doc processor is implemented in cajeta itself once cajeta is
mature enough, using the cajeta parser to read source and a
Markdown library (`cajeta.text.markdown`) to render prose. Before
the cajeta bootstrap, the v1 implementation is in C++ alongside
the compiler, exposed via the `cajeta doc` subcommand.

---

## Style guide

These are conventions for writing cajeta doc comments. They are
not enforced by tooling yet, and reviewers may push back on
violations.

**First sentence is the summary.** The processor extracts it for
the type-index page and the search-result snippet. Keep it
self-contained, because readers might never see the rest.

```cajeta
/**
 * Immutable, encoding-aware character sequence.
 *
 * Long-form description...
 */
```

Not:

```cajeta
/**
 * This class represents a string. The string is immutable.
 * Strings hold characters encoded in UTF-8 internally.
 * ...
 */
```

**Use second person ("you") for prose; imperative voice for tags.**
Doc comments speak to a future caller.

```
- "Pass [HexCase.UPPER] to switch to uppercase letters."   (good)
- "The user can pass HexCase.UPPER..."                     (avoid)
```

**Document every public declaration.** Internal / private members
are optional but encouraged. The `cajeta test --check-docs`
linter enforces public-declaration coverage.

**Markdown sparingly.** Headings inside a doc comment are usually
unnecessary, and a paragraph break is enough. Reserve `##` for
multi-section discussions on big-surface classes (`String`,
`HashMap`).

**Link bare type names freely.** Auto-linking does the right thing
in the common case. Use explicit `[link]` syntax only for cross-
references that fail to auto-resolve (overloaded methods,
ambiguous short names).

**One `@Param` per parameter, in source order.** Don't skip
parameters with "obvious" meanings, because the table reads better
when every column is filled.

**`@Return` is mandatory except for `void` methods.** Even if the
return is obvious from the type, document the meaningful range or
edge cases (null return, magic sentinel values, etc.).

**`@Throws` for every exception type the method may throw,
including indirect ones from called methods if they propagate
unchanged.**

**Cajeta-specific tags first** (`@FiberSafe`, `@Complexity`,
`@Owns`, `@Drops self`), then the standard JavaDoc tags. They
render in that order in the output.

---

## Migration plan

The codebase mixes legacy comment styles (`//` line comments,
plain `/* */` blocks) with what should become documentation
comments (`/** */`). Migration is two steps:

1. **Cajeta source files (`runtime/src/cajeta/**.cajeta`).**
   Convert to `/** */` for declaration-attached documentation
   immediately. The file count is small, and the changes are few.
2. **C++ compiler / runtime source (`src/**.cpp,h`,
   `runtime/native/cajeta_runtime.c`).** Convert
   opportunistically. When touching a file, upgrade its
   function-header `// ...` comments to `/** */` blocks. Doc
   tooling for the C++ side is out of scope, because the C++ code
   is internal compiler implementation and is not user-facing API.
   The convention is just for source readability. There is no
   bulk-conversion commit, and the codebase migrates over time as
   files get touched.

The `cajeta doc` processor only reads cajeta source, so the C++
side is invisible to it regardless.

---

## Open questions

- **Multi-version doc sites.** Single output directory per
  build, or scaffold a `/v1.0/`, `/v1.1/` tree that
  accumulates across builds? The lean is a per-build single tree,
  with a separate `docs-aggregator` tool handling multi-version
  sites for projects that need them.
- **Doc-comment validation strictness.** How aggressively should
  `cajeta test --check-docs` enforce? Required tags by visibility
  (every public method needs `@Param` + `@Return` + `@Throws`)?
  Or just "every public declaration has *some* doc comment"?
  The lean is configurable per-project via `cajeta.toml
  [docs.lint]`, defaulting to "every public declaration has a
  comment."
- **Custom tags.** Library authors might want their own tags
  (`@http-route`, `@sql-table`). Allow registering custom tags
  via `cajeta.toml`? The lean is yes, opt-in. It renders as a
  labeled badge / section like the built-in cajeta-specific tags.
- **Search backend.** Client-side full-text works for libraries
  up to ~50K declarations. Past that, the search index gets
  unwieldy. Defer the "real search backend" question until a
  cajeta project hits that scale.
- **Code execution in docs.** Spec says "no concern about code
  in comments executing as tests." That is reasonable for v1.
  Revisit it if user demand emerges, since drift between docs and
  behavior is the big motivator.
- **Doc preview during authoring.** `cajeta doc --watch` rebuilds
  on save. Is that enough, or do we want a live-preview Markdown
  editor mode? The lean is that --watch is enough, and live
  preview is editor-vendor territory.
