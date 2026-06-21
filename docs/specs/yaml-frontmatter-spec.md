# YAML Frontmatter — Specification

> Status: **ratified** (2026-06-20). Authored with the **design** skill. A focused,
> reusable build-tool facility; first consumer is **skill discovery**
> (`docs/specs/skill-discovery-spec.md`, unit D.1).

## 1. Definition

### 1.1 Purpose
A facility to parse **front-matter Markdown** — a document with an optional leading
`---`-fenced **YAML** header followed by a **Markdown** body — into (a) a structured
frontmatter value and (b) the verbatim body text.

### 1.2 Why
Skills (and other cajeta authored docs) are front-matter Markdown: a YAML metadata header
plus a Markdown body (§ skill-discovery 4.1). cajeta has a JSONC facility (`JsonC`) but no
YAML reader. This facility fills that gap with a small, self-contained, well-tested parser
covering the YAML constructs frontmatter actually uses — not a general-purpose YAML engine.

### 1.3 In-memory model
The parsed frontmatter is returned as an **`llvm::json::Value`**, so it composes with the
existing `JsonC`/`llvm::json` ecosystem and downstream code uses one value type. The body
is returned as a `std::string`, **byte-for-byte** (the facility never parses or rewrites
Markdown).

### 1.4 Non-goals
- A complete YAML 1.2 implementation (anchors/aliases, tags, multi-document streams,
  complex keys, block scalar chomping nuances beyond the documented set).
- Parsing or transforming the Markdown body.
- Serialization (YAML emit) — read-only.

## 2. Document splitting

### 2.1 Requirements
Detect a leading frontmatter fence and split header from body without parsing either.
- A document **starts** with frontmatter iff its first line (ignoring a leading UTF-8 BOM)
  is exactly `---`. The header runs until the next line that is exactly `---` (or `...`);
  the body is everything after that closing fence.
- **No leading fence** → empty frontmatter (`{}`), and the **entire input** is the body.
- **Unterminated fence** (opening `---` with no closing fence) → an error.
- Tolerate both `\n` and `\r\n` line endings; the returned body preserves the original
  bytes after the closing fence (including its line ending).

### 2.2 Use cases
- **2.2.1** As a consumer, given a doc with a `---` header, I get the header text and the
  Markdown body split at the closing fence.
- **2.2.2** As a consumer, given a doc with no frontmatter, I get `{}` and the whole input
  as the body (so plain `.md` is valid input).
- **2.2.3** As a consumer, given an opening `---` with no closing fence, I get a clear error.

## 3. YAML header parsing

### 3.1 Supported constructs
The header parser supports the frontmatter-relevant subset:
- **Comments** — `#` to end of line (and blank lines).
- **Mappings** — `key: value`, nested by indentation.
- **Scalars** — plain, single-quoted (`'...'`), and double-quoted (`"..."`); typed as
  JSON `bool` (`true`/`false`), `null` (`null`/`~`), `number` (integer/decimal), else
  `string`. A quoted scalar is always a string.
- **Sequences** — block (`- item` lines) and flow (`[a, b, c]`); items may be scalars.
- **Nesting** — mappings and sequences may nest to a reasonable depth.

### 3.2 Errors
Parse failures return an error (`llvm::Expected`) naming the **line** (and column where
meaningful) and the reason (e.g. bad indentation, unterminated quote, tab indentation).

### 3.3 Use cases
- **3.3.1** As a consumer, a header of `key: value` pairs parses to a JSON object with the
  right scalar types.
- **3.3.2** As a consumer, a `key: [a, b]` flow list and a block `- ` list each parse to a
  JSON array of strings.
- **3.3.3** As a consumer, comments and blank lines are ignored.
- **3.3.4** As a consumer, a malformed header (unterminated quote, tab indent) yields an
  error with a line number.

## 4. Public facility

### 4.1 Requirements
- A single entry point returning both halves, e.g.
  `llvm::Expected<FrontMatter> parseFrontMatter(std::string_view source)` where
  `FrontMatter { llvm::json::Value frontmatter; std::string body; }`.
- A file variant that reads from disk and includes the path in error messages.
- Deterministic: same input → same value and body, every run.

### 4.2 Use cases
- **4.2.1** As skill-discovery D.1, I call `parseFrontMatter` on a skill file and receive
  the metadata as `llvm::json::Value` plus the Markdown body verbatim.
- **4.2.2** As a consumer, an I/O or parse error is an `llvm::Error` carrying file + line
  context, never a crash.

## 5. Performance / SIMD
Frontmatter inputs are tiny (a header of a handful of lines), so a **scalar** parser is
the right call — SIMD would add complexity for sub-microsecond savings, and no mature
SIMD YAML engine exists (simdjson is JSON-only; cajeta's current JSON path is
`llvm::json`, also scalar). Decisions:
- The parser is **scalar**, but the hot byte-scans (BOM skip, fence detection, newline
  splitting in §2) are written as isolated routines so a SIMD `memchr`-style scan can drop
  in later without touching the parse logic — **if** a non-frontmatter consumer ever feeds
  it large inputs.
- Where SIMD **would** actually pay off in this effort is **not** here but in
  skill-discovery's index/fuzzy candidate generation over large key sets (spec
  skill-discovery §3.5) — tracked there, not in this facility.
