# Cajeta Annotation Tag Audit & Cleanup

## Summary

The 15 Cajeta-specific `@`-tags were **documentation conventions only** — defined in
`cajeta-docs/Documentation.md` as a doc-comment vocabulary, but never wired into the
compiler. No C/C++/header source parses any of them, and no doc-comment processor exists
(`cajetadoc` is only a plan: `plans/docs/cajetadoc-tool.md`). Of the 15, only `@complexity`
was ever used in real `.cajeta` source. The audit kept the semantic subset (renamed to
alpha-only CamelCase), pruned the speculative remainder, updated the spec and the
cajetadoc plan, and added a directive to apply the kept tags to real stdlib doc comments.

## Catalog

| Original tag | Origin | Where found | Used in real code? | Compiler support? | Verdict | New name |
|---|---|---|---|---|---|---|
| `@borrows` | `cajeta-docs/Documentation.md` | spec, cajetadoc plan, stale Astro copy | No | No | Keep + rename | `@Borrows` |
| `@moves` | `cajeta-docs/Documentation.md` | spec, cajetadoc plan, stale Astro copy | No | No | Keep + rename | `@Moves` |
| `@owns` | `cajeta-docs/Documentation.md` | spec, cajetadoc plan, stale Astro copy | No | No | Keep + rename | `@Owns` |
| `@drops` | `cajeta-docs/Documentation.md` | spec, cajetadoc plan, stale Astro copy | No | No | Keep + rename | `@Drops` |
| `@fiber-safe` | `cajeta-docs/Documentation.md` | spec, cajetadoc plan, stale Astro copy | No | No | Keep + rename | `@FiberSafe` |
| `@fiber-unsafe` | `cajeta-docs/Documentation.md` | spec, cajetadoc plan, stale Astro copy | No | No | Keep + rename | `@FiberUnsafe` |
| `@blocks` | `cajeta-docs/Documentation.md` | spec, cajetadoc plan, stale Astro copy | No | No | Keep + rename | `@Blocks` |
| `@nonblocking` | `cajeta-docs/Documentation.md` | spec, cajetadoc plan, stale Astro copy | No | No | Keep + rename | `@NonBlocking` |
| `@complexity` | `cajeta-docs/Documentation.md` | spec, cajetadoc plan, stale Astro copy, **`runtime/src/cajeta/hash/Hash.cajeta` (x3), `runtime/src/cajeta/lang/Object.cajeta` (x1)** | **Yes — the only applied tag** | No | Keep + rename | `@Complexity` |
| `@allocates` | `cajeta-docs/Documentation.md` | spec, cajetadoc plan, stale Astro copy | No | No | Pruned (unused/speculative) | — |
| `@no-alloc` | `cajeta-docs/Documentation.md` | spec, cajetadoc plan, stale Astro copy | No | No | Pruned (unused/speculative) | — |
| `@stability` | `cajeta-docs/Documentation.md` | spec, cajetadoc plan, stale Astro copy | No | No | Pruned (unused/speculative) | — |
| `@platform` | `cajeta-docs/Documentation.md` | spec, cajetadoc plan, stale Astro copy | No | No | Pruned (unused/speculative) | — |
| `@panics` | `cajeta-docs/Documentation.md` | spec, cajetadoc plan, stale Astro copy | No | No | Pruned (unused/speculative) | — |
| `@aspect-affects` | `cajeta-docs/Documentation.md` | spec, cajetadoc plan, stale Astro copy | No | No | Pruned (unused/speculative) | — |

## Naming convention

Kept tags are renamed to **alpha-only, fully CamelCase**: strip hyphens, capitalize each
hyphen-delimited segment, and join with no separator. Example:
`@fiber-unsafe` -> `@FiberUnsafe` (two segments, both capitalized, hyphen removed).

Judgment call: `@nonblocking` was treated as two segments, `non` + `blocking`, yielding
`@NonBlocking` (rather than `@Nonblocking`), to keep it readable and consistent with the
`@FiberSafe` / `@FiberUnsafe` pairing.

## Changes applied

### `cajeta-docs/Documentation.md` (canonical spec — origin)

- **Renamed** all 9 kept tags to CamelCase across the tag table, the `push()` example,
  prose bullets, and the style-guide ordering line.
- **Pruned** all 6 speculative tags: removed their definition rows (the last 6 rows of
  the tag table) plus dependent text — the `@allocates` line in the `push()` snippet, the
  `@stability` internal-strip prose bullet, and the `--include-internal` flag description
  (rewritten to "Include internal declarations").
- Not touched in *this* (cajeta-specific) pass: the generated filenames `fiber-safe.html`
  and `stability.html`, and the `--include-deprecated` / `--include-internal` CLI flags
  (flag names, not `@`-tags). The standard JavaDoc tags (`@param`, `@return`, `@deprecated`,
  …) were CamelCased in a **separate consistency pass** — see "JavaDoc tag CamelCasing"
  below.

### `plans/docs/cajetadoc-tool.md` (cajetadoc build plan)

- **Renamed** all 9 kept tags to CamelCase (confined to §0 and §8).
- **Pruned** all 6 speculative tags from the §0 tag list, the §8 cost/policy lists, and
  the §8.1.4/§8.1.5 tests (removed), §8.2.3 deliverable (removed), and §8.3.1/§8.3.3
  acceptance criteria. Surviving items were not renumbered.
- **New directive (§8.2.4):** added a checkbox deliverable instructing that the kept
  semantic tags be applied to real stdlib doc comments in `runtime/src/cajeta/**` where
  appropriate, tied to the stdlib comment review, noting only `@Complexity` is currently
  applied.

### `runtime/src/cajeta/hash/Hash.cajeta`

- **Renamed** `@complexity` -> `@Complexity` (3 occurrences: lines 61, 85, 122). Nothing
  pruned; no other tags present.

### `runtime/src/cajeta/lang/Object.cajeta`

- **Renamed** `@complexity` -> `@Complexity` (1 occurrence: line 74). Nothing pruned.

### `cajeta-docs-site/src/pages/docs/Documentation.md` (stale Astro copy)

- This file is a stale hand-copy of the canonical spec and is **slated for deletion** per
  site plan §14.4; it was updated here only for cross-reference consistency.
- **Renamed** all 9 kept tags to CamelCase and **pruned** all 6 speculative tags
  (table rows plus dependent prose, output-tree entries, header description, and CLI
  option references), matching the canonical spec.
- The illustrative custom-tag examples `@http-route` and `@sql-table` in Open Questions
  are not part of the rename set and were left untouched.

## JavaDoc tag CamelCasing (consistency pass)

After the cajeta-specific cleanup above, **all JavaDoc doc-comment tags were also
converted to CamelCase** so the entire doc-tag vocabulary is spelled consistently. Only
the spelling changes; every tag keeps its standard JavaDoc meaning (recorded in the spec,
`cajeta-docs/Documentation.md`).

### Mapping

| Kind | Original (JavaDoc) | New (CamelCase) |
|---|---|---|
| Block | `@param` | `@Param` |
| Block | `@return` | `@Return` |
| Block | `@throws` | `@Throws` |
| Block | `@exception` | `@Exception` |
| Block | `@see` | `@See` |
| Block | `@since` | `@Since` |
| Block | `@deprecated` | `@Deprecated` |
| Block | `@version` | `@Version` |
| Block | `@author` | `@Author` |
| Block | `@serial` | `@Serial` |
| Block | `@serialData` | `@SerialData` |
| Block | `@serialField` | `@SerialField` |
| Block | `@hidden` | `@Hidden` |
| Block | `@apiNote` | `@ApiNote` |
| Block | `@implSpec` | `@ImplSpec` |
| Block | `@implNote` | `@ImplNote` |
| Inline | `{@link}` | `{@Link}` |
| Inline | `{@linkplain}` | `{@LinkPlain}` |
| Inline | `{@code}` | `{@Code}` |
| Inline | `{@literal}` | `{@Literal}` |
| Inline | `{@value}` | `{@Value}` |
| Inline | `{@inheritDoc}` | `{@InheritDoc}` |
| Inline | `{@docRoot}` | `{@DocRoot}` |
| Inline | `{@index}` | `{@Index}` |
| Inline | `{@summary}` | `{@Summary}` |
| Inline | `{@systemProperty}` | `{@SystemProperty}` |
| Inline | `{@snippet}` | `{@Snippet}` |

Judgment call: `{@linkplain}` -> `{@LinkPlain}` (treated as `link` + `plain`, mirroring the
`@NonBlocking` decision above).

### Scope (what was converted)

Applied across the **cajeta documentation system** only — a deterministic `sed` rename
(safer than agents for a pure mechanical transform):

- **22 `.cajeta` source files** under `runtime/src/cajeta/**` (doc comments in
  `collection/**`, `hash/**`, `lang/Object`, `threading/{LockGuard,WriteGuard}`, …).
- **Doc + plan `.md`:** `cajeta-docs/Documentation.md`, `cajeta-docs/BuildTool.md`,
  `plans/docs/cajetadoc-tool.md`, `plans/site/cajeta-site-plan.md`, and the two stale Astro copies
  (`cajeta-docs-site/src/pages/docs/{Documentation,BuildTool}.md`, deletion-bound).

### Exclusions (deliberately NOT converted)

- **C++ / header comments (~27 `.cpp`/`.h` files).** This doc-tag convention is for
  **cajeta code only**, not the C/C++ compiler sources. Those `@param`/`@return` comments
  are Doxygen/JavaDoc-*style* but **unprocessed** — the project does **not** use Doxygen
  (no `Doxyfile`, no CMake Doxygen integration), so nothing consumes their casing. The
  spec's migration plan also marks the C++ side as internal compiler implementation, out
  of scope for cajeta doc tooling. (Note: most `@`-tokens in C++ files are cajeta
  *annotations* embedded in test fixtures / example code — e.g. `@Component`, `@Inject`,
  `@Kernel` — already CamelCase and not doc-comment tags.)
- **JSON spec schemas** (`cajeta-docs/specs/manifest-v1.json`, `lockfile-v1.json`). Their
  matches are `"name@version"` *strings*, not doc tags.

### Naming-collision notes (context-distinguished, intentional)

- **`@Throws`** — the renamed doc-tag now shares a spelling with cajeta's **`@Throws(...)`
  language annotation** (e.g. `@Throws(IOException.class)`, see `stdlib/Annotations.md`).
  Disambiguated by context: the annotation takes arguments and attaches to a declaration;
  the doc-tag sits inside a `/** */` block. No content was changed on the annotation.
- **`{@Value}`** (inline doc-tag) vs the **`@Value`** value-class annotation — brace-
  distinguished (`{@Value}` only ever appears braced inside a doc comment).
- **`@Deprecated`** — no cajeta `@Deprecated` *annotation* exists, so the renamed
  `@deprecated` doc-tag introduces no collision.

> Note: this is an intentional divergence from literal JavaDoc spelling (which the spec
> previously cited "for familiarity"). The trade-off — internal consistency over JavaDoc
> muscle-memory — was chosen deliberately; a clarifying sentence was added to the spec's
> JavaDoc-tag section.

## Follow-ups

- The cajetadoc plan (§8, deliverable §8.2.4) now carries a directive to apply the kept
  semantic tags (`@Borrows`, `@Moves`, `@Owns`, `@Drops`, `@FiberSafe`, `@FiberUnsafe`,
  `@Blocks`, `@NonBlocking`, `@Complexity`) to `runtime/src/cajeta/**` doc comments where
  appropriate. Currently only `@Complexity` is applied (`Hash.cajeta`, `Object.cajeta`);
  the other eight are not yet used in code.
- This tag application should happen during the **stdlib comment review** (site plan §20).
