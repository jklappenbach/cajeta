# jit-run-parse-abort — CLI aborts on malformed source

## 1. Definition

### 1.1 Purpose
`cajeta jit-run` terminates with SIGABRT on malformed input instead of printing
a diagnostic and exiting nonzero. Discovered 2026-07-03 by the doc-snippet
checker (`scripts/check-doc-snippets.sh`) feeding it imperfect snippets.

### 1.2 Observed crash shapes
1. A parse error is thrown as a raw `char const*` that no jit-run frame
   catches: the ANTLR diagnostic prints, then
   `terminate called after throwing an instance of 'char const*'` → SIGABRT.
2. Method-declaration-inside-method nesting produces
   `terminate called after throwing an instance of 'std::bad_any_cast'`
   → SIGABRT with no source diagnostic at all.

### 1.3 Non-goals
Recovering or repairing malformed source; only failing cleanly.

## 2. Clean failure on malformed input

### 2.1 Requirements
1. Any source error reaching the jit-run CLI produces a source-located
   diagnostic on stderr and exit code 1 — never SIGABRT, never an uncaught
   exception.
2. The `bad_any_cast` shape gets a real diagnostic (the visitor should not
   any_cast child results after a parse error has been recorded).

### 2.2 Use cases
1. As a **tool author** (doc-snippet checker, IDE), when I feed jit-run a bad
   file, I get a parseable diagnostic and a nonzero exit, not a core dump.
2. As a **user**, when my file has a syntax error, jit-run reports it the same
   way `cajeta build` does.

## 3. Tests
Fixtures for both shapes (top-level stray token; method-in-method nesting),
asserting diagnostic text and exit code 1 with no abort.

---

**CLOSED — verified fixed on cajeta 0.14.0 (8ca5b362), 2026-08-01.** Re-ran this
spec's repro against a freshly built 0.14.0 compiler; the defect no longer
reproduces. Archived per td-project-workflow (spec -> archive, INDEX row dropped).
