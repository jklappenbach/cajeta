---
title: 'Memory Model — Implementation Status'
layout: '~/layouts/MarkdownLayout.astro'
category: 'Status'
description: 'Tracks rollout progress for the doctrine in MemoryModel.md. Update as work proceeds — when a step completes, change [ ] to [x] and move the ← currently here marker.'
---

Tracks rollout progress for the doctrine in `MemoryModel.md`. Update as work proceeds — when a step completes, change `[ ]` to `[x]` and move the **← currently here** marker.

---

## Current status

**Phase:** v1 memory-model rollout **complete**. Sessions 1, 2, 3, 3.5 done.
**Current line item:** none — see **Known gaps** at the bottom for deferred items if/when picked up.

---

## Completed

### Design & specification
- `MemoryModel.md` — v1 spec: single-owner heap, `#` transfer operator, borrow/move rules, static-analysis rules (intra-function + elision), fields-as-owners, container conventions, drop chain with watermark, debug-mode runtime checks.
- Auto-memory pointers: `project_memory_model.md`, `project_implementation_status.md`.

### Pre-doctrine codebase state
- 268 tests passing (Java-idiom, no `#`).
- Existing memory model leaks (no drops, no borrow checking).
- 16 files modified + 9 new test files uncommitted on `main` from prior session.

---

## Plan

### Session 1 — Parser foundation  ✅ complete

- [x] **1.1** Update annotation casing in docs to TypeCamelCase (`@Align`).
- [x] **1.2** Lexer: add `#` token (already `REFERENCE`).
- [x] **1.3** Parser grammar: `#` as expression-prefix, optional `#` on `formalParameter` typeType and `typeTypeOrVoid`.
- [x] **1.4** Regenerate ANTLR parser.
- [x] **1.5** Existing 268 tests pass — backward-compatible.
- [x] **1.6** 17 new parse-level tests in `test/parser/Session1ParseTests.cpp`: valid samples (parses) + invalid samples (parser rejects).

### Session 2 — AST + basic codegen  ✅ complete

- [x] **2.1** Added `MoveExpression` AST node wrapping `#expr`. Cleaner than a flag on Expression — consumers detect via `dynamic_pointer_cast`. Wired through `Expression::fromContext` via the new `REFERENCE expression` grammar alternative.
- [x] **2.2** `FormalParameter` gained `bool transferred` set from `ctx->REFERENCE()`. `Method` gained `bool returnsOwnership` set from `ctx->typeTypeOrVoid()->REFERENCE()` in the visitor.
- [ ] **2.4** Drop emission at scope end — **deferred to Session 3** with the runtime DropEntry infrastructure (premature without the chain).
- [x] **2.5** `Scope::markMoved` / `Scope::isMoved` track moved-out identifiers; walks up the scope chain to find the declaring scope. `IdentifierExpression::generateCode` throws `CAJETA_ERROR_USE_AFTER_MOVE` on reads of moved names.
- [x] **2.6** 10 new tests in `test/parser/UseAfterMoveTests.cpp` — valid moves (5) and rejected use-after-move (5). All passing. 295 tests total.

Grammar cleanup as part of this session: removed the legacy `REFERENCE?` from `variableDeclarator` and the var-form `localVariableDeclaration`; `#expr` now flows uniformly through `MoveExpression`. The `bool reference` flag on `VariableDeclarator` is now always false (left in place to avoid churning callers; will be retired during migration).

### Session 3 — Drop chain + elision  🟡 mostly complete (3.4 deferred to 3.5-session)

- [x] **3.1** Runtime: `cajeta_drop_entry`, push/pop/mark-inactive helpers, `drop_watermark` on exception frame, `__cajeta_drop_count_*` test observability.
- [x] **3.2** Codegen: array locals push DropEntry allocas; move-out flips inactive; `Method::emitOwnerDrops` fires pop+drop on every normal return path.
- [x] **3.3** `__cajeta_throw` unwinds drops to the catching frame's watermark before `longjmp`.
- [ ] **3.4** Path-based borrow tracking — **carved out into Session 3.5**.
- [x] **3.5** Multi-parameter free-function borrow-return rejected at `Method::generatePrototype` with `CAJETA_ERROR_BORROW_RETURN_MULTI_PARAM`.
- [x] **3.6** 12 new tests: 5 `DropChainTests`, 7 `ElisionTests`. 307 tests total.

New language-internal intrinsics: `Cajeta.dropCount()` / `Cajeta.dropCountReset()` for test observability of the drop counter. Diagnostic-only — not user-facing stdlib.

### Session 3.5 — Path-based borrow tracking (carved out of Session 3)  ✅ complete

- [x] **3.4.1** `Scope` now tracks `movedPaths` as a string set with prefix semantics; `markMovedPath` / `isPathMoved` walk parent scopes.
- [x] **3.4.2** `DotExpression::generateCode` builds the dotted path and consults the scope at the top of codegen, throwing `CAJETA_ERROR_USE_AFTER_MOVE` if any prefix has been moved.
- [x] **3.4.3** Root-identifier moves (`#person`) also invalidate sub-paths (`person.name`) via the shared `movedNames` check inside `isPathMoved`.
- [x] **3.4.4** 7 new tests in `test/parser/PathBorrowTests.cpp`: exact-match path moves, root moves blocking sub-paths, double-moves, deeper-path moves blocking transitive reads, sibling paths still readable. 314 tests total.

Alias-mutation through path writes (`person.name = #other` invalidating a live borrow into `person.name`) is deferred — needs a "live borrow" tracking pass that knows which paths each named borrow inhabits. Today's coverage catches every case where a move appears textually before the conflicting read.

---

## Conventions

- Each session must end with full regression passing (`./build/test/cajeta_test`). No half-finished commits.
- Update the **← currently here** marker every time work moves to a new step.
- Mark items `[x]` only when both implementation and tests are complete for that step.
- If a step expands into multiple sub-tasks, sub-bullet them under the original.

---

## Deferred / out-of-scope

- Multi-threading / `Send`/`Sync` analog.
- FFI safety beyond signature-trust.
- `unsafe` escape hatch.
- Reflection / dynamic-dispatch borrow analysis.
- Debug-mode runtime checks (`--debug-borrows` flag, generation field, 2-layout build): deferred to a post-v1 session.

---

## Notes / open questions

(Add as they surface during implementation.)
