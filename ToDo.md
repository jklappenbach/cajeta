# ToDo

Working tracker for the next chunks of compiler work. Replaces the per-rollout status files now that the struct/view + memory-model rollouts are both complete (archived in `cajeta-docs/history/`).

Convention: each entry is a brief description, why it matters, where it bites today, and any pointer at where the design discussion lives. Mark `[x]` when complete; promote items into a session/rollout doc if the work warrants one.

---

## Current state (2026-05-17, late session)

Tree at 813/813 — zero disabled tests.

Recently landed (Phase 7 + P6.5/P6.7 + memory-model gap pass + Priority 1 push):
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

Stack vs heap class instantiation is fully working: `heap T(args)`, `stack T(args)`, `heap T { … }`, `stack T { … }`, all with vtable init + ctor invocation + correct virtual destructor dispatch. See `test/parser/UnifiedClassSyntaxTests.cpp:44,137,184,249,317,576`, `test/parser/ClassDropTests.cpp`, and `test/parser/VirtualDropDispatchTests.cpp`.

---

## Remaining work — single prioritized list

Priority is rough effort × user-visible correctness impact.

### Priority 1 — open correctness items

(All of the items previously listed at P1 — Gap 4, lambda static fields, Gap 5 — have landed. New P1 items will appear here as they surface.)

### Priority 2 — language surface

1. **More collections — multi-session.** HashSet (HashMap-backed thin wrapper), HashMap.entries/keys/values() returning Streams, LinkedList, `Collector<T,R>` + `cajeta.lang.Collectors`. Each is its own piece.

2. **Stream lambda combinators — multi-session.** map, filter, flatMap, take, skip, peek, fold, reduce, anyMatch, allMatch, noneMatch, findFirst, collect. Each is its own concrete `*Stream` wrapper class plus the method on `Stream<T>`. The forEach pattern generalizes; combinators that return a new stream need wrapper construction.

3. **Generic-static-factory call syntax — needs method-level generics first.** `Optional<int32>.Some(42)` doesn't parse. Grammar rejects `public static <T> Box of(T arg)`. Add `typeParameters?` to `methodDeclaration`, then wire visitor + dispatch.

### Priority 3 — completeness / cleanup

4. **P6.6 chained-form completion — ~1 session.** `xs.stream().count()` direct chain. Setting `resolvedType` on the inner stream MCE in generateCode breaks ~100 unrelated tests; cleaner path is to thread the user module into `TemplateInstantiator`'s structures map or override `resolveTypes` to do the lookup without instantiation. **Also covers `xs.stream().forEach(lambda)`** — surfaced during the static-field landing; the chained call currently swallows the second method (forEach never runs).

5. **Non-literal static field initializers — ~1 session.** Today only integer / float literals (with optional `-` prefix) constant-fold into the global's initializer. Method calls, references to other statics, computed expressions, and string literals fall back to zero. Implementation path: emit a per-module `<clinit>`-style init function registered via `llvm.global_ctors` that runs the user expression and stores into the global at module load.

6. **P3c switch/loops/try-catch DA merging — 0.5 sessions each.** Implementation pattern is clear from P3a/P3b; deferred until consumed.

7. **Phase 7 cleanup — 0.5 sessions, not urgent.** Strip the 15 dead `dynamic_pointer_cast<CajetaStruct>` expressions and delete CajetaStruct.h. Cosmetic.

8. **Restore lost test coverage from Phase 7 — incremental.** The 9 deleted struct test files contained ~105 tests. Many exercised happy-path behavior still valid under the unified model.

### Memory-model carry-overs (lower priority, design-doc only)

- ✅ **Automatic field drops.** Landed via FieldOwnership.md § Solution B: the per-thread live-allocation set lets every heap allocation track its liveness; auto-drop calls the type-specific helper (`__cajeta_class_virtual_drop` / `__cajeta_free_array`) which atomically claims out of the set, so aliased fields no-op safely. Spec at `MemoryModel.md:138` updated; doctrine at `cajeta-docs/FieldOwnership.md`; tests at `test/parser/AutoFieldDropTests.cpp` and `test/parser/FieldOwnershipAliasingTests.cpp`. Trade-off: use-after-free of an aliased field whose source has dropped is now the programmer's responsibility (Phase 6+ lifetime tracker can re-tighten).
- **No `super.~Class()` chaining.** Now reachable via virtual dispatch (Gap 1) — needs design pass for whether the base destructor should chain implicitly. Doc: `MemoryModel.md:139`.
- **Multi-parameter borrow-return needs lifetime annotations.** Multi-input free functions can't return a borrow. Rust-style explicit lifetimes would lift it. Doc: `MemoryModel.md:307`.

---

## Done

(See "Current state" above for the running list; older entries below.)
