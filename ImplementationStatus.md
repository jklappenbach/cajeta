# Memory Model + Wire Formats — Implementation Status

Tracks rollout progress for the doctrine in `MemoryModel.md` and `WireFormats.md`. Update as work proceeds — when a step completes, change `[ ]` to `[x]` and move the **← currently here** marker.

---

## Current status

**Phase:** Sessions 1–4 complete (parser → drop chain → struct views). 320 tests total, all passing. Struct views support: declaration, view construction, field read/write through the view.
**Current line item:** **Session 5 / Step 5.1** — endianness/alignment annotation processing.

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

### Session 4 — Struct view layout + constructor  🟡 mostly complete

- [x] **4.1** `CajetaStruct::generatePrototype` builds a packed LLVM struct, registers under both canonical and short name. `getFixedSize` returns the byte count.
- [x] **4.2** View constructor synthesis via MethodCallExpression intercept: when the call site is `MyStruct(byte[])` with the matching struct name, emit the GEP past the array header and return the typed data pointer.
- [ ] **4.3–4.5** Construction-time bounds check — **deferred to Session 5**. Current intercept assumes the buffer is large enough; proper checking needs element-size accounting (the array header stores element count, not byte count).
- [x] **4.6** Field accessor codegen: `DotExpression` loads through alloca'd struct views and emits `CreateStructGEP` for the field. Pre-existing class field path was unused/untested; this session made it work for structs. ASSIGN and ReturnStatement both load-through DotExpression GEPs at the field's declared type, and the rhs is coerced to the field's slot type so wide-default integer literals (i64) write the right number of bytes.
- [x] **4.7** Borrow checker integration falls out of the existing path-based machinery: `h.version` is a DotExpression and the path tracker already handles it.
- [x] **4.8** 6 new tests in `test/parser/StructViewTests.cpp`: struct declaration, view construction, single-field write + read, multi-field independence, view-buffer aliasing, fresh-buffer zero reads. 320 tests total.

Auxiliary fixes from this session: `IdentifierExpression`-receiver `getResolvedType()` is null at pre-pass time (locals not in scope yet), so `DotExpression::generateCode` now re-resolves the receiver at codegen; `BinaryOpExpression` ASSIGN and `ReturnStatement` do the same for the receiver lookup when deciding load-/store-type.

### Session 5 — Endianness, alignment, variable-size offsets, view bounds-check  ← **next**

- [ ] **5.1** Annotation processing: `@BigEndian`, `@LittleEndian`, `@Align(natural)` on struct declarations.  ← **currently here**
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
