# ToDo

Working tracker for the next chunks of compiler work. Replaces the per-rollout status files now that the struct/view + memory-model rollouts are both complete (archived in `cajeta-docs/history/`).

Convention: each entry is a brief description, why it matters, where it bites today, and any pointer at where the design discussion lives. Mark `[x]` when complete; promote items into a session/rollout doc if the work warrants one.

---

## Current state (2026-05-17, end-of-session)

Tree at 849/849 — zero disabled tests.

Recently landed (Phase 7 + P6.5/P6.7 + memory-model gap pass + Priority 1 push + mode/debug-features push + P1.1 template-field codegen):
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
- **Automatic field drops — done.** Per-thread live-allocation set discriminates owner vs alias at drop time; auto-drop calls type-specific helpers that atomically claim out of the set so the second drop attempt on an aliased address (ArrayStream.data aliasing ArrayList.data, Optional<Hello>.value aliasing a local) silently no-ops. Doctrine in `cajeta-docs/FieldOwnership.md`; spec at `cajeta-docs/MemoryModel.md` § Destructors. Pinned by `test/parser/AutoFieldDropTests.cpp` (6 tests) + `test/parser/FieldOwnershipAliasingTests.cpp` (6 tests).
- **Compiler mode infrastructure — done.** `CompilerMode` enum (Debug, DebugRelease, Release, Fast, Minimal) + `CompilerFlags` struct with 13 toggle fields; CLI parses `--mode=`, flavor aliases, and per-feature overrides. Threaded through `Compiler` → `CajetaModule`. Doc: `cajeta-docs/CompilerModes.md`.
- **Source-tagged drop-chain entries — done.** When `flags.sourceTags` is on (default under `--debug`), every drop-chain push carries the alloc-site file + line via `cajeta_drop_entry_debug` (40 B vs 32 B base). Pinned by `test/parser/SourceTaggedDropTests.cpp` (6 tests).
- **SIGABRT handler with chain walk — done.** Runtime constructor installs the handler at load; on abort, dumps the per-thread drop chain (with source tags when available) and chains to the previous handler.
- **Template-instantiation field codegen — done.** All class-typed fields (templated or plain) now lay out as `ptr` rather than the inline class body; matches the `pass class by pointer` rule in Method.cpp. Also fixed companion issues: BinaryOpExpression loadIfLValue + BINARY_OP_ASSIGN slot-type unified to `ptr` for class refs, and MethodCallExpression receiver-load now load-throughs chained DotExpression field GEPs for class-ref receivers (`this.field.method()`). Unblocks the wrapper-stream pattern: `TakeStream<T>` landed as the first intermediate combinator with a `Stream<T> source;` field. Pinned by `test/parser/StreamIntermediateTests.cpp` (5 tests).

Stack vs heap class instantiation is fully working: `heap T(args)`, `stack T(args)`, `heap T { … }`, `stack T { … }`, all with vtable init + ctor invocation + correct virtual destructor dispatch. See `test/parser/UnifiedClassSyntaxTests.cpp:44,137,184,249,317,576`, `test/parser/ClassDropTests.cpp`, and `test/parser/VirtualDropDispatchTests.cpp`.

---

## Remaining work — single prioritized list

Priority is rough effort × user-visible correctness impact.

### Priority 1 — compiler infrastructure

P1.1 (compiler mode infrastructure), P1.2 (annotation argument capture), and P1.1-template-field-codegen all landed. The annotation-arg machinery in `Annotatable` (typed `getString`/`getInt`/`getBool`/`getClassRef`/`getStringList`/`getIntList` accessors) + `CajetaLlvmVisitor::parseAnnotationInstance` populates args for every annotation site. Live consumers: `@Order(n)`, `@Component(name=...)`, `@Inject(name=..., allocate=...)`, `@SuppressLint(...)`, `@Native(value=...)`. New annotations that take args plug into the same machinery — no new infrastructure needed.

1. **Intrinsic-result upcast to parent template type — ~0.5 session.** `[mode-agnostic]` — `Stream<int32> src = xs.stream();` (where `.stream()` returns `ArrayStream<int32>`) trips a JIT symbol-resolution failure at lookup time: "Symbols not found: cajeta.lang.ArrayStream<int32>::ArrayStream(...)". Exact-type local works (`ArrayStream<int32> as = xs.stream()`), as does explicit two-step `ArrayStream<int32> as = …; Stream<int32> src = as;`. Likely a cross-module instantiation issue where the LHS type drives which module gets the ArrayStream<int32> body. Doesn't block correctness — the explicit two-step pattern works today; see `test/parser/StreamIntermediateTests.cpp` for the working shape.

2. **Subtype-aware ctor / method param matching — ~1 session.** `[mode-agnostic]` — `Method::buildGeneric` keys lookups by exact param canonical names; passing an `ArrayStream<int32>` arg where a `Stream<int32>` param is declared misses the lookup unless the local is typed exactly as `Stream<int32>`. Fix path: in `resolveMethod`'s fallback after the exact lookup fails, try alternative keys with each class-typed arg walked up through its `superClasses`. Surfaced while testing TakeStream — workaround is to declare the local at the parent type.

### Priority 2 — language surface

1. **More collections — multi-session.** `[mode-agnostic]` — HashSet (HashMap-backed thin wrapper), `HashMap.entries()/keys()/values()` returning Streams (`HashMap` itself already exists in `runtime/src/cajeta/collection/HashMap.cajeta`), LinkedList, `Collector<T,R>` + `cajeta.lang.Collectors`. Each is its own piece.

2. **Stream lambda combinators — multi-session.** `[mode-agnostic]` — Terminals **anyMatch, allMatch, noneMatch, findFirst, reduce** landed (pinned by `test/parser/StreamTerminalTests.cpp`, 13 tests). **TakeStream<T>** landed as the first intermediate combinator (pinned by `test/parser/StreamIntermediateTests.cpp`, 5 tests). Remaining intermediate combinators: skip, filter, peek, map, flatMap — same wrapper-stream shape as TakeStream. Remaining terminals: collect (needs `Collector<T,R>` from P2.1) + method-level-templated fold<R> (needs P2.3 method-level type parameters).

3. **Templated-static-factory call syntax — needs method-level template parameters first.** `[mode-agnostic]` — `Optional<int32>.Some(42)` doesn't parse. Grammar rejects `public static <T> Box of(T arg)`. Add `typeParameters?` to `methodDeclaration`, then wire visitor + dispatch.

4. **`@Encoding(EncoderClass)` for views — ~1.5 sessions.** `[mode-agnostic]` — `Encoder<T>` interface in `cajeta.wire`; view constructor synthesizes a `Encoder.decode(bytes)` call; `toBytes()` synthesizes `Encoder.encode(this)`. Mutually exclusive with `@BigEndian`/`@LittleEndian`/`@HostEndian`/`@Align` (encoder owns wire layout). Annotation-arg machinery already in place; consume via `findAnnotation("Encoding")->getClassRef("value")`. Doc: `cajeta-docs/Annotations.md` § `@Encoding` for views.

### Priority 3 — Lombok-mirror annotations (all `[mode-agnostic]`)

In Lombok's recommended adoption order:

1. **`@Getter` / `@Setter` — ~1 session.** Field-walk synthesizers. Visibility via `(level="private")`. Doc: `cajeta-docs/Annotations.md` § Accessors.
2. **`@ToString` — ~0.5 session.** `(exclude={"...","..."})` variants. Doc: `Annotations.md` § Equality + hashing + toString.
3. **`@EqualsAndHashCode` — Rejected.  We already have @AutoHash planned, perhaps could rename to @Hash.  There's no equals.  We have operator ==()
4. **`@NoArgsConstructor` / `@AllArgsConstructor` / `@RequiredArgsConstructor` — ~1 session.** Constructor synthesizers. Doc: `Annotations.md` § Constructors.
5. **`@Data` / `@Value` — ~0.5 session.** Bundle annotations expanding into the above. Doc: `Annotations.md` § Bundles.
6. **`@NonNull` — ~1 session.** `[both]` — synthesis is mode-agnostic, but the emitted null-check composes with `--null-checks` (P5 below). Doc: `Annotations.md` § Null safety.
7. **`@Builder` — ~2 sessions.** Largest piece; synthesizes a Builder inner class + chained setters + `.build()` with `@NonNull` validation. `@Builder.Default` on a field. Doc: `Annotations.md` § Builders.
8. **`@With` — ~1 session.** Per-field copy-with mutators (`withX(value)` returns a new instance with `x` replaced). Doc: `Annotations.md` § Immutability friend.
9. **`@Cleanup("method"="close")` — ~0.5 session.** try/finally synthesis around the annotated local. Doc: `Annotations.md` § Resource cleanup.

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

1. **P6.6 chained-form completion — ~1 session.** `xs.stream().count()` direct chain. Setting `resolvedType` on the inner stream MCE in generateCode breaks ~100 unrelated tests; cleaner path is to thread the user module into `TemplateInstantiator`'s structures map or override `resolveTypes` to do the lookup without instantiation. **Also covers `xs.stream().forEach(lambda)`** — surfaced during the static-field landing; the chained call currently swallows the second method (forEach never runs).

2. **Non-literal static field initializers — ~1 session.** Today only integer / float literals (with optional `-` prefix) constant-fold into the global's initializer. Method calls, references to other statics, computed expressions, and string literals fall back to zero. Implementation path: emit a per-module `<clinit>`-style init function registered via `llvm.global_ctors` that runs the user expression and stores into the global at module load.

3. **P3c switch/loops/try-catch DA merging — 0.5 sessions each.** Implementation pattern is clear from P3a/P3b; deferred until consumed.

4. **Phase 7 cleanup — 0.5 sessions, not urgent.** Strip the 8 remaining `dynamic_pointer_cast<CajetaStruct>` expressions in `src/` and delete `CajetaStruct.h`. Cosmetic.

5. **Restore lost test coverage from Phase 7 — incremental.** The 9 deleted struct test files contained ~105 tests. Many exercised happy-path behavior still valid under the unified model.

### Memory-model carry-overs (lower priority, design-doc only)

- ✅ **Automatic field drops.** Landed via FieldOwnership.md § Solution B: the per-thread live-allocation set lets every heap allocation track its liveness; auto-drop calls the type-specific helper (`__cajeta_class_virtual_drop` / `__cajeta_free_array`) which atomically claims out of the set, so aliased fields no-op safely. Spec at `MemoryModel.md:138` updated; doctrine at `cajeta-docs/FieldOwnership.md`; tests at `test/parser/AutoFieldDropTests.cpp` and `test/parser/FieldOwnershipAliasingTests.cpp`. Trade-off: use-after-free of an aliased field whose source has dropped is now the programmer's responsibility (Phase 6+ lifetime tracker can re-tighten).
- **No `super.~Class()` chaining.** Now reachable via virtual dispatch (Gap 1) — needs design pass for whether the base destructor should chain implicitly. Doc: `MemoryModel.md:139`.
- **Multi-parameter borrow-return needs lifetime annotations.** Multi-input free functions can't return a borrow. Rust-style explicit lifetimes would lift it. Doc: `MemoryModel.md:307`.

---

## Done

(See "Current state" above for the running list; older entries below.)
