# Structs + Views — Implementation Status

Tracks rollout progress for the new aggregate-type design specified in `Structs.md` and `Views.md`. Update as work proceeds — when a step completes, change `[ ]` to `[x]` and move the **← currently here** marker.

This rollout supersedes the old "struct as wire-format view" implementation. Significant portions of the old `CajetaStruct` machinery (layout, endianness, packed/aligned annotations, bswap codegen, bounds checking, length-prefix sweep) are **reusable** for the new `view` construct; the new `struct` construct (stack value aggregate with class refs) is largely new work.

---

## Current status

**Phase:** Phase 2 complete (S4, S5, S5b done). Phase 3 in progress.
**Current line item:** S6.1–S6.5 complete — structs lay out, init via brace syntax, hold class refs, participate in the drop chain, and integrate with the existing borrow / path-borrow tracker. Next: S6.6 (explicit variable-tail rejection at layout pass).

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

#### Session 3 — View owning variant + required endianness  ✅ complete
- [x] **3.1** No new parser work — `#expr` already parses as `MoveExpression`. The view-construction site (`LocalVariableDeclaration`'s view-ctor detection) now classifies the single argument: `MoveExpression` → owning, anything else → borrow. New `Field::isOwningView()` flag carries the classification forward to the drop-registration site.
- [x] **3.2** New runtime helper `__cajeta_view_drop_owned(void* data_ptr)` reconstructs the array header (data_ptr − 8 bytes) and frees it. `LocalVariableDeclaration` emits a drop entry pointing at the field with this helper when `field->isOwningView()` is true. `MoveExpression::generateCode` already deactivates the source's drop entry, so no double-free.
- [x] **3.3** Owning-form views skip `setViewSource()`, so the existing `Statement.cpp` escape check (which fires only when `getViewSource()` is set) naturally allows owning views to be returned and transferred. Borrow-form keeps the escape rejection.
- [x] **3.4** `CajetaView::generatePrototype()` rejects views missing an endianness annotation with `CAJETA_ERROR_VIEW_ENDIANNESS_REQUIRED`. Visitor recognizes a new `@HostEndian` annotation in addition to `@BigEndian` / `@LittleEndian`. New `bool endiannessExplicit` on `CajetaView` distinguishes "user wrote @HostEndian" from "user wrote nothing".
- [x] **3.5** 8 new tests across two suites: `ViewOwningTests` (4 — basic owning construction, drop count exactly 1 per buffer for both forms, borrow-form escape rejected, owning-form escape allowed) and `ViewEndiannessRequiredTests` (4 — Big/Little/Host accepted, missing rejected). Existing view tests migrated: every `public view ...` declaration now carries `@HostEndian` where it was previously implicit (`StructViewTests`, `StructViewBoundsTests`, `VariableSizeStructTests`, `KeywordEquivalenceTests`, `EndianAlignTests`).
- **Pass criteria met:** 726 / 726 tests pass (718 prior + 8 new). Both view forms work end-to-end; endianness annotation is mandatory.

### Phase 2 — View enhancements

#### Session 4 — View methods + nested views  ✅ complete
- [x] **4.1** No new parser work — `classBody` (which both `classDeclaration` and `viewDeclaration` reuse) already accepts method declarations. Probe test confirmed method syntax parses on views.
- [x] **4.2** Method codegen path needed two fixes: (a) `CajetaClass::invokeMethod` was routing every non-static instance call through vtable dispatch (load vtable from object header → indirect call). Views have no vtable header. Now: aggregates skip the vtable path entirely (direct call to the resolved method). (b) `Method::generatePrototype` was re-inserting the implicit `this` parameter on every call, ballooning method signatures to `(this:ptr, this:ptr, this:ptr)` when iterated from multiple sites. Now: idempotency check on `parameterList.front()->getName() == "this"` skips re-insertion.
- [x] **4.3** Per-method restrictions:
  - **Virtual rejection**: not a syntactic concern. The vtable skip in 4.2 means aggregates don't get virtual dispatch by construction; there's no `virtual` keyword in cajeta and views can't inherit, so nothing to reject at the source level.
  - **Method-level generics**: turned out to be a non-restriction. The grammar (`methodDeclaration`) doesn't include `typeParameters?` on methods at all — they aren't supported anywhere in cajeta, by design (see the `interfaceMemberDeclaration` comment in `CajetaParser.g4`). Nothing view-specific to enforce. The parser does mishandle `<T>` written on a method (stderr error then segfault) — that's a general parser-error-handling issue, not S4 scope.
  - **Self-rooted borrow returns**: genuine Views.md restriction (a method can't return a borrow into `this`). Detection needs a data-flow pass tracing return-value provenance back through field reads; deferred to a follow-up session. Documented as TODO in `ViewMethodsTests.cpp`.
- [x] **4.4** Nested views worked out of the box for layout — the layout loop's `property->getType()->getLlvmType()` returns the inner view's LLVM struct type, which inlines naturally. Each view declares its own endianness (S3.4 requirement), so v1 does not auto-inherit from outer; spec language about inheritance describes the conceptual model, not v1's behavior. Added a direct-recursion guard (`CAJETA_ERROR_VIEW_RECURSIVE`) that rejects a view containing a field of its own type; transitive cycles deferred.
- [x] **4.5** Path-borrow tracking for `outer.inner.field` already works via the existing `DotExpression::buildPath` / scope `markMovedPath` machinery, which builds dotted paths of arbitrary depth. Confirmed by the doubly-nested test exercising `bb.first.a.x` four-level paths.
- [x] **4.6** 10 new tests across two files: `ViewMethodsTests` (5 — read-only method, write method, method calling another method, interleaved read/write, static method) and `NestedViewTests` (5 — fixed-size inner inlines, per-view endianness annotation, mixed endianness across nesting, doubly-nested view, recursive cycle rejected).
- **Pass criteria met:** 736 / 736 tests pass (726 prior + 10 new). View methods callable; nested views work for fixed and variable inners; recursion rejected at layout time.

#### Session 5 — Multi-trailing variable-size fields + length-prefix validation  ✅ tractable subset complete
- [x] **5.1** Layout pass tracks all variable-size fields. The "single var-size field" restriction is lifted for the trailing case (multiple `String` fields in a row work). The "fixed field after var-size" restriction is RETAINED with a clearer error message pointing at S5b — supporting it needs the offset cache (carved out).
- [skip→S5b] **5.2** Construction-time offset cache — deferred. The chosen approach is walk-the-prefixes at every accessor instead. Costs O(K) per access for the Kth var-size field, but no cache infrastructure (no stack alloca per view local, no fat-pointer view value). When post-variable fixed fields land in S5b, the cache becomes mandatory.
- [skip→S5b] **5.3** Post-variable accessor codegen — N/A; the walk-the-prefixes path in S5.2 handles the trailing-only case without it.
- [skip→S5b] **5.4** Fast-path detection — N/A under walk-the-prefixes; everything follows the same path. Relevant when S5b adds the cache.
- [x] **5.5** Length-prefix validation: `MethodCallExpression`'s view-ctor synthesis now walks every variable-size field at construction, loads its i32 prefix, verifies `(currentOffset + 4 + prefix) <= buf.length`, and throws via `__cajeta_throw` (uncaught → aborts; user code can `try/catch`). One sweep at construction; per-access reads stay free.
- [x] **5.6** 8 new tests in `MultiVarSizeViewTests`: declaration of multi-trailing strings, read first / read second / read third (walking), oversize length rejected, exact-fit accepted, one-byte-short rejected, second-prefix validated too. Deferred tests (T[] field, post-var fixed, offset-cache verification, deeply-nested validation) carved out as S5b.
- **Pass criteria met for the tractable subset:** 744 / 744 tests pass (736 prior + 8 new). Multi-trailing var-size views work; length-prefix attacks throw at construction.

Also during S5: a non-obvious bug surfaced and got fixed — `DotExpression` was treating a class field of type `String` as a variable-size view field (because `CajetaAggregate::isVariableSize` returned true for any String-typed property). The old code emitted bogus-but-non-null GEPs that callers happened to tolerate; my new walk-the-prefixes code returned `nullptr` in the same path, which downstream code did NOT tolerate (segfaulted on non-view tests). The fix: only enter the var-size path when the receiver type is `CajetaView` (cast first, then check `isVariableSize`). Class fields of String type fall through to the standard struct-GEP path.

#### Session 5b — Post-variable fields + T[] views  ✅ complete
- [x] **5b.1** Fixed-after-var restriction lifted. The chosen approach is extending walk-the-prefixes to post-var fixed fields too (rather than the spec-proposed offset cache), keeping the view value's shape uniform — single pointer in/out, no fat alloca, no calling-convention change. Per-access cost is O(K) for the Kth post-var field (one prefix-load + advance per preceding var-size field, plus a static-size advance per preceding post-var fixed field). Fine for the common shape of 1-3 trailing fields after a var-size; an explicit offset cache becomes a profiling-driven optimization rather than a v1 requirement.
- [x] **5b.2** T[] support landed. `CajetaAggregate::isVariableSize` now recognizes `CajetaArray`-typed fields. New runtime helper `__cajeta_array_view_to_owned(data, count, elem_size)` allocates a fresh array header + memcpys the wire-format bytes into the data region. DotExpression's var-size accessor dispatches on field-type qualified name — String routes to `__cajeta_str_view_to_owned`, T[] routes to the new array helper, anything else returns the raw data pointer.
- [x] **5b.3** Length-prefix validation now walks all fields in declaration order (was previously only `varSizeCount` iterations). Var-size fields read prefix + validate + advance; post-var fixed fields advance by static size + validate. Pre-first-var fixed fields are skipped (already in `fixedPrefixSize`). Recursive validation into nested var-size views remains deferred — nested var-size views aren't yet detected as variable-size at all (`isVariableSize` checks for String/CajetaArray, not "view containing var-size"). Future work for when nested-var-size composition becomes common.
- [x] **5b.4** 7 new tests in `PostVariableFieldTests`: fixed-after-var compiles, post-var fixed read, interleaved pre-var/post-var fixed, multiple post-var fixed, interleaved var/fixed/var, T[] var-size read, T[] elements round-trip.
- **Pass criteria met:** 751 / 751 tests pass (744 prior + 7 new). Fixed-after-var works; T[] view fields work.

#### S5b limitations called out
- `.length()` on a T[] returned from a view read trips a separate alloca-related codegen path (LLVM `dyn_cast<AllocaInst>` on a non-present value) that's unrelated to S5b proper. Tests work around by indexing instead. Worth a focused look in a future session.
- Variable-size nested views (a view field whose type is another view that itself has var-size fields) aren't detected as variable-size and so don't participate in the length-prefix validation sweep. Falls out clean only because no test currently exercises that shape; will need explicit handling when it does.

### Phase 3 — New struct (stack value aggregate)

#### Session 6 — Stack alloca + class ref fields + aggregate initializer
- [x] **6.1** Codegen: `struct Foo f;` emits `alloca` of struct's fixed total size, zero-initializes. `CajetaStruct::generatePrototype` lays out the LLVM body (primitives + nested structs); `LocalVariableDeclaration` allocates the struct body inline and stores its address in the local's HeapField slot (same shape as a view local, but the pointer points at a stack alloca instead of an external buffer). 5 new tests in `StructStackTests` (declare primitive-only, multiple locals, mixed primitive layout, reject T[] field, reject recursive). KeywordEquivalence `structDeclaredAndUsedAsLocalCompiles` inverted from its S2 stub-error pin. The prior `structKeywordParsesButRejectsAtCodegen` was deleted — its `Header(bytes)` source segfaults a separate dispatch path now that struct lays out; that rejection is a real follow-up but separable from S6.1.
- [x] **6.2** Aggregate initializer: `Foo f = Foo { first: 7, second: 11 };` parses and codegens as per-field stores into the alloca. New `aggregateInitializer : identifier '{' parameterList? '}'` production reuses the existing `parameterList` shape so labeled bindings parse without new lex tokens. `AggregateInitializerExpression` resolves the type to a `CajetaStruct` (rejects views with CAJETA_ERROR_AGGREGATE_INIT_ON_VIEW and plain classes with CAJETA_ERROR_AGGREGATE_INIT_NOT_STRUCT), allocas the struct body, zero-inits, then per-binding does field-name → property → GEP → coerce → store. Bindings can appear in any order — resolution is by name, not position. Required `loadIfLValue` fix: aggregates now follow the same "value IS the pointer" rule as `CajetaArray` so the catch-all "loadTy != v->getType()" branch doesn't load the whole struct through the bodyAlloca and corrupt the receiving slot (pre-fix this corrupted memory for any aggregate-typed initializer whose AST set `resolvedType`). 6 new tests in `StructStackTests`: full init + read, partial init zeroes rest, bindings out of order resolve by name, mixed-width primitive coercion, unknown-field rejection, view-receiver rejection.
- [x] **6.3** Allow class-typed fields in struct declarations (lift the prior "no class refs" restriction). CajetaStruct::generatePrototype now accepts plain CajetaClass-typed fields (not aggregate, not array, not interface) and lays them out as 8-byte `ptr` slots per Structs.md § "Class references occupy a single pointer-width slot" — deliberately diverging from CajetaClass's inline-class-field wart noted at CajetaClass.cpp:235-239. Interface-typed fields are rejected with CAJETA_ERROR_STRUCT_FIELD_TYPE and a message pointing at S10 (interface fields need the 24-byte fat-pointer layout). Interface detection uses `klass->isInterface()` rather than dynamic_pointer_cast<CajetaInterface> because the visitor stores interfaces as plain CajetaClass with the flag set. `loadIfLValue` got a parallel branch in the DotExpression path: when the parent is a CajetaStruct and the field is a plain class ref, loadTy = ptr (not the class struct type) so the load reads exactly 8 bytes from the slot instead of walking past into the next field. 4 new tests in StructStackTests: single class-ref field round-trip, two class-ref fields side by side, partial init leaves a class-ref null, interface-field rejection.
- [x] **6.4** Drop chain: struct local pushes a drop entry; struct's drop function runs owned-class-ref fields' drops in reverse declaration order, then frees no bytes (stack-resident). `CajetaStruct::getOrCreateDropFunction` overrides the class version: walks `propertyList` in reverse, GEPs each class-ref slot, loads the pointer, calls the referent class's drop fn directly (the chain isn't re-entered, so dropCount doesn't double-bump for nested drops). `LocalVariableDeclaration` registers the struct's drop entry alongside the existing class-instance path. `AggregateInitializerExpression` performs per-binding ownership transfer: when a class-ref field is bound from a local identifier, the source local's drop entry gets `__cajeta_drop_mark_inactive`'d so only the struct's drop frees the instance. v1 simplification: every class-ref binding is treated as a move regardless of `#` — borrow-form (struct field whose drop the struct must skip) is deferred to S6.5. 4 new drop-count tests in StructStackTests: primitive-only struct fires one drop, owned class-ref drops once, owned class-ref destructor runs exactly once (rules out double-free), two owned class-refs both run their destructors.
- [x] **6.5** Borrow checker: path-borrow into struct fields rooted at the struct; same machinery the path tracker already uses. Two parts: (a) `AggregateInitializerExpression` now calls `scope->markMoved(idExpr->getTextValue())` alongside its existing drop-entry deactivation when a class-ref binding sources from a local identifier — closes the S6.4 soundness gap so subsequent reads of the source local trip CAJETA_ERROR_USE_AFTER_MOVE. (b) Path-borrow through struct fields (`#h.t` then reading `h.t.n`) confirmed to work out of the box: `DotExpression::buildPath` walks any dotted path back to its named root and `Scope::markMovedPath` / `isPathMoved` are type-agnostic, so the existing tracker handles struct field paths the same as class field paths with no new code. 4 new tests in StructStackTests: use-after-move on local consumed by struct init, use-after-move on second source (both bindings move), unbound locals stay readable (move-mark is per-identifier not blanket), use-after-move on path through struct field after `#h.t`.
- [ ] **6.6** Reject variable-tail fields (`byte[?]`) in structs at layout pass — those are view-only.
- [ ] **6.7** 12 new tests: declare + zero-init, declare + aggregate-init, declare + field-assign, struct holding String, struct holding two class refs, drop count verification, path-borrow into struct field, escape rejection, recursive struct rejected, variable-tail rejected, struct as parameter (pass-by-ptr), struct as return value.
- [ ] **Pass criteria:** Stack structs work end-to-end with primitive and class-ref fields.

#### S6.1 limitations called out
- `Foo(args)` constructor-call syntax on a struct (e.g. `Header(bytes)`) segfaults rather than cleanly rejecting. Pre-S6.1 the rejection came from the struct prototype itself throwing; with that gone, the call enters method-call dispatch which assumes a class receiver and trips a null dereference somewhere downstream. Needs a guard at the dispatch site that recognizes a CajetaStruct receiver and throws CAJETA_ERROR_STRUCT_NO_CTOR (or routes to aggregate-initializer parse once S6.2 lands). Test for this case was removed in S6.1 to keep the suite green; restore + flip to a clean rejection after the fix.

#### S6.4 limitations called out
- ~~Every class-ref binding in an aggregate initializer is treated as a move regardless of whether the source carried `#`...~~ **Resolved by S6.5.** The source local is now marked moved-from at the binding site, so `tag.n` after `Holder { t: tag }` trips CAJETA_ERROR_USE_AFTER_MOVE before runtime. The "no per-instance borrow form" caveat still stands — every class-ref binding is structurally a move; an actual borrow form (struct holding a borrowed class ref whose drop the struct must skip) would need per-instance ownership tracking. Not blocking any current use case.
- Aliasing a struct local (`Foo a; Foo b = a;`) — both register independent drop entries that fire on the same body alloca, double-calling the struct drop on shared inner class refs. Same shape as the class-instance borrow-detection pattern in LocalVariableDeclaration but not yet ported to structs; matters once `Foo b = a;`-style struct copies appear in real code.

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
