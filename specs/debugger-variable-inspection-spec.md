# debugger-variable-inspection — spec (DRAFT)

## 1 Definition

### 1.1 Purpose
Make stopped-state variables genuinely inspectable in CLion: structured
expansion, readable values, declared names, editing, and hover evaluation.

### 1.2 Problem (Julian, live tour session 2026-07-22)
The Variables view shows flat locals only: `variablesReference` is always 0,
so collections and objects cannot be expanded; strings render as
handles/ids rather than text; mangled/linker names leak where declared
names belong; values can be set only for flat scalars; hovering a variable
in the editor shows nothing.

## 2 Features (to be enumerated with use cases at design time)
- 2.1 Structured children: expand objects to fields, collections to
  elements (ArrayList, maps, arrays), bounded + paged for large ones.
- 2.2 Value rendering: String as text; primitives typed; null/moved-out
  facets preserved (readOnly stays).
- 2.3 Declared names only, at every level.
- 2.4 Editing: setVariable through references (fields/elements), not just
  frame-local scalars.
- 2.5 Editor hover: evaluate an identifier against the active frame when
  stopped (plugin XDebuggerEvaluator + a server `evaluate`, which does not
  exist today).

## 3 Notes
Overlaps memory-viewer (layout walking) and DebugVars (frame decode) —
reuse both. Server has NO `evaluate` request today; hover needs one.
