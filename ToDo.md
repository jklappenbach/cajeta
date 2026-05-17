# ToDo

Working tracker for the next chunks of compiler work. Replaces the per-rollout status files now that the struct/view + memory-model rollouts are both complete (archived in `cajeta-docs/history/`).

Convention: each entry is a brief description, why it matters, where it bites today, and any pointer at where the design discussion lives. Mark `[x]` when complete; promote items into a session/rollout doc if the work warrants one.

---

## Current state (2026-05-17, end-of-session)

Tree at 891/891 — zero disabled tests. (L-03 MI follow-up: 8 of 9 surfaced gaps shipped + polymorphic dispatch through non-first-parent-typed bindings via secondary vtables + offset thunks. P6.1 chained-form completion verified already shipped — basic `xs.stream().count()` / `.forEach(λ)` direct chains work today; pinned by `test/parser/ChainedFormTests.cpp` (3 regression-guard tests). P6.2 non-literal static initializers shipped via per-class clinit-style functions registered with `llvm.global_ctors`; pinned by 3 new `StaticFieldTests` (`computedIntInitializer`, `initReferencesAnotherStatic`, `multiTermArithmeticInitializer`). Remaining: Gap 9 `super[Base].method()` disambiguation, diamond / virtual-base machinery, and other upcast sites (assignment, return, params) still need `adjustForUpcast` threaded through.)

Recently landed (Phase 7 + P6.5/P6.7 + memory-model gap pass + Priority 1 push + mode/debug-features push + P1.1 template-field codegen + P1.1 follow-ups + inline-MCE-as-ctor-arg + intermediate Stream combinators):
- `struct` is a transitional alias for `class`; CajetaAggregate retired.
- `(T) -> void` parses as a method-parameter type; lambda-arg expectedType inference works.
- `cajeta.lang.Stream.forEach` + `cajeta.collection.ArrayList<T>` landed.
- `Optional.get()` throws `CAJETA_ERROR_NONE_UNWRAP` on empty.
- **[GAP 1] Virtual dispatch on drop — done.** Vtable header carries `drop_fn` slot; `__cajeta_class_virtual_drop` loads through the dynamic type's vtable.
- **[GAP 2] Block-exit drops — verified done** (already worked).
- **[GAP 3] TLS / per-fiber drop chain — verified done** (already worked).
- **[GAP 4] Alias-mutation borrow check — done.** `Scope::liveBorrows`; assignment-site check throws on prefix overlap. Pinned by `test/parser/AliasMutationBorrowTests.cpp` (3 tests).
- **[GAP 5] Owned-string drops — done.** Owned-string shapes (binary `+`, routed string method intrinsics) register `__cajeta_free`. Pinned by `test/parser/OwnedStringDropTests.cpp` (4 tests).
- **Static class fields — done.** `CajetaClass::getOrCreateStaticFieldGlobal` emits LLVM globals named `<class.canonical>.<field>`; DotExpression resolves class-name LHS via `canonicalMap`. Literal initializers (`= 100`, `= -7`, floats) constant-fold into the global's initializer. Lambdas reading/writing statics work because statics resolve as globals, not captures. Pinned by `test/parser/StaticFieldTests.cpp` (9 tests) + `test/parser/LambdaStaticCaptureTests.cpp` (2 tests).
- **Automatic field drops — done.** Per-thread live-allocation set discriminates owner vs alias at drop time; auto-drop calls type-specific helpers that atomically claim out of the set so the second drop attempt on an aliased address (ArrayStream.data aliasing ArrayList.data, Optional<Hello>.value aliasing a local) silently no-ops. Doctrine in `cajeta-docs/stdlib/FieldOwnership.md`; spec at `cajeta-docs/stdlib/MemoryModel.md` § Destructors. Pinned by `test/parser/AutoFieldDropTests.cpp` (6 tests) + `test/parser/FieldOwnershipAliasingTests.cpp` (6 tests).
- **Compiler mode infrastructure — done.** `CompilerMode` enum (Debug, DebugRelease, Release, Fast, Minimal) + `CompilerFlags` struct with 13 toggle fields; CLI parses `--mode=`, flavor aliases, and per-feature overrides. Threaded through `Compiler` → `CajetaModule`. Doc: `cajeta-docs/CompilerModes.md`.
- **Source-tagged drop-chain entries — done.** When `flags.sourceTags` is on (default under `--debug`), every drop-chain push carries the alloc-site file + line via `cajeta_drop_entry_debug` (40 B vs 32 B base). Pinned by `test/parser/SourceTaggedDropTests.cpp` (6 tests).
- **SIGABRT handler with chain walk — done.** Runtime constructor installs the handler at load; on abort, dumps the per-thread drop chain (with source tags when available) and chains to the previous handler.
- **Template-instantiation field codegen — done.** All class-typed fields (templated or plain) now lay out as `ptr` rather than the inline class body; matches the `pass class by pointer` rule in Method.cpp. Also fixed companion issues: BinaryOpExpression loadIfLValue + BINARY_OP_ASSIGN slot-type unified to `ptr` for class refs, and MethodCallExpression receiver-load now load-throughs chained DotExpression field GEPs for class-ref receivers (`this.field.method()`). Unblocks the wrapper-stream pattern: `TakeStream<T>` landed as the first intermediate combinator with a `Stream<T> source;` field. Pinned by `test/parser/StreamIntermediateTests.cpp` (5 tests).
- **Subtype-aware ctor / method param matching — done.** `resolveMethod` now falls back from exact canonical-key lookup to a positional scan that walks each arg's `superClasses` via BFS, returning the most specific match. Lets `ArrayStream<int32>` args bind to `Stream<int32>` params (ctor or otherwise) without an explicit upcast local. Pinned by `test/parser/SubtypeArgLookupTests.cpp` (4 tests, both user-defined hierarchy and stdlib templates).
- **Intrinsic-result upcast JIT symbol resolution — done.** A user method body that triggers a stdlib template instantiation mid-codegen (e.g. `xs.stream()` lowers to `ArrayStream<int32>::ArrayStream(...)`) used to leave that instantiation's method bodies un-emitted because the Phase 2 loop had already swept stdlib. The Phase 2 (and Phase 1) walk now loops until the total method count across all modules stops growing — `Method::getLlvmFunctionType` and `Method::generateCode` are both idempotent, so revisiting already-emitted methods is a no-op. Applied to both `Compiler::compile` and the JIT test harness. Pinned by `test/parser/UpcastInitializerTests.cpp` (2 tests).
- **Inline-MCE-as-ctor-arg — done.** `heap T(xs.stream(), n)` or `heap T(Mk.make(xs), n)` used to crash because `MethodCallExpression` never set its own `resolvedType`. ClassCreatorRest fell back to `CajetaType::of(value)` which returns the generic `pointer` type for an opaque-pointer Value; `Method::buildGeneric` then built the wrong lookup key, the lookup failed, and downstream code crashed. Fix mirrors `NewExpression::resolveTypes`: MCE now pins `resolvedType` from the called method's return type (both the `.stream()` intrinsic and the general dispatch path), and `loadIfLValue` gets the same NewExpression-style carve-out — for MCE-typed expressions the returned pointer IS the language-level value and shouldn't be load-throughed. Pinned by `test/parser/InlineCtorArgTests.cpp` (3 tests covering intrinsic, user-method, and nested-heap forms).
- **Function-typed field invocation (`this.fn(args)`) — done.** MCE now detects when the receiver is a class and the called name resolves to a `CajetaFunctionType` field; loads the closure record (same `{ ptr fn, ptr captures, ptr drop_fn }` layout the local-lambda path uses) and indirect-calls. Previously the dispatch fell through to ordinary method lookup, found nothing, returned null, and downstream codegen silently substituted defaults (`i1 false` for boolean-returning lambdas, garbage for everything else). Unlocked by the wrapper-stream pattern across all intermediate combinators.
- **Intermediate Stream combinators (P2.2 second slice complete) — done.** Landed `SkipStream`, `FilterStream`, `MapStream<T, R>` (cross-type), `PeekStream` (side-effecting passthrough), and `FlatMapStream<T, R>` (nested-stream flattening with inter-pull state). All inherit from `Stream<T>` (or `Stream<R>` for type-changing variants) and use the wrapper-stream pattern unlocked by P1.1. Pinned by `test/parser/StreamIntermediateTests.cpp` (19 tests total: 5 take + 4 skip + 3 filter + 3 map + 2 peek + 2 flatMap).

Stack vs heap class instantiation is fully working: `heap T(args)`, `stack T(args)`, `heap T { … }`, `stack T { … }`, all with vtable init + ctor invocation + correct virtual destructor dispatch. See `test/parser/UnifiedClassSyntaxTests.cpp:44,137,184,249,317,576`, `test/parser/ClassDropTests.cpp`, and `test/parser/VirtualDropDispatchTests.cpp`.

---

## Remaining work — single prioritized list

Priority is rough effort × user-visible correctness impact.

### Priority 1 — compiler infrastructure

P1.1 (compiler mode infrastructure), P1.2 (annotation argument capture), P1.1-template-field-codegen, P1.1-subtype-ctor-lookup, P1.1-intrinsic-upcast-jit, and inline-MCE-as-ctor-arg all landed. The annotation-arg machinery in `Annotatable` (typed `getString`/`getInt`/`getBool`/`getClassRef`/`getStringList`/`getIntList` accessors) + `CajetaLlvmVisitor::parseAnnotationInstance` populates args for every annotation site. Live consumers: `@Order(n)`, `@Component(name=...)`, `@Inject(name=..., allocate=...)`, `@SuppressLint(...)`, `@Native(value=...)`. New annotations that take args plug into the same machinery — no new infrastructure needed.

No open P1 items.//

### Priority 2 — language surface

1. **More collections — multi-session.** `[mode-agnostic]` — HashSet (HashMap-backed thin wrapper), `HashMap.entries()/keys()/values()` returning Streams (`HashMap` itself already exists in `runtime/src/cajeta/collection/HashMap.cajeta`), LinkedList, `Collector<T,R>` + `cajeta.lang.Collectors`. Each is its own piece.

2. **Stream lambda combinators — mostly landed.** `[mode-agnostic]` — Terminals **anyMatch, allMatch, noneMatch, findFirst, reduce** landed (pinned by `test/parser/StreamTerminalTests.cpp`, 13 tests). Intermediate combinators **take, skip, filter, map, peek, flatMap** all landed (pinned by `test/parser/StreamIntermediateTests.cpp`, 19 tests). Remaining: terminal **collect** (needs `Collector<T,R>` from P2.1) + method-level-templated **fold<R>** (needs P2.3 method-level type parameters). Also blocked on the chained-form path (P6.6) before users can write `xs.stream().filter(p).map(f).count()` — today each step needs its own ctor + local.

3. **Templated-static-factory call syntax — needs method-level template parameters first.** `[mode-agnostic]` — `Optional<int32>.Some(42)` doesn't parse. Grammar rejects `public static <T> Box of(T arg)`. Add `typeParameters?` to `methodDeclaration`, then wire visitor + dispatch.

4. **`@Encoding(EncoderClass)` for views — ~1.5 sessions.** `[mode-agnostic]` — `Encoder<T>` interface in `cajeta.wire`; view constructor synthesizes a `Encoder.decode(bytes)` call; `toBytes()` synthesizes `Encoder.encode(this)`. Mutually exclusive with `@BigEndian`/`@LittleEndian`/`@HostEndian`/`@Align` (encoder owns wire layout). Annotation-arg machinery already in place; consume via `findAnnotation("Encoding")->getClassRef("value")`. Doc: `cajeta-docs/stdlib/Annotations.md` § `@Encoding` for views.

5. **Multiple inheritance gaps (L-03 follow-up) — landed except Gap 9 + diamond.** `[mode-agnostic]` — 8 of 9 surfaced gaps shipped + polymorphic dispatch through non-first-parent-typed bindings; pinned by `test/parser/MultipleInheritanceGapTests.cpp` (14 tests, all enabled). Tree at 885/885. Features.md L-03 stays `partial` (true diamond / virtual-base remain; upcast adjustment is only wired for `LocalVariableDeclaration` — assignment expressions, return statements, parameter passing still need `CajetaClass::adjustForUpcast` threaded through), L-19 `shipped`.
   1. ✅ **Implicit super-ctor chains ALL parents.** `Method.cpp` removed the `break;  // single inheritance chain` at the end of the implicit-super loop. Test: `twoParentsBothCtorsRun` (uses a static counter to avoid the Gap 8 field-slot issue).
   2. ✅ **Multi-parent field layout / lookup.** Rewrote `CajetaClass::getFieldLlvmIndex` to walk the layout in the same DFS order as `appendInherited`, rather than returning the standalone-struct index (which only worked for single inheritance). Test: `twoParentsFieldReadBackThroughChildWrite`.
   3. ✅ **Diamond field dedup.** Already worked by symmetry: `appendInherited` allocates duplicate slots but name resolution lands on the same slot for both casts, so writes and reads are self-consistent. Real layout-level dedup is a cleanup task but not user-visible. Test: `diamondCommonAncestorFieldShared`.
   4. ✅ **Abstract-method enforcement.** Three parts: (a) `visitMethodDeclaration` now accepts `methodBody : ';'` (was throwing `bad_any_cast`); (b) sets `method->setAbstract(true)` for body-less methods; (c) `buildVirtualTable` raises `CAJETA_ERROR_ABSTRACT_NOT_IMPLEMENTED` when this class has no abstract methods of its own (heuristic "self is concrete") and any inherited abstract lacks an override. Tests: `unimplementedAbstractMethodRejected`, `abstractMethodWithOverrideCompilesAndDispatches`.
   5. ✅ **`super.method()` upcall (L-19).** Replaced `UnsupportedExpression` with new `SuperExpression` (resolves to the first declared parent, code-gens the same `this` pointer). `MethodCallExpression` detects `SuperExpression` receivers and passes `forceDirectCall=true` through `CajetaClass::invokeMethod` to bypass vtable. Test: `superMethodCallReachesParent`. Stale `UnsupportedExpressionTests.superCallThrowsNotImplemented` removed.
   6. ✅ **Explicit `super(args)` in ctor.** `MethodCallExpression` ctor now handles the `methodCall : SUPER '(' parameterList? ')'` alternative (was null-derefing on `ctx->identifier()->getText()`); generateCode routes the call through the parent's matching ctor. Implicit no-arg super is naturally skipped when an args-only parent ctor exists. Test: `explicitSuperCtorWithArgs`.
   7. ✅ **Unsatisfied-interface enforcement.** `buildVirtualTable` raises `CAJETA_ERROR_INTERFACE_NOT_IMPLEMENTED` when an interface method has no matching concrete impl (was silently omitting the vtable entry). Test: `unimplementedInterfaceMethodRejected`.
   8. ✅ **Multi-parent ctor/method field-slot baking — SHIPPED via per-parent sub-object layout.** `CajetaClass::generatePrototype` now lays each class out as `{ vtable_primary, first-parent-sub-object-content (shares vptr), vtable_secondary_for_next_parent, ..., own-fields }`. Every non-first parent gets its own vtable slot at the start of its sub-object so a pointer adjusted to that offset looks structurally identical to that parent standalone — the parent's pre-compiled ctor/method IR uses its own slot indices and lands on the right fields. A `subObjectSlotMap` keyed by ancestor class records where each ancestor's sub-object begins; `getSubObjectByteOffset` computes the byte offset via `DataLayout`. Call-site `this` adjustments now fire in three places: Method.cpp's implicit super-ctor emission, MethodCallExpression's explicit `super(args)` path, and `CajetaClass::invokeMethod` for any method whose resolved declaring class differs from the receiver class. `getFieldLlvmIndex` mirrors the same sub-object walk so own/inherited field GEPs match the layout exactly. Tests: `twoParentsBothInstanceFieldsFromParentCtors`, `parentMethodReadsOwnFieldOnSubclassInstance`, `twoParentsMultipleFieldsEach`. Remaining caveat: polymorphic dispatch through a non-first-parent-typed binding (e.g. `B b = c` then `b.foo()` where C extends A, B) needs secondary-vtable thunks for cross-class override correction; the secondary vptr slots are currently zeroed and that path will null-deref until it lands. True diamond / virtual-base machinery (A duplicated when it's a non-first-parent on either side) is the other piece — both are now the L-03-polymorphism follow-up.
   9. ⏸ **`super[Base].method()` disambiguation — DEFERRED.** With MI, two parents can both define `m()`; a child that wants the A-version vs the B-version has no syntax today. Needs a grammar change (`super[Base]` selector after `super`) plus per-parent selection in MCE. Not currently blocking any stdlib row.
   10. ✅ **Polymorphic dispatch through a non-first-parent-typed binding — SHIPPED.** Three pieces:
       - **Upcast adjustment at `LocalVariableDeclaration`.** After `HeapField::getOrCreateAllocation` stores the RHS pointer, we reload, shift by `srcClass->getSubObjectByteOffset(dstClass)`, and store back. The static helper lives at `CajetaClass::adjustForUpcast` so future call sites (assignment expressions, return statements, parameter passing) can reuse it.
       - **Secondary vtables.** `CajetaClass::getOrCreateSecondaryVTable(parent)` lazily synthesizes a vtable whose LLVM type matches the parent's standalone vtable (same hash entries / sort order). For each parent vtable entry: if this class overrides the method (declaring class differs), the entry points at an offset thunk; otherwise it points at the parent's own function directly (the receiver is already a parent-sub-pointer and is what the parent's function expects). `ClassCreatorRest::generateCode` initializes each non-first-parent vptr slot with the matching secondary vtable global at `new` time, via `klass->getNonFirstSubObjects()`.
       - **Cross-class override thunks.** `CajetaClass::synthesizeOffsetThunk(parent, impl, parentOffsetInThis)` emits a tiny private function: receives `this` as the parent-sub-pointer, GEPs `this - parentOffsetInThis` to recover the most-derived pointer, then tail-calls the override. The thunk's signature matches the override exactly so the dispatch site doesn't need to know it's a thunk. Tests: `dispatchThroughNonFirstParentTypedBinding`, `overrideThroughNonFirstParentTypedBinding`, `dispatchThroughFirstParentTypedBindingUnchanged`. `getNonFirstSubObjects` walks the layout deterministically (not the `subObjectSlotMap`, which collapses diamond ancestors) so each non-first-vptr slot is enumerated exactly once.
   - Unblocked stdlib rows (now including the multi-parent-with-instance-fields case after Gap 8): **S-107** `Optional<T>` multi-inherits `Stream<T>`, **S-302** `ArrayList<T>` IS-A `Stream<T>`, **S-305** `HashMap<K,V>` IS-A `Stream<Pair<K,V>>`. **S-313** for-loop desugar through `.next()` is a separate language item — not strictly L-03, but reads as the same feature surface for users.

### Priority 3 — Lombok-mirror annotations (all `[mode-agnostic]`)

In Lombok's recommended adoption order:

1. **`@Getter` / `@Setter` — ~1 session.** Field-walk synthesizers. Visibility via `(level="private")`. Doc: `cajeta-docs/stdlib/Annotations.md` § Accessors.
2. **`@ToString` — ~0.5 session.** `(exclude={"...","..."})` variants. Doc: `Annotations.md` § Equality + hashing + toString.
3. **`@EqualsAndHashCode` — Rejected.  We already have @AutoHash planned, perhaps could rename to @Hash.  There's no equals on Object in Cajeta.  We have operator ==() overrides
4. **`@NoArgsConstructor` / `@AllArgsConstructor` / `@RequiredArgsConstructor` — ~1 session.** Constructor synthesizers. Doc: `Annotations.md` § Constructors.
5. **`@Data` / `@Value` — ~0.5 session.** Bundle annotations expanding into the above. Doc: `Annotations.md` § Bundles.
6. **`@NonNull` — ~1 session.** `[both]` — synthesis is mode-agnostic, but the emitted null-check composes with `--null-checks` (P5 below). Doc: `Annotations.md` § Null safety.
7. **`@Builder` — ~2 sessions.** Largest piece; synthesizes a Builder inner class + chained setters + `.build()` with `@NonNull` validation. `@Builder.Default` on a field. Doc: `Annotations.md` § Builders.
8. **`@With` — ~1 session.** Per-field copy-with mutators (`withX(value)` returns a new instance with `x` replaced). Doc: `Annotations.md` § Immutability friend.
9. **`@Cleanup("method"="close")` — Rejected, we have destructors.  

### Priority 4 — debug-mode features (CompilerModes.md phasing order)

All `[debug]`. P4.1 source-tagged drop-chain entries and P4.2 SIGABRT chain-walk handler landed; remaining items below renumbered.

1. **`--live-set=strict` — ~0.5 session.** Unbounded growth + rehash; assert on duplicate-add (catches compiler-codegen bugs in the live-set hook path). Doc: `CompilerModes.md` § `--live-set`.
2. **`--poison-free=on` — ~0.5 session.** memset freed body with sentinel pattern before glibc free. Doc: `CompilerModes.md` § `--poison-free`.
3. **`--drop-chain-validate=on` — ~0.5 session.** Per-push/pop linked-list integrity checks; assert + diagnostic on corruption. Doc: `CompilerModes.md` § `--drop-chain-validate`.
4. **`--diag-hints=on` (compile-time) — ~1 session.** "Did you mean..." for typo'd identifiers; recommend `#`-transfer when a borrow violates lifetime; suggest `@SuppressLint(...)` for noisy lints. Doc: `CompilerModes.md` § `--diag-hints`.
5. **Stack-trace capture on throw — ~1 session.** `backtrace(3)` + DWARF + source-map symbolization in the exception payload. Doc: `CompilerModes.md` § `--stack-trace-capture`.
6. **`--use-after-move-rt=on` — ~0.5 session.** Sentinel in moved slot header; trap on read. Backs up the static use-after-move tracker. Doc: `CompilerModes.md` § `--use-after-move-rt`.
7. **`--ub-traps=on` — ~0.5 session.** Trap instructions for signed overflow, divide-by-zero, oversized shift, unaligned atomic. Catches "compiler made my code do something weird" early. Doc: `CompilerModes.md` § `--ub-traps`.

### Priority 5 — release-mode features

All `[release]`.

1. **`--bounds=trap` codegen — ~0.5 session.** Skip the exception throw; emit `@llvm.trap` for the fastest bail. Doc: `CompilerModes.md` § `--bounds`.
2. **`--overflow-checks=wrapping`/`off` codegen — ~1 session.** Today integer arithmetic is implicit wrapping; make the choice explicit via the flag and wire `--overflow-checks=off` so the compiler can assume no overflow and optimize accordingly. Doc: `CompilerModes.md` § `--overflow-checks`.
3. **`--null-checks=on/off/trap` codegen — ~0.5 session.** Today null-check generation is implicit; expose the flag so users can opt out at high `--release` confidence. `@NonNull` (P3.6) integrates here. Doc: `CompilerModes.md` § `--null-checks`.
4. **`--profile-counters=on` — ~1 session.** Per-method invocation counter + wall-time tally for PGO collection. Default on under `--debug-release`. Doc: `CompilerModes.md` § `--profile-counters`.

### Priority 6 — completeness / cleanup

All `[mode-agnostic]`.

1. ✅ **P6.6 chained-form completion (basic case)** — verified shipped. `xs.stream().count()` and `xs.stream().forEach(λ)` direct chains work today, likely incidental to the earlier inline-MCE-as-ctor-arg + function-typed-field-invocation landings. Pinned by `test/parser/ChainedFormTests.cpp` (3 tests). Improved `ReturnStatement::generateCode` diagnostic for null-lowered return expressions (was hitting `dyn_cast<AllocaInst>(nullptr)` assert; now emits "[cajeta] return value lowered to null" warning). Out of scope here: longer chains like `xs.stream().filter(p).count()` fail because `Stream<T>.filter()` isn't defined — combinators must be constructed via `heap FilterStream<T>(source, pred)`. That's a stdlib-completeness gap (Priority 2 § 2), not chained-form.

2. ✅ **Non-literal static field initializers — SHIPPED.** `CajetaClass::generateStaticInitializers` emits a per-class `__cajeta_clinit_<class.canonical>` function in the class's home module that evaluates any initializer beyond the constant-folder's reach (`= 1 + 2`, `= a + 5`, `= (2*3) + (10/2)`, etc.) and stores the result into the global. Registered with `llvm.global_ctors` at default priority 65535 via `llvm::appendToGlobalCtors`. Compiler.cpp + JitTestHelper.cpp call it once after the Phase 1/2 quiescence loop completes, so expression codegen sees the final method set. JitTestHelper now calls `LLJIT::initialize(mainDylib)` explicitly so the JIT actually runs the ctors (the host-linked runtime ctors fire via process startup regardless, but pure-JIT clinits need this). IdentifierExpression gained a static-shorthand path so a bare `a` inside a static initializer (e.g. `b = a + 5`) resolves to the class's static field global. Pinned by 3 new tests in `StaticFieldTests`. Out of scope: string literals (still need global-string emission), method calls / references to other classes' statics (need ordering between cross-module clinits), aggregate / array literal initializers.

3. **P3c switch/loops/try-catch DA merging — 0.5 sessions each.** Implementation pattern is clear from P3a/P3b; deferred until consumed.

4. **Phase 7 cleanup — 0.5 sessions, not urgent.** Strip the 8 remaining `dynamic_pointer_cast<CajetaStruct>` expressions in `src/` and delete `CajetaStruct.h`. Cosmetic.

5. **Restore lost test coverage from Phase 7 — incremental.** The 9 deleted struct test files contained ~105 tests. Many exercised happy-path behavior still valid under the unified model.

### Memory-model carry-overs (lower priority, design-doc only)

- ✅ **Automatic field drops.** Landed via FieldOwnership.md § Solution B: the per-thread live-allocation set lets every heap allocation track its liveness; auto-drop calls the type-specific helper (`__cajeta_class_virtual_drop` / `__cajeta_free_array`) which atomically claims out of the set, so aliased fields no-op safely. Spec at `MemoryModel.md:138` updated; doctrine at `cajeta-docs/stdlib/FieldOwnership.md`; tests at `test/parser/AutoFieldDropTests.cpp` and `test/parser/FieldOwnershipAliasingTests.cpp`. Trade-off: use-after-free of an aliased field whose source has dropped is now the programmer's responsibility (Phase 6+ lifetime tracker can re-tighten).
- **No `super.~Class()` chaining.** Now reachable via virtual dispatch (Gap 1) — needs design pass for whether the base destructor should chain implicitly. Doc: `MemoryModel.md:139`.
- **Multi-parameter borrow-return needs lifetime annotations.  [I don't like those.  They're cumbersome and confusing.   Find other ways to solve the initial problems] ** Multi-input free functions can't return a borrow. Rust-style explicit lifetimes would lift it. Doc: `MemoryModel.md:307`.

---

## Done

(See "Current state" above for the running list; older entries below.)
