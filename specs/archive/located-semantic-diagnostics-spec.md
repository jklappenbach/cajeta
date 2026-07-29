# Located semantic diagnostics (Phase 1) — spec

## 1. Definition

### 1.1 Purpose
Attach a source location (file / line / column) to **semantic** diagnostics so the IDE
places the squiggly on the offending token instead of at line 1. Semantic errors remain
single-shot (throw-abort) — surfacing *multiple* semantic errors at once is Phase 2.

### 1.2 Background
Semantic errors are `throw cajeta::Exception(message, errorId)` with **no location**; the
top-level handler (`src/main.cpp`) emits them location-less, so under
`--diag-format=json` the editor gets `line`/`column` = null and lands the squiggly on
line 1 (`CajetacRunner.rangeFor` falls back to the first non-blank line). The location
already exists: `AbstractSyntaxNode` stores `sourceLine` (1-based) and `sourceColumn`
(0-based) from the node's token (`AbstractSyntaxNode.h:53-57`, getters at 65/69), and the
active module's path is `CajetaModule::getActiveModule()->getSourcePath()`. This phase
plumbs that existing location into the diagnostic.

### 1.3 Scope
- `cajeta::Exception` carries optional `file` / `line` / `column` (1-based line **and**
  column; empty/`≤0` ⇒ none).
- A convenience to throw *with* the current node's location — e.g. an
  `AbstractSyntaxNode::locatedException(msg, id)` / free helper that stamps
  `getActiveModule()->getSourcePath()`, `getSourceLine()`, `getSourceColumn() + 1`.
- The top-level handler emits the exception's location (`emitJsonDiagnostic(..., file,
  line, column)` in json; `file:line:col: id: msg` in text).
- Migrate the **flagship** semantic diagnostic (unresolved type,
  `CAJETA_ERROR_UNRESOLVED_TYPE`) plus an initial set of common editor-facing semantic
  errors to carry location.

### 1.4 Non-goals
- **Multiple diagnostics / collect-and-continue** — Phase 2; errors still abort on the
  first.
- **Migrating all ~228 throw sites** — incremental; un-migrated sites keep emitting with
  no location (exactly as today), so nothing regresses.
- **Syntax diagnostics** — already located; unchanged.

## 2. Diagnostic location model

### 2.1
As the compiler, when a migrated semantic error is thrown from an AST node, then the
`Exception` carries the node's `file` (active module source path), `line` (1-based), and
`column` (1-based = `getSourceColumn() + 1`).

### 2.2
As the compiler, when an un-migrated semantic error is thrown, then the `Exception`
carries no location and is emitted location-less — identical to today.

## 3. Emission

### 3.1
As the IDE (`--diag-format=json`), when a located semantic error occurs, then the NDJSON
diagnostic has non-null `file`/`line`/`column`, and the plugin places the annotation at
that token (via the existing `parseDiagnostics`/`rangeFor`).

### 3.2
As a CLI user (text mode), when a located semantic error occurs, then the human line
includes `file:line:column` before the id/message; un-migrated errors print as today.

### 3.3
As a consumer, a clean compile still emits nothing; exit codes are unchanged.

## 4. Migration set

### 4.1
The flagship: **unresolved type** (`CAJETA_ERROR_UNRESOLVED_TYPE`) carries the reference
token's location.

### 4.2
An initial set of the most common editor-facing semantic errors (e.g. unknown
method/member, invalid assignment, type mismatch) carries location, each via the same
helper — establishing the pattern for migrating the remaining long tail later.

## 5. Plugin
No plugin change: `CajetacRunner.parseDiagnostics` already maps `line`/`column` to a
precise `TextRange`. A located semantic diagnostic simply lands on the right token.
