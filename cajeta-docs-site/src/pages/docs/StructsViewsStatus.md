---
title: 'Structs + Views — Implementation Status'
layout: '~/layouts/MarkdownLayout.astro'
category: 'Status'
description: 'Tracks rollout progress for the new aggregate-type design specified in Structs.md and Views.md. Update as work proceeds — when a step completes, change [ ] to [x] and move the ← currently here marker.'
---

Tracks rollout progress for the new aggregate-type design specified in `Structs.md` and `Views.md`. Update as work proceeds — when a step completes, change `[ ]` to `[x]` and move the **← currently here** marker.

This rollout supersedes the old "struct as wire-format view" implementation. Significant portions of the old `CajetaStruct` machinery (layout, endianness, packed/aligned annotations, bswap codegen, bounds checking, length-prefix sweep) are **reusable** for the new `view` construct; the new `struct` construct (stack value aggregate with class refs) is largely new work.

---

## Current status

**Phase:** Phase 1 in progress. Sessions 1, 2 complete.
**Current line item:** S3 (next session start) — view owning variant (`#bytes`) + required endianness annotation.

---

## What's reusable from prior work

The old `CajetaStruct` implementation already provides, mostly intact for the new `view`:

- Packed-by-default layout with `@Align(natural)` opt-in.
- `@BigEndian` / `@LittleEndian` annotations + `bswap` intrinsic emission.
- View constructor synthesis (the call-site `MyStruct(byte[])` lowering).
- Construction-time bounds check (`buf.length >= MinSize`).
- Field accessor codegen with endian-aware load/store.
- Single-trailing-`String`-field variable-size layout (Session 5.5b).
- Pass-by-pointer calling convention.

What's **not** reusable and must be built fresh:

- The new `struct` semantics (stack alloca, class refs, inline composition, interface dispatch). The old `struct` was view-only and forbade class refs entirely.
- Tagged fat pointer interface dispatch (3-word interface values, vtable synthesis per (struct, interface) pair, kind-tag drop dispatch).
- Owning-view variant (`MyStruct(#bytes)` — the additive form the old spec deferred).
- Multiple variable-size fields per view + fields after a variable-size field with offset cache.
- Required endianness annotation (currently optional).

---

## Plan

Sessions are sized for ~1 working day; each ends with full regression passing.

### Phase 1 — Keyword separation + view repurposing

#### Session 1 — `view` keyword + migrate existing tests  ✅ complete
- [x] **1.1** Lexer: `VIEW` token added in `CajetaLexer.g4` between `TRY` and `VOID` (alphabetical).
- [x] **1.2** Parser: `viewDeclaration` rule added in `CajetaParser.g4` mirroring `structDeclaration`; `typeDeclaration` accepts either. Visitor: `visitViewDeclaration` and `visitStructDeclaration` both delegate to a new private `buildStructOrViewNode` helper, so the two paths share the existing CajetaStruct codegen verbatim. `Compiler.cpp`'s archive pre-scan visitor also handles both.
- [x] **1.3** Migrated 24 tests across `StructViewTests`, `StructViewBoundsTests`, `EndianAlignTests`, `VariableSizeStructTests` — `public struct` → `public view` in every cajeta source string. All pass under the new keyword.
- [x] **1.4** New `KeywordEquivalenceTests.cpp` with 3 tests: view-keyword executes, struct-keyword still executes, both produce identical runtime results from the same source. The third test will need to evolve in S2 when struct gains stack-alloca semantics and diverges from view.
- **Pass criteria met:** 718 / 718 tests pass (715 prior + 3 new equivalence).

#### Session 2 — Repurpose `struct` for stack semantics (no-op layer)  ✅ complete
- [x] **2.1** `CajetaStruct` and `CajetaView` are siblings under a new common base `CajetaAggregate` (extends `CajetaClass`). Initial S2 commit had `CajetaView` inheriting from `CajetaStruct` for cast-site convenience; that was refactored into the cleaner sibling shape before S3 started (see follow-up commit notes below). `CajetaAggregate` carries only what's genuinely shared: the variable-size-field predicate and the no-vtable-header field-index override. Endianness, alignment, and view-specific codegen live on `CajetaView`. Stack-struct semantics will land on `CajetaStruct` in S6.
- [x] **2.2** `CajetaStruct::generatePrototype()` throws `CAJETA_ERROR_STRUCT_UNIMPLEMENTED` with a message pointing at the rollout doc. `CajetaView::generatePrototype()` owns the legacy view-style codegen (packed/aligned layout, length-prefix substitution, bounds check).
- [x] **2.3** `buildStructOrViewNode` gained an `asView` parameter and constructs a `CajetaAggregatePtr` to either leaf type. Endianness/alignment annotations apply only to views (per Structs.md, structs are host-endian compiler-chosen-layout). All 24 existing view tests pass on the new `CajetaView` path.
- [x] **2.4** `KeywordEquivalenceTests` evolved as planned in S1: `structKeywordParsesButRejectsAtCodegen` confirms struct declarations throw at compile time; `structDeclaredAndUsedAsLocalIsRejected` is the S2.4 negative test. Both expect the stub error today and flip to positive coverage in S6 when stack-struct semantics land.
- [x] **Cast-site audit** (follow-up). All 14 existing `dynamic_pointer_cast<CajetaStruct>(t)` sites reclassified: 12 → `CajetaAggregate` (the "is this struct-shaped?" intent — applies to both leaves); 2 → `CajetaView` (view-ctor detection in `LocalVariableDeclaration`, view-ctor synthesis in `MethodCallExpression`, plus the endianness bswap in `DotExpression::maybeBswap` since endianness is view-only).
- **Pass criteria met:** 718 / 718 tests pass; `view` works on `CajetaView`, `struct` is parsing-only; class hierarchy is sibling-clean before S3 builds on top.

#### Session 3 — View owning variant + required endianness
- [ ] **3.1** Parser: accept `#bytes` argument to view constructor; AST records owning-vs-borrow at the call site.
- [ ] **3.2** Codegen: owning-form view registers a drop entry for the buffer; scope exit drops the buffer.
- [ ] **3.3** Borrow checker: owning view is treated as an owner of its buffer; transferring (`#h`) and storing in heap fields allowed.
- [ ] **3.4** Reject view declarations without `@BigEndian` / `@LittleEndian` / `@HostEndian` annotation (`CAJETA_ERROR_VIEW_ENDIANNESS_REQUIRED`).
- [ ] **3.5** 8 new tests: 4 owning-form (basic, transfer, drop count, escape rejection) + 4 endianness-required (each annotation accepted, missing rejected, multi-annotation rejected, `@HostEndian` accepted).
- [ ] **Pass criteria:** Both view forms work; endianness is mandatory.

### Phase 2 — View enhancements

#### Session 4 — View methods + nested views
- [ ] **4.1** Parser: accept method declarations inside `viewDeclaration`.
- [ ] **4.2** Codegen: methods on views emit normal LLVM functions with `this` as the view pointer. Direct calls only (views don't implement interfaces in v1).
- [ ] **4.3** Reject view methods that are `virtual`, return `Self`-rooted borrows, or declare their own type parameters.
- [ ] **4.4** Parser + layout: view field can be of another view type. Inner inlines at its declared offset. Layout pass recurses; endianness/alignment inherits from outer unless inner has its own annotation.
- [ ] **4.5** Borrow checker: `outer.inner.field` resolves as a path-borrow rooted at the outer's buffer.
- [ ] **4.6** 10 new tests: 5 view-method (read-only method, write method, method calling another method, virtual rejected, Self-borrow rejected); 5 nested view (fixed-size inner, variable-size inner, endianness inheritance, alignment inheritance, recursive cycle rejected).
- [ ] **Pass criteria:** View methods callable; nested views work for both fixed and variable inners.

#### Session 5 — Multiple variable-size fields + post-variable fields
- [ ] **5.1** Layout pass: track all variable-size fields, not just one. Drop the "single trailing variable-size field" restriction.
- [ ] **5.2** Construction-time offset cache: when a view has any variable-size field, allocate a stack-resident offset table; constructor walks length-prefixes in order, writes resolved offsets.
- [ ] **5.3** Field accessor codegen: post-variable-size field GEPs load offset from the cache instead of using a compile-time constant.
- [ ] **5.4** Fast-path detection: if all variable-size fields are at the end, skip the offset cache entirely (every fixed field has a compile-time-constant offset).
- [ ] **5.5** Length-prefix validation: constructor sweeps every length-prefix, verifies `(currentOffset + 4 + prefix-value) <= buf.length`. Throws `ParseException` on overrun. Recurses into nested variable-size views.
- [ ] **5.6** 12 new tests: multi-String, String + int32[], int32[] + String + int32, post-variable fixed field, post-variable nested view, oversize length-prefix throws, exact-fit, near-miss, fast-path triggered when all-trailing, fast-path NOT triggered when interleaved, deeply-nested length validation, offset cache size verification.
- [ ] **Pass criteria:** Arbitrary variable-size layouts work; security: length-prefix attacks throw cleanly.

### Phase 3 — New struct (stack value aggregate)

#### Session 6 — Stack alloca + class ref fields + aggregate initializer
- [ ] **6.1** Codegen: `struct Foo f;` emits `alloca` of struct's fixed total size, zero-initializes.
- [ ] **6.2** Aggregate initializer: `Foo f = Foo { first: 7, second: 11 };` parses and codegens as per-field stores into the alloca.
- [ ] **6.3** Allow class-typed fields in struct declarations (lift the prior "no class refs" restriction).
- [ ] **6.4** Drop chain: struct local pushes a drop entry; struct's drop function runs owned-class-ref fields' drops in reverse declaration order, then frees no bytes (stack-resident).
- [ ] **6.5** Borrow checker: path-borrow into struct fields rooted at the struct; same machinery the path tracker already uses.
- [ ] **6.6** Reject variable-tail fields (`byte[?]`) in structs at layout pass — those are view-only.
- [ ] **6.7** 12 new tests: declare + zero-init, declare + aggregate-init, declare + field-assign, struct holding String, struct holding two class refs, drop count verification, path-borrow into struct field, escape rejection, recursive struct rejected, variable-tail rejected, struct as parameter (pass-by-ptr), struct as return value.
- [ ] **Pass criteria:** Stack structs work end-to-end with primitive and class-ref fields.

#### Session 7 — Inline composition (struct as class field)
- [ ] **7.1** Class layout pass: when a class field's type is a struct, inline the struct's LLVM type at that offset instead of a pointer slot.
- [ ] **7.2** Class drop codegen: recurse into embedded struct fields in reverse declaration order, calling each struct's drop function.
- [ ] **7.3** Field access: `obj.embeddedStruct.field` GEPs through both the class layout and the struct layout in one chained operation.
- [ ] **7.4** Borrow tracking: path borrow extends through embedded struct fields (`obj.struct.field` is a path-borrow rooted at `obj`).
- [ ] **7.5** 8 new tests: class with embedded primitive-only struct, class with embedded struct holding class refs, drop order verification, field access through embedded struct, path-borrow through embedded, embedded struct sized correctly (sizeof match), nested embedded (class → struct → struct), array of embedded structs.
- [ ] **Pass criteria:** Inline composition works without extra allocation; drops fire in correct order.

#### Session 8 — Struct methods (direct calls only)
- [ ] **8.1** Parser: accept method declarations in `structDeclaration` (already accepted syntactically; just route to struct method codegen).
- [ ] **8.2** Codegen: struct method = LLVM function with `this` as struct pointer. Direct calls inline at the call site (or LLVM inliner takes care of it given the static target).
- [ ] **8.3** `this` field accesses GEP into the struct slot via the existing aggregate calling convention.
- [ ] **8.4** Method may return `Self` for direct-call use; restriction only applies later under interface dispatch.
- [ ] **8.5** 6 new tests: simple getter, mutating method, method calling another method on self, method taking another struct by value, method returning Self, method writing through embedded struct field.
- [ ] **Pass criteria:** Struct methods work for all field types; direct-call IR is monomorphized.

### Phase 4 — Struct interface dispatch

#### Session 9 — Vtable synthesis + interface implementation
- [ ] **9.1** Parser: accept `struct Foo implements Interface<T> { ... }` clause.
- [ ] **9.2** Type system: record per-struct list of implemented interfaces; for each (struct, interface) pair, synthesize a static vtable global containing function pointers to the struct's method implementations.
- [ ] **9.3** Vtable layout matches the interface's method order; method signature compatibility checked at struct declaration.
- [ ] **9.4** Reject interface declarations whose methods have their own type parameters (`CAJETA_ERROR_INTERFACE_METHOD_GENERIC`).
- [ ] **9.5** Direct calls on the concrete struct type still go through the monomorphized path — no vtable hit.
- [ ] **9.6** 6 new tests: struct implements one interface, struct implements two interfaces, vtable populated correctly (linkage-time check), method signature mismatch rejected, missing method rejected, method-generic-on-interface rejected.
- [ ] **Pass criteria:** Vtables generated correctly; concrete-type calls remain direct.

#### Session 10 — Tagged fat pointer construction + borrow tracking
- [ ] **10.1** Interface value LLVM layout: `{ data_ptr, vtable_ptr, kind_tag }`, total 24 bytes (with padding).
- [ ] **10.2** Assignment codegen: assigning a struct to an interface variable builds the fat pointer with the right vtable + `kind_tag = BORROWED_STRUCT`. Assigning a class builds it with the class's vtable (loaded from class header) + `kind_tag = BORROWED_CLASS` or `OWNED_CLASS` depending on `#` at assignment site.
- [ ] **10.3** Borrow tracker: struct-rooted interface value records a borrow rooted at the source struct. Storing in a heap field or returning from a function whose source struct is local → `CAJETA_ERROR_INTERFACE_VALUE_ESCAPE` pointing at the underlying struct.
- [ ] **10.4** Interface value drop: switch on `kind_tag`. `BORROWED_*` → no action. `OWNED_CLASS` → drop the underlying class via `data_ptr`.
- [ ] **10.5** 10 new tests: struct → interface assignment, class → interface assignment (borrowed), class → interface assignment (`#owned`), struct interface escape rejected, class-borrow escape rejected (existing rule), interface value transferred (`#it`), drop count verification per kind, mixed-implementation interface array, layout-size check (24 bytes), kind tag values match spec.
- [ ] **Pass criteria:** Fat pointer construction is correct; borrow rules apply per kind.

#### Session 11 — Dispatch through interface + method restrictions
- [ ] **11.1** Method call through interface value: load `vtable_ptr`, GEP to method offset, indirect-call with `data_ptr` as first arg.
- [ ] **11.2** Reject method calls through interface value when the method returns `Self` (`CAJETA_ERROR_DYN_DISPATCH_SELF_RETURN`).
- [ ] **11.3** Reject method calls through interface value when the method has its own type parameters (already rejected at interface declaration per S9.4, but double-check at call site for any path that slips through).
- [ ] **11.4** Mixed-implementation interface dispatch: same call site works on both class-rooted and struct-rooted interface values.
- [ ] **11.5** 8 new tests: dispatch to struct impl, dispatch to class impl, dispatch in a polymorphic array iteration, Self-return through interface rejected, direct Self-return on concrete struct still works, method called through interface value matches direct-call result, vtable hit count verified, mixed class/struct in same Vec dispatches correctly.
- [ ] **Pass criteria:** Through-interface dispatch works for both kinds with uniform call shape; restrictions enforced.

### Phase 5 — Wrap-up

#### Session 12 — Cleanup + docs + final regression
- [ ] **12.1** Update `ImplementationStatus.md` to note that struct/view rollout has separate tracker (`StructsViewsStatus.md`), and that this rollout completed.
- [ ] **12.2** README test-suite table: add ViewKeyword, ViewOwning, ViewMethods, ViewNested, ViewMultiVar, StructStack, StructComposition, StructMethods, StructInterface, FatPointerDispatch sub-suites.
- [ ] **12.3** Remove the stub error from `struct` keyword (Session 2.2); all paths now have real implementations.
- [ ] **12.4** Remove any remaining "old-struct-was-view" deprecation shims.
- [ ] **12.5** Final regression: 3 consecutive clean runs of full test suite.
- [ ] **12.6** Update `MemoryModel.md`'s Structs and Views sections if any rules changed during implementation.
- [ ] **Pass criteria:** All tests pass three times in a row; no `XXX_UNIMPLEMENTED` paths remain.

---

## Conventions

- Each session must end with full regression passing (`./build/test/cajeta_test`). No half-finished commits.
- Update the **← currently here** marker every time work moves to a new step.
- Mark items `[x]` only when both implementation and tests are complete.
- If a step expands into sub-tasks, sub-bullet them under the original.
- Sessions are guidelines, not contracts; if a session's work fits in half a day, move on to the next. If it splits, carve out a `Session N.5`.

---

## Deferred / out-of-scope

Captured here so future work knows where to pick up:

- **`Self`-returning methods through dyn dispatch.** Strict v1 — direct calls only. Relax later via associated-type-style mechanisms if real use cases push.
- **Method-level generics on interface methods.** Strict v1 — interface methods cannot have own type parameters. Dictionary-passing dispatch could relax this.
- **Owning struct boxed behind an interface** (Rust's `Box<dyn Trait>`). Defer; v1 fat pointers always borrow struct data.
- **Owning-view variant for self-describing formats.** Out of scope entirely — views are fixed-layout only. Ion / JSON / CBOR / MessagePack codecs live in `cajeta.codec.*` libraries.
- **Variable-size element arrays in views** (`String[]` as a view field). Single-level variable-size only in v1; deeply-nested variable-size fields require offset-tree machinery.
- **Methods on views returning borrows into self.** Defer until nested-borrow tracking is solid.
- **Multi-threading on views.** Buffer sharing across fibers needs synchronization primitives that aren't part of v1.
- **Inheritance for structs.** Out of scope by design — structs don't inherit.
- **Inheritance for views.** Out of scope by design.

---

## Notes / open questions

(Add as they surface during implementation.)

- **Performance baseline.** Once Session 5 lands, run the view-overlay micro-benchmark from `HarnessDesign.md` to confirm zero-copy claims hold. Target: within 5% of C `(Record*) buf` for host-endian fixed-layout reads.
- **Interface value size.** 24 bytes is generous. If profiling shows interface-heavy code paths suffering, consider packing `kind_tag` into the low bits of `data_ptr` (pointer alignment gives 3 free bits, plenty for the 3 kind values). Optimization, not v1.
- **Iterator-as-struct ergonomics.** Sessions 9-11 implement the machinery; the user-facing `for (x in arr)` desugaring still needs to be wired to monomorphize through the iterator. May surface as a Session 11.5 if the for-loop codegen needs adjustment.
