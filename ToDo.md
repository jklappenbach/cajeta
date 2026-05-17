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

### Priority 1 — compiler infrastructure (foundational; unblocks the per-mode + Lombok work)

1. **Compiler mode infrastructure — ~1 session.** `[both]` — `CompilerMode` enum + `CompilerFlags` struct holding all per-feature toggles + CLI parsing for `--mode=...`, flavor flags (`--debug`, `--release`, `--fast`, `--debug-release`, `--minimal`), and per-feature overrides (`--bounds`, `--source-tags`, `--poison-free`, `--live-set`, `--drop-chain-validate`, `--ub-traps`, `--use-after-move-rt`, `--overflow-checks`, `--stack-trace-capture`, `--diag-verbosity`, `--diag-hints`, `--profile-counters`). Migrate existing `boundsCheckEnabled` into the struct. Threaded through `Compiler` → `CajetaModule` like the existing bounds-check flag. Foundation for every other mode-conditional feature. Doc: `cajeta-docs/CompilerModes.md`.

2. **Annotation argument capture for all annotations — ~1 session.** `[mode-agnostic]` — Today only `@SuppressLint` and `@Native` parse their args (`Annotatable::findAnnotation` handles names uniformly, but value extraction is per-annotation). Generalize so `@Order(2)`, `@Component(name="primary")`, `@Inject(name="primary")`, `@Builder(...)`, `@Encoding(EncoderClass)`, and all the Lombok-mirror annotation configuration parameters work. Doc: `cajeta-docs/Annotations.md` § Implementation notes; spec at `AspectModel.md` § A1.

### Priority 2 — language surface

1. **More collections — multi-session.** `[mode-agnostic]` — HashSet (HashMap-backed thin wrapper), HashMap.entries/keys/values() returning Streams, LinkedList, `Collector<T,R>` + `cajeta.lang.Collectors`. Each is its own piece.

2. **Stream lambda combinators — multi-session.** `[mode-agnostic]` — map, filter, flatMap, take, skip, peek, fold, reduce, anyMatch, allMatch, noneMatch, findFirst, collect. Each is its own concrete `*Stream` wrapper class plus the method on `Stream<T>`. The forEach pattern generalizes; combinators that return a new stream need wrapper construction.

3. **Generic-static-factory call syntax — needs method-level generics first.** `[mode-agnostic]` — `Optional<int32>.Some(42)` doesn't parse. Grammar rejects `public static <T> Box of(T arg)`. Add `typeParameters?` to `methodDeclaration`, then wire visitor + dispatch.

4. **`@Encoding(EncoderClass)` for views — ~1.5 sessions.** `[mode-agnostic]` — `Encoder<T>` interface in `cajeta.wire`; view constructor synthesizes a `Encoder.decode(bytes)` call; `toBytes()` synthesizes `Encoder.encode(this)`. Mutually exclusive with `@BigEndian`/`@LittleEndian`/`@HostEndian`/`@Align` (encoder owns wire layout). Depends on P1.2 (annotation arg capture). Doc: `cajeta-docs/Annotations.md` § `@Encoding` for views.

### Priority 3 — Lombok-mirror annotations (all `[mode-agnostic]`; depend on P1.2)

In Lombok's recommended adoption order:

1. **`@Getter` / `@Setter` — ~1 session.** Field-walk synthesizers. Visibility via `(level="private")`. Doc: `cajeta-docs/Annotations.md` § Accessors.
2. **`@ToString` — ~0.5 session.** `(exclude={"...","..."})` variants. Doc: `Annotations.md` § Equality + hashing + toString.
3. **`@EqualsAndHashCode` — ~1 session.** Subsumes `@AutoHash` as a soft-deprecation alias. Doc: same.
4. **`@NoArgsConstructor` / `@AllArgsConstructor` / `@RequiredArgsConstructor` — ~1 session.** Constructor synthesizers. Doc: `Annotations.md` § Constructors.
5. **`@Data` / `@Value` — ~0.5 session.** Bundle annotations expanding into the above. Doc: `Annotations.md` § Bundles.
6. **`@NonNull` — ~1 session.** `[both]` — synthesis is mode-agnostic, but the emitted null-check composes with `--null-checks` (P5 below). Doc: `Annotations.md` § Null safety.
7. **`@Builder` — ~2 sessions.** Largest piece; synthesizes a Builder inner class + chained setters + `.build()` with `@NonNull` validation. `@Builder.Default` on a field. Doc: `Annotations.md` § Builders.
8. **`@With` — ~1 session.** Per-field copy-with mutators (`withX(value)` returns a new instance with `x` replaced). Doc: `Annotations.md` § Immutability friend.
9. **`@Cleanup("method"="close")` — ~0.5 session.** try/finally synthesis around the annotated local. Doc: `Annotations.md` § Resource cleanup.

### Priority 4 — debug-mode features (CompilerModes.md phasing order)

All `[debug]`. Land on top of P1.1 (mode infrastructure).

1. **Source-tagged drop-chain entries — ~1 session.** Extend `cajeta_drop_entry` with alloc/drop source positions in debug mode; debug variants of `__cajeta_drop_push` / `__cajeta_drop_pop_run`; codegen passes source positions from each `emitDropEntry*` site. Foundation for all source-tagged diagnostics. Doc: `cajeta-docs/CompilerModes.md` § Source-tagged drop-chain entries.
2. **SIGABRT handler with chain walk — ~0.5 session.** Install in debug-mode runtime init; on fire, walk the per-thread drop chain and print the head entry's source tags. Catches every glibc heap-corruption abort with a useful diagnostic. Doc: `CompilerModes.md` § Phasing #2.
3. **`--live-set=strict` — ~0.5 session.** Unbounded growth + rehash; assert on duplicate-add (catches compiler-codegen bugs in the live-set hook path). Doc: `CompilerModes.md` § `--live-set`.
4. **`--poison-free=on` — ~0.5 session.** memset freed body with sentinel pattern before glibc free. Doc: `CompilerModes.md` § `--poison-free`.
5. **`--drop-chain-validate=on` — ~0.5 session.** Per-push/pop linked-list integrity checks; assert + diagnostic on corruption. Doc: `CompilerModes.md` § `--drop-chain-validate`.
6. **`--diag-hints=on` (compile-time) — ~1 session.** "Did you mean..." for typo'd identifiers; recommend `#`-transfer when a borrow violates lifetime; suggest `@SuppressLint(...)` for noisy lints. Doc: `CompilerModes.md` § `--diag-hints`.
7. **Stack-trace capture on throw — ~1 session.** `backtrace(3)` + DWARF + source-map symbolization in the exception payload. Doc: `CompilerModes.md` § `--stack-trace-capture`.
8. **`--use-after-move-rt=on` — ~0.5 session.** Sentinel in moved slot header; trap on read. Backs up the static use-after-move tracker. Doc: `CompilerModes.md` § `--use-after-move-rt`.
9. **`--ub-traps=on` — ~0.5 session.** Trap instructions for signed overflow, divide-by-zero, oversized shift, unaligned atomic. Catches "compiler made my code do something weird" early. Doc: `CompilerModes.md` § `--ub-traps`.

### Priority 5 — release-mode features

All `[release]`. Land on top of P1.1 (mode infrastructure).

1. **`--bounds=trap` codegen — ~0.5 session.** Skip the exception throw; emit `@llvm.trap` for the fastest bail. Doc: `CompilerModes.md` § `--bounds`.
2. **`--overflow-checks=wrapping`/`off` codegen — ~1 session.** Today integer arithmetic is implicit wrapping; make the choice explicit via the flag and wire `--overflow-checks=off` so the compiler can assume no overflow and optimize accordingly. Doc: `CompilerModes.md` § `--overflow-checks`.
3. **`--null-checks=on/off/trap` codegen — ~0.5 session.** Today null-check generation is implicit; expose the flag so users can opt out at high `--release` confidence. `@NonNull` (P3.6) integrates here. Doc: `CompilerModes.md` § `--null-checks`.
4. **`--profile-counters=on` — ~1 session.** Per-method invocation counter + wall-time tally for PGO collection. Default on under `--debug-release`. Doc: `CompilerModes.md` § `--profile-counters`.

### Priority 6 — completeness / cleanup

All `[mode-agnostic]`.

1. **P6.6 chained-form completion — ~1 session.** `xs.stream().count()` direct chain. Setting `resolvedType` on the inner stream MCE in generateCode breaks ~100 unrelated tests; cleaner path is to thread the user module into `TemplateInstantiator`'s structures map or override `resolveTypes` to do the lookup without instantiation. **Also covers `xs.stream().forEach(lambda)`** — surfaced during the static-field landing; the chained call currently swallows the second method (forEach never runs).

2. **Non-literal static field initializers — ~1 session.** Today only integer / float literals (with optional `-` prefix) constant-fold into the global's initializer. Method calls, references to other statics, computed expressions, and string literals fall back to zero. Implementation path: emit a per-module `<clinit>`-style init function registered via `llvm.global_ctors` that runs the user expression and stores into the global at module load.

3. **P3c switch/loops/try-catch DA merging — 0.5 sessions each.** Implementation pattern is clear from P3a/P3b; deferred until consumed.

4. **Phase 7 cleanup — 0.5 sessions, not urgent.** Strip the 15 dead `dynamic_pointer_cast<CajetaStruct>` expressions and delete CajetaStruct.h. Cosmetic.

5. **Restore lost test coverage from Phase 7 — incremental.** The 9 deleted struct test files contained ~105 tests. Many exercised happy-path behavior still valid under the unified model.

### Memory-model carry-overs (lower priority, design-doc only)

- ✅ **Automatic field drops.** Landed via FieldOwnership.md § Solution B: the per-thread live-allocation set lets every heap allocation track its liveness; auto-drop calls the type-specific helper (`__cajeta_class_virtual_drop` / `__cajeta_free_array`) which atomically claims out of the set, so aliased fields no-op safely. Spec at `MemoryModel.md:138` updated; doctrine at `cajeta-docs/FieldOwnership.md`; tests at `test/parser/AutoFieldDropTests.cpp` and `test/parser/FieldOwnershipAliasingTests.cpp`. Trade-off: use-after-free of an aliased field whose source has dropped is now the programmer's responsibility (Phase 6+ lifetime tracker can re-tighten).
- **No `super.~Class()` chaining.** Now reachable via virtual dispatch (Gap 1) — needs design pass for whether the base destructor should chain implicitly. Doc: `MemoryModel.md:139`.
- **Multi-parameter borrow-return needs lifetime annotations.** Multi-input free functions can't return a borrow. Rust-style explicit lifetimes would lift it. Doc: `MemoryModel.md:307`.

---

## Done

(See "Current state" above for the running list; older entries below.)
