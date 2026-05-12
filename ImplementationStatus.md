# Memory Model + Wire Formats — Implementation Status

Tracks rollout progress for the doctrine in `MemoryModel.md` and `WireFormats.md`. Update as work proceeds — when a step completes, change `[ ]` to `[x]` and move the **← currently here** marker.

---

## Current status

**Phase:** Sessions 1–5 + 5.5b complete. 334 tests total, all passing. Wire-format support: declared structs, packed/natural alignment, endianness bswap, bounds-checked construction, inline `String` fields (read-only, owned-copy result).
**Current line item:** **Session 6** — migration (rewrite leaking runtime helpers, update existing tests).

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

### Session 5 — Endianness, alignment (variable-size offsets deferred)  🟡 core complete

- [x] **5.1** Annotation parsing — `visitStructDeclaration` walks the enclosing `typeDeclaration`'s `classOrInterfaceModifier` list, recognizes `@BigEndian`, `@LittleEndian`, and `@Align(...)` (treated as natural alignment in v1), and configures the `CajetaStruct` instance.
- [x] **5.2** `DotExpression::maybeBswap` emits `llvm.bswap.iN` on integer fields >= 16 bits when struct endianness ≠ host. Hooked into the read path (`loadIfLValue`, `ReturnStatement`) and the write path (`BinaryOpExpression` ASSIGN, after slot-type coercion).
- [x] **5.3** `CajetaStruct::generatePrototype` passes `isPacked` to `setBody` based on alignment annotation; natural alignment inserts padding.
- [x] **5.4** View construction now bounds-checks: `count * elem_size >= sizeof(struct)`. Element size comes from the argument's `CajetaArray` resolvedType (e.g. `int32[]` → 4-byte elements). Failure routes through `__cajeta_throw` so user code can `try/catch` it.
- [ ] **5.5** Variable-size struct fields (`String`, `T[]` inline) + their offset cache + mutation rule — **deferred to Session 5.5b**. Needs a different value representation for inline strings since they aren't null-terminated.
- [x] **5.6** 9 new tests across two suites — 5 in `EndianAlignTests`, 4 in `StructViewBoundsTests` (sufficient buffer, undersize-throws, exact-size, near-miss). 329 tests total.

### Session 5.5b — Variable-size struct fields  ✅ complete (single-trailing-field shape)

- [x] **5.5b.1** `CajetaStruct::isVariableSize` recognizes `String`-typed fields. The LLVM struct substitutes `i32` (the length prefix) at the slot; data bytes live past the LLVM struct's footprint. Layout rule: variable-size fields must be last (enforced at `generatePrototype` with `CAJETA_ERROR_VARSIZE_FIELD_NOT_LAST`).
- [x] **5.5b.2** Runtime helper `__cajeta_str_view_to_owned(data, length)` allocates a null-terminated copy so the result is compatible with the existing String stdlib.
- [x] **5.5b.3** `DotExpression::generateCode` detects variable-size fields and emits specialized codegen: load length-prefix, GEP past the LLVM struct to data start, call the runtime helper, return the owned ptr.
- [x] **5.5b.4** `BinaryOpExpression` ASSIGN rejects variable-size field writes with `CAJETA_ERROR_VARSIZE_FIELD_ASSIGN`.
- [x] **5.5b.5** 5 new tests in `VariableSizeStructTests`: declaration shape, content read-back via `.equals`, `.size()` returns the prefix length, assignment rejected, post-variable fixed-size field rejected. 334 tests total.

**Out of scope:** multiple variable-size fields in one struct, `T[]` as a variable-size struct field, fields after a variable-size field (would need runtime-computed offsets), zero-copy String reads (today's read materializes an owned copy — pragmatic compromise; a length-aware StringView would be a bigger redesign).

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
