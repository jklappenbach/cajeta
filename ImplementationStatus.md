# Memory Model + Wire Formats — Implementation Status

Tracks rollout progress for the doctrine in `MemoryModel.md` and `WireFormats.md`. Update as work proceeds — when a step completes, change `[ ]` to `[x]` and move the **← currently here** marker.

---

## Current status

**Phase:** Session 2 complete (AST + minimal use-after-move static check). 295 tests total, all passing.
**Current line item:** **Session 3 / Step 3.1** — runtime `DropEntry` + helpers + `drop_watermark` on exception frame.

---

## Completed

### Design & specification
- `MemoryModel.md` — v1 spec: single-owner heap, `#` transfer operator, borrow/move rules, static-analysis rules (intra-function + elision), fields-as-owners, container conventions, struct-view integration, drop chain with watermark, debug-mode runtime checks.
- `WireFormats.md` — v1 spec: `struct` keyword, packed-default layout + `@Align(natural)`, endianness annotations (`@BigEndian` / `@LittleEndian`), inline length-prefix for variable-size fields, view constructor synthesis (`MyStruct(byte[])` + `.from()` / `.view()` aliases), validate-once-at-construction, mutation rules, wire-format versioning guidance.
- Auto-memory pointers: `project_memory_model.md`, `project_wire_formats.md`, `project_implementation_status.md`.

### Pre-doctrine codebase state
- 268 tests passing (Java-idiom, no `#`, no `struct`).
- Existing memory model leaks (no drops, no borrow checking).
- 16 files modified + 9 new test files uncommitted on `main` from prior session.

---

## Plan

### Session 1 — Parser foundation  ✅ complete

- [x] **1.1** Update annotation casing in docs to TypeCamelCase (`@BigEndian`, `@LittleEndian`, `@Align`).
- [x] **1.2** Lexer: add `#` token (already `REFERENCE`); add `struct` keyword (`STRUCT`).
- [x] **1.3** Parser grammar: `#` as expression-prefix, optional `#` on `formalParameter` typeType and `typeTypeOrVoid`; new `structDeclaration` rule; `@BigEndian` / `@LittleEndian` / `@Align(natural)` accepted via the existing `annotation` rule.
- [x] **1.4** Regenerate ANTLR parser — added `visitStructDeclaration` stub in `CajetaLlvmVisitor` to keep it concrete.
- [x] **1.5** Existing 268 tests pass — backward-compatible.
- [x] **1.6** 17 new parse-level tests in `test/parser/Session1ParseTests.cpp`: valid samples (parses) + invalid samples (parser rejects).

### Session 2 — AST + basic codegen  ✅ complete

- [x] **2.1** Added `MoveExpression` AST node wrapping `#expr`. Cleaner than a flag on Expression — consumers detect via `dynamic_pointer_cast`. Wired through `Expression::fromContext` via the new `REFERENCE expression` grammar alternative.
- [x] **2.2** `FormalParameter` gained `bool transferred` set from `ctx->REFERENCE()`. `Method` gained `bool returnsOwnership` set from `ctx->typeTypeOrVoid()->REFERENCE()` in the visitor.
- [x] **2.3** `CajetaStruct` stub at `src/cajeta/type/CajetaStruct.h` — sibling of `CajetaClass`, carries `endianness` and `alignment` annotations. Full layout/codegen deferred to Session 4.
- [ ] **2.4** Drop emission at scope end — **deferred to Session 3** with the runtime DropEntry infrastructure (premature without the chain).
- [x] **2.5** `Scope::markMoved` / `Scope::isMoved` track moved-out identifiers; walks up the scope chain to find the declaring scope. `IdentifierExpression::generateCode` throws `CAJETA_ERROR_USE_AFTER_MOVE` on reads of moved names.
- [x] **2.6** 10 new tests in `test/parser/UseAfterMoveTests.cpp` — valid moves (5) and rejected use-after-move (5). All passing. 295 tests total.

Grammar cleanup as part of this session: removed the legacy `REFERENCE?` from `variableDeclarator` and the var-form `localVariableDeclaration`; `#expr` now flows uniformly through `MoveExpression`. The `bool reference` flag on `VariableDeclarator` is now always false (left in place to avoid churning callers; will be retired during migration).

### Session 3 — Drop chain + path-based analysis  ← **next**

- [ ] **3.1** Runtime: `DropEntry` struct, `__cajeta_drop_push` / `__cajeta_drop_pop_run` helpers, `drop_watermark` field on exception frame.  ← **currently here**
- [ ] **3.2** Codegen: DropEntry alloca + chain push/pop at scope boundaries.
- [ ] **3.3** `__cajeta_throw` updated to unwind drops down to the watermark before `longjmp`.
- [ ] **3.4** Static check: path-based borrow tracking (field-path borrows, alias-mutation).
- [ ] **3.5** Static check: inter-procedural elision (method returns borrow tied to `this`; single-param returns tied to that param; multi-param borrow-return forbidden).
- [ ] **3.6** Tests: valid borrows, invalid alias-mutation, invalid multi-param borrow-return.

### Session 4 — Struct view layout + constructor

- [ ] **4.1** Type system: `CajetaStruct` with explicit layout computation (fixed-prefix size, variable-size field identification, all-fixed fast-path detection).
- [ ] **4.2** Constructor synthesis: `MyStruct(byte[])` returning a borrow-view; plus `.from(...)` and `.view(...)` aliases.
- [ ] **4.3** Construction-time bounds check (`data.size() >= minSize`).
- [ ] **4.4** Construction-time length-prefix validation (single sweep over variable-size fields).
- [ ] **4.5** Throw on construction failure.
- [ ] **4.6** Field accessor codegen: load/store at the computed offset.
- [ ] **4.7** Borrow checker integration: view = borrow of buffer; field access = path-based borrow.
- [ ] **4.8** Tests: valid view construction, valid field reads, valid field writes, oversize/undersize buffer rejection.

### Session 5 — Endianness, alignment, variable-size offsets

- [ ] **5.1** Annotation processing: `@BigEndian`, `@LittleEndian`, `@Align(natural)` on struct declarations.
- [ ] **5.2** Endianness intrinsics: emit `bswap` on field access when struct endianness differs from host.
- [ ] **5.3** `@Align(natural)` codegen: insert padding for natural alignment; use standard aligned loads.
- [ ] **5.4** Variable-size offset cache: resolve offsets at construction time, cache in view layout.
- [ ] **5.5** Mutation rule enforcement: reject reassignment of variable-size struct fields at the AST level.
- [ ] **5.6** Tests: big-endian reads, little-endian reads, aligned layouts, variable-size offsets, rejected mutations.

### Session 6 — Migration

- [ ] **6.1** Rewrite stdlib runtime helpers (string concat, substring, etc.) to integrate with drops instead of leaking.
- [ ] **6.2** Migrate existing 268 tests to the new ownership idioms where applicable.
- [ ] **6.3** Update `README.md` test-suite table.

---

## Conventions

- Each session must end with full regression passing (`./build/test/cajeta_test`). No half-finished commits.
- Update the **← currently here** marker every time work moves to a new step.
- Mark items `[x]` only when both implementation and tests are complete for that step.
- If a step expands into multiple sub-tasks, sub-bullet them under the original.

---

## Deferred / out-of-scope

- Owning-view variant (`MyStruct(#bytes)`): purely additive; add post-v1.
- Multi-threading / `Send`/`Sync` analog.
- FFI safety beyond signature-trust.
- `unsafe` escape hatch.
- Reflection / dynamic-dispatch borrow analysis.
- Debug-mode runtime checks (`--debug-borrows` flag, generation field, 2-layout build): deferred to a post-v1 session.

---

## Notes / open questions

(Add as they surface during implementation.)
