# ToDo

Working tracker for the next chunks of compiler work. Replaces the per-rollout status files now that the struct/view + memory-model rollouts are both complete (archived in `cajeta-docs/history/`).

Convention: each entry is a brief description, why it matters, where it bites today, and any pointer at where the design discussion lives. Mark `[x]` when complete; promote items into a session/rollout doc if the work warrants one.

---

## Current state (2026-05-17, late session)

Tree at 802/802 enabled (+2 DISABLED_ documenting the static-field landing).

Recently landed (Phase 7 + P6.5/P6.7 + memory-model gap pass + P1 push):
- `struct` is a transitional alias for `class`; CajetaAggregate retired.
- `(T) -> void` parses as a method-parameter type; lambda-arg expectedType inference works.
- `cajeta.lang.Stream.forEach` + `cajeta.collection.ArrayList<T>` landed.
- `Optional.get()` throws `CAJETA_ERROR_NONE_UNWRAP` on empty.
- **[GAP 1] Virtual dispatch on drop — done.** Vtable header carries `drop_fn` slot; `__cajeta_class_virtual_drop` loads through the dynamic type's vtable.
- **[GAP 2] Block-exit drops — verified done** (already worked).
- **[GAP 3] TLS / per-fiber drop chain — verified done** (already worked).
- **[GAP 4] Alias-mutation borrow check — done.** `Scope::liveBorrows` populated at LocalVariableDeclaration for pointer-shaped path-read initializers; `BinaryOpExpression`'s assignment branch consults `findInvalidatingBorrow` and throws `CAJETA_ERROR_USE_AFTER_MOVE` when the write path overlaps a live borrow. Pinned by `test/parser/AliasMutationBorrowTests.cpp` (3 tests).
- **[GAP 5] Owned-string drops — done.** `LocalVariableDeclaration` recognizes binary `+` lowered to `__cajeta_str_concat` plus the routed string method intrinsics (substring/toUpperCase/toLowerCase/trim/replace) and registers `__cajeta_free` as the local's drop fn. Pinned by `test/parser/OwnedStringDropTests.cpp` (4 tests).

Stack vs heap class instantiation is fully working: `heap T(args)`, `stack T(args)`, `heap T { … }`, `stack T { … }`, all with vtable init + ctor invocation + correct virtual destructor dispatch. See `test/parser/UnifiedClassSyntaxTests.cpp:44,137,184,249,317,576`, `test/parser/ClassDropTests.cpp`, and `test/parser/VirtualDropDispatchTests.cpp`.

---

## Remaining work — single prioritized list

Priority is rough effort × user-visible correctness impact.

### Priority 1 — open correctness gaps

1. **Static class fields not yet implemented — multi-session.** `public static int32 total = 0;` parses but emits no LLVM global. STATIC modifier is honored for methods only. Lambdas reading/writing statics segfault because the field reference resolves to nothing. Fix needs: a `StaticField` type alongside StackField/HeapField, per-class static-property global emission (with optional initializers in `<clinit>`-style globals), and `DotExpression` / `IdentifierExpression` resolution to look up class-static-properties when the LHS resolves to a `CajetaClass`. Spec'd by `test/parser/LambdaStaticCaptureTests.cpp` (`DISABLED_`).

### Priority 2 — language surface

2. **More collections — multi-session.** HashSet (HashMap-backed thin wrapper), HashMap.entries/keys/values() returning Streams, LinkedList, `Collector<T,R>` + `cajeta.lang.Collectors`. Each is its own piece.

3. **Stream lambda combinators — multi-session.** map, filter, flatMap, take, skip, peek, fold, reduce, anyMatch, allMatch, noneMatch, findFirst, collect. Each is its own concrete `*Stream` wrapper class plus the method on `Stream<T>`. The forEach pattern generalizes; combinators that return a new stream need wrapper construction.

4. **Generic-static-factory call syntax — needs method-level generics first.** `Optional<int32>.Some(42)` doesn't parse. Grammar rejects `public static <T> Box of(T arg)`. Add `typeParameters?` to `methodDeclaration`, then wire visitor + dispatch.

### Priority 3 — completeness / cleanup

5. **P6.6 chained-form completion — ~1 session.** `xs.stream().count()` direct chain. Setting `resolvedType` on the inner stream MCE in generateCode breaks ~100 unrelated tests; cleaner path is to thread the user module into `TemplateInstantiator`'s structures map or override `resolveTypes` to do the lookup without instantiation.

6. **P3c switch/loops/try-catch DA merging — 0.5 sessions each.** Implementation pattern is clear from P3a/P3b; deferred until consumed.

7. **Phase 7 cleanup — 0.5 sessions, not urgent.** Strip the 15 dead `dynamic_pointer_cast<CajetaStruct>` expressions and delete CajetaStruct.h. Cosmetic.

8. **Restore lost test coverage from Phase 7 — incremental.** The 9 deleted struct test files contained ~105 tests. Many exercised happy-path behavior still valid under the unified model.

### Memory-model carry-overs (lower priority, design-doc only)

- **No automatic field drops.** Owned heap fields require explicit release in the user destructor. Rust auto-generates these; we don't yet. Doc: `MemoryModel.md:138`.
- **No `super.~Class()` chaining.** Now reachable via virtual dispatch (Gap 1) — needs design pass for whether the base destructor should chain implicitly. Doc: `MemoryModel.md:139`.
- **Multi-parameter borrow-return needs lifetime annotations.** Multi-input free functions can't return a borrow. Rust-style explicit lifetimes would lift it. Doc: `MemoryModel.md:307`.

---

## Done

(See "Current state" above for the running list; older entries below.)
