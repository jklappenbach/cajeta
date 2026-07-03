# Cajeta features — implementation status

One row per user-visible stdlib / language feature. IDs are stable;
do not renumber. Each row links to the design doc for the feature
(`docs/...`) and, where applicable, the pinning test file(s).

Status legend:
- **shipped** — implemented, tested, in `main`
- **partial** — surface partially implemented; sub-items tracked
- **designed** — surface specified in docs; no implementation yet

## Language features

| ID | Name | Description | Status | Doc / tests |
|----|------|-------------|--------|-------------|
| L-01 | Class declarations (`class T`) | Single-inheritance, fields + methods + ctors | shipped | `docs/specification/lang/UnifiedClasses.md`; `test/parser/UnifiedClassSyntaxTests.cpp` |
| L-02 | Templates (`<T>`, `<T, R>`) | C++-style monomorphization; not generics | shipped | `docs/specification/lang/UnifiedClasses.md` § Templates; `test/parser/TemplateBasicTests.cpp` |
| L-03 | Multiple inheritance | Class extends multiple classes/interfaces; required for "container IS a stream" pattern | partial | grammar/vtable/dispatch/super/Gap 8 layout/poly-MI dispatch shipped: per-parent sub-object layout, this-offset adjustment at parent ctor/method call sites, upcast pointer adjustment at LocalVariableDeclaration sites, secondary vtables for non-first-parent sub-objects with synthesized offset thunks for cross-class overrides. Design for collision handling + parent-view selection (P-1..P-6 / R-1..R-4) captured in `docs/specification/lang/MultiClassing.md`; 3-phase implementation outline tracked under Priority 1 in ToDo.md. **MultiClassing Phase 1 (P-1) shipped 2026-05-17** — sibling-collision rejection in `CajetaClass::buildVirtualTable` (methods + return types; covariant overrides preserved) and `DotExpression::generateCode` (fields at access site); pinned by `test/parser/MultiClassingPhase1Tests.cpp` (7 enabled). **MultiClassing Phase 2 (P-2) shipped 2026-05-17** — `super[Base].method()` (direct-call to selected ancestor, bypasses vtable) and `this[Base].field` (read/write the selected ancestor's slot); grammar `primary` extended, visitor wires chosen-ancestor name onto `Super`/`ThisExpression`, `resolveTypes` validates with `CAJETA_ERROR_NOT_AN_ANCESTOR`, `generateCode` reuses `CajetaClass::adjustForUpcast`; pinned by `test/parser/MultiClassingPhase2Tests.cpp` (7 tests). **MultiClassing Phase 3 v1 (P-4 diamond canonicalization) shipped 2026-05-17** — `subObjectSlotMap` now records the first-encountered offset for each ancestor, so `adjustForUpcast(this, D, A)` returns a canonical position; `this.x` and `this[A].x` in D's own code reach the same shared storage. **MultiClassing Phase 3 v2 (cross-path bracketed access) shipped 2026-05-17** — `DotExpression::generateCode` detects diamond access at the use site (anchor = receiver's static type; offset mismatch = diamond signature), shifts `base` to the canonical position, and redirects the GEP through declaringClass's standalone type. `this[NonFirstParent].sharedAncestorField` now reads and writes the canonical shared storage. **MultiClassing Phase 3 v3 (inherited-method re-adjustment) shipped 2026-05-17** — `MethodCallExpression`'s super-dispatch path re-adjusts `thisValue` from the bracketed position to the resolved method's declaring-class canonical position when a diamond is detected; `super[C].setX(...)` (where `setX` is inherited from a shared ancestor) now reaches the canonical storage. Pinned by `test/parser/MultiClassingPhase3Tests.cpp` (10 enabled tests). **MultiClassing P-5 dispatch fix shipped 2026-05-17** — abstract-declared canonicals now alias to the concrete sibling impl in the vtable; `c.step()` dispatches correctly when A declares `abstract step()` and B (sibling parent) provides the concrete body. **`adjustForUpcast` threading shipped 2026-05-17** — the upcast pointer shift now fires at all four sites where a descendant value flows into an ancestor-typed slot: LocalVariableDeclaration (Phase 1), assignment expressions (BinaryOpExpression `=`), return statements (typed-return path), and parameter passing (CajetaClass::invokeMethod's arg coercion). Pinned by `test/parser/UpcastThreadingTests.cpp` (5 tests). **R-3 `@Override(from=Ancestor)` verification shipped 2026-05-17** — `buildVirtualTable` verifies the named ancestor is in the receiver's parent chain AND declares a same-suffix method; raises `CAJETA_ERROR_OVERRIDE_FROM_MISMATCH` on miss. Accepts both `from=B` and `from=B.class`. Pinned by `test/parser/OverrideFromTests.cpp` (6 tests). **R-2 narrow implicit-ctor-skip warning shipped 2026-05-17** — `Method::generateCode` emits `warning: [implicit-ctor-skip]` when ctor body has explicit `super(args)` AND a sibling parent has both no-arg + args ctors. Pinned by `test/parser/ImplicitCtorSkipWarningTests.cpp` (4 tests). **MultiClassing track CLOSED.** Tree at zero disabled tests. Remaining gap: Phase 3 v4 — non-first parent's OWN methods (declared on C, not inherited) touching shared ancestors via internal `this.x` — exotic pattern, needs vbase ABI or per-descendant recompilation, no test pinned. Other upcast sites (assignment expression, return statement, parameter passing) still need `CajetaClass::adjustForUpcast` threaded through. Pinned by `test/parser/MultipleInheritanceGapTests.cpp` and `test/parser/DynamicDispatchTests.cpp`. `docs/specification/lang/MultiClassing.md`, `docs/specification/lang/stream/Streams.md`, Collections.md |
| L-04 | Interfaces (`interface T`) | Fat-pointer dispatch, multiple implementation | shipped | `docs/specification/lang/UnifiedClasses.md` § Interfaces; `test/parser/InterfaceTests.cpp` |
| L-05 | Lambdas + function types | `(T) -> R` types, closure capture, indirect call | shipped | `docs/specification/lang/Lambdas.md`; `test/parser/LambdaL{1,15,2,3,4}Tests.cpp` |
| L-06 | Function-typed field invocation | `this.fn(args)` where `fn` is a `(T) -> R` field | shipped | `src/cajeta/asn/expression/MethodCallExpression.cpp`; covered by `test/parser/StreamIntermediateTests.cpp` (filter/map/peek tests would fail without it) |
| L-07 | `heap` / `stack` allocation prefix | Unified-class storage selection | shipped | `docs/specification/lang/UnifiedClasses.md`; `test/parser/UnifiedClassSyntaxTests.cpp` |
| L-08 | Memory model (`#` transfer, drop chain) | Single-owner heap; static + runtime checks | shipped | `docs/specification/lang/MemoryModel.md`, `docs/specification/lang/FieldOwnership.md`; many parser tests |
| L-09 | Wire-format views (`struct`) | Zero-copy typed overlays; `@BigEndian`/`@Align` | shipped | `docs/specification/lang/Views.md`; `test/parser/StructView*.cpp` |
| L-10 | Switch expressions | Pattern-matching syntax | shipped | `test/expression/SwitchExpressionTests.cpp` |
| L-11 | Static class fields | `static T field` lowered to LLVM global | shipped | `test/parser/StaticFieldTests.cpp` (9), `test/parser/LambdaStaticCaptureTests.cpp` (2) |
| L-12 | `@AutoHash` synthesis | Structural `hash()` walk over fields | shipped | `docs/specification/hash/Hashing.md`; `test/parser/AutoHashTests.cpp` |
| L-13 | Annotation argument capture | `Annotatable.getString/Int/Bool/ClassRef/StringList/IntList` | shipped | `docs/specification/reflect/Annotations.md`; tests across `ComponentRegistrationTests`, `NamedInjectTests`, `OrderChainingTests`, `AnnotationParsingTests` |
| L-14 | Compiler modes (`--debug`/`--release`/etc.) | `CompilerMode` enum + 13 toggle flags | shipped | `docs/CompilerModes.md` |
| L-15 | Source-tagged drop entries | 40-byte debug drop record with file:line tag | shipped | `test/parser/SourceTaggedDropTests.cpp` |
| L-16 | SIGABRT handler with chain walk | Runtime ctor installs handler; dumps per-thread drop chain | shipped | runtime smoke-tested via existing crash paths |
| L-17 | Template-instantiation field codegen | Class-ref fields lay out as `ptr` (not inline body) | shipped | recent commits `ef521b9`, `78981f6`, `4cbd88c`, `8fcfaf5` |
| L-18 | Subtype-aware ctor/method lookup | BFS over arg's superClasses for parameter binding | shipped | `test/parser/SubtypeArgLookupTests.cpp` |
| L-19 | `super.method()` calls | Direct upcall to parent's implementation | shipped | `test/parser/MultipleInheritanceGapTests.cpp` (superMethodCallReachesParent + explicitSuperCtorWithArgs). `docs/specification/lang/UnifiedClasses.md` § super |
| L-20 | Lambda capture by reference | `let captures = ...` semantics | shipped (partial) | `docs/specification/lang/Lambdas.md`; full closure semantics in Lambda tests |
| L-21 | Chained call form (`a.b().c()`) | `xs.stream().filter(p).map(f).count()` | partial | works for simple chains; P6.6 covers the remaining cases |
| L-22 | Method-level type parameters (`final <T> ...`) | Per-method `<T>` declaration | shipped | `docs/specification/lang/MethodLevelTemplate.md`; static + instance method templates, inference + explicit-type-arg call syntax (`xs.map<int64>(...)` — Form C, type args after the identifier) all working |
| L-23 | Templated-static-factory call syntax | `Optional.Some<int32>(42)` | shipped (Form C) | `Optional.Some<int32>(42)` and `xs.method<R>(args)` parse and dispatch via L-22's explicit-type-arg path. `Optional<int32>.Some(42)` (type-name-with-args as receiver — Form B) intentionally not pursued: Form C subsumes its use cases |
| L-24 | `@Encoding(EncoderClass)` for views | Encoder interface for wire-format serialization | shipped | `docs/specification/reflect/Annotations.md` § `@Encoding`; `test/parser/EncodingPhase{A,B}Tests.cpp` (10). Synthesizes `T(byte[])` ctor delegating to `EncoderClass.decode(bytes)` (memcpy + shell-free) and `#byte[] toBytes()` delegating to `EncoderClass.encode(this)`. Mutual exclusion with `@BigEndian`/`@LittleEndian`/`@HostEndian`/`@Align` enforced. Encoder dispatch is duck-typed (static `encode`/`decode` by name on the encoder class) — `cajeta.wire.Encoder<T>` interface shipped alongside for documentation; impl-side verification of `Encoder<T>` conformance waits on templated-interface vtable instantiation. |
| L-25 | Static nested classes | `public class Outer { public static class Inner { ... } }` accessed as `Outer.Inner` | shipped | `test/parser/NestedClassTests.cpp` (5). Wired via `NestedClassDeclaration` wrapper + `Method::create` switched from `structureStack.front()` to `.back()` for the enclosing parent. v1 supports static-nested only (no implicit outer-this). |
| L-26 | Templated interface declarations | `interface Encoder<T>` parses + lives in stdlib | shipped (parse) | `test/parser/TemplatedInterfaceTests.cpp` (2). `visitInterfaceDeclaration` now processes typeParameters + skips body-walk for templates (mirrors class-template path). Implementing-class instantiation of templated-interface vtables remains future work. |
| L-27 | Return-ownership enforcement | `return heap/new/stack X` requires `#T` return type | shipped | `CAJETA_ERROR_FRESH_RETURN_NEEDS_TRANSFER` in `ReturnStatement::generateCode`. Lambda returns are exempted (no `#R` syntax in `(T) -> R`). Stdlib swept (14 sites updated: `ArrayList.stream`, `HashMap.{keys,values,entries}`, every Stream subclass's `next()`, etc.); test files updated (7). |
| L-28 | `this`-passing convention | `loadIfLValue`'s class-ref catch-all loads as `ptr` not inline struct | shipped | `test/parser/ThisAsArgTests.cpp` (3). Fixes a latent class of `return heap Wrapper(this)` patterns that previously SIGSEGV'd at JIT verify. |
| L-29 | Encoding-prefixed byte-array literals | `utf8 "..."` / `utf16le "..."` / `utf16be "..."` / `utf32 "..."` / `ascii "..."` / `latin1 "..."` materialize a static byte-array (or codepoint-array) constant in the named encoding. Compile-time encode, zero allocation, no `String` round trip — eases interop with file I/O, sockets, hashing, wire protocols. `ascii` / `latin1` raise a compile error on out-of-range codepoints. Distinct from S-105 (runtime decode of bytes to String). | designed | `docs/specification/lang/EncodingPrefixedLiterals.md`; task #164 |
| L-30 | Class-String literal codegen flip (Phase 2b-β) | String literals materialize as static `cajeta.lang.String` view-mode instances (vtable + bytes pointer + byteLength + mode=1 + cachedCpLength=-1) rather than bare `i8*` C-strings. Class methods (`size`, `count`, `equals`, `isEmpty`, `charAt`, `indexOf`, `startsWith`, `endsWith`, `contains`, `substring`, `toLowerCase`, `toUpperCase`, `trim`, `replace`) now dispatch via the class instead of the legacy `__cajeta_str_*` intrinsic table. Strings remain process-lifetime (never dropped). | shipped | `test/expression/StringMethodsTests.cpp` (29). LocalVariableDeclaration carves out `cajeta.lang.String` from drop-entry registration to honor the never-drop rule. |
| L-31 | Implicit destructor chaining (C++ semantics) | `~Derived()` body runs first, then `~Base()` (and each ancestor in turn) runs automatically — no user-written `super.drop()` call needed. The synthesized drop wrapper calls each first-parent ancestor's user-declared `drop()` body directly (not the parent's full wrapper, which would double-walk fields and double-free). Multi-level chains work; missing levels (no `drop()` declared) are skipped. Applies to both heap and stack allocations. Virtual dispatch still picks the dynamic type's drop, then the chain runs from there. | shipped | `test/parser/DestructorChainTests.cpp` (6); `VirtualDropDispatchTests.derivedDestructorWinsOverBase` updated (now expects chain, count = 4). |
| L-32 | JSON Tier-1 array fields | `Json.parse<T>` / `Json.toBytes<T>` synthesizer handles `int32[]`, `int64[]`, `boolean[]`, `float64[]`, `cajeta.lang.String[]` fields. Read accumulates into a temp `ArrayList<E>`, then assigns the sized `E[]` directly to `out.<field>` (no intermediate local — the intermediate's drop would free the buffer before the caller could read it). Write iterates with a typed temp-local per element so the JIT verifier sees a real i64 / etc. instead of a slot pointer. Nested-class arrays (`T[] items` where T is a class) are a documented v1 limitation pending a `JsonReader.peek()` surface. | shipped | `test/parser/JsonSynthesizerTests.cpp` (7 array-specific tests + 1 String-overload). Also: `Json.parse<T>(String)` overload + `Json.parse(String)` Tier-3 sibling. Companion fix: `MethodCallExpression`'s receiver load-through now applies to array fields (`b.ids.count()` now reads the count word, not the slot's first 8 bytes). |
| L-33 | JSON `@JsonProperty(name)` + `@JsonIgnore` | Per-field annotations driving the synthesized parse / toBytes bodies. `@JsonProperty("user_id")` renames the wire key while preserving the Cajeta-level declared name; `@JsonIgnore` skips the field entirely on both sides (default value on read, omitted on write). Mixed annotated + un-annotated fields work in the same class. Class-level naming strategy (`@JsonNamingStrategy(SNAKE_CASE \| KEBAB)`) and other field-level annotations (`@JsonRequired`, `@JsonInclude(...)`, `@JsonAlias`, `@JsonRaw`) are the next planned commits. | shipped | `test/parser/JsonSynthesizerTests.cpp` (6 annotation tests: rename-on-write, rename-on-read, rename-round-trip, ignore-not-written, ignore-not-read, mixed). |

## Stdlib — `cajeta.lang`

| ID | Name | Description | Status | Doc / tests |
|----|------|-------------|--------|-------------|
| S-101 | `Object` base | Universal root; identity defaults | shipped | `docs/specification/lang/Lang.md` |
| S-102 | `String` (intrinsic ops) | `+`, contains, indexOf, substring, upper/lower, trim, replace, equals, size, split | shipped | `test/expression/StringMethodsTests.cpp` |
| S-103 | `String` owned vs view modes | `viewOf`, `toOwned`, lifetime tying | designed | `docs/specification/lang/Lang.md` |
| S-104 | `String.fromCodePoints` / `.repeat` / `.lines` / `getBytes` / `codePointAt` / `compare` / `lastIndexOf` | Less common String surface | designed | `docs/specification/lang/Lang.md` |
| S-105 | `Encoding` enum | Explicit encoding type for String construction | designed | UTF-8 currently hardcoded |
| S-106 | `Optional<T>` construction + extraction | `isPresent`/`isEmpty`/`get`/`orElse` | shipped | `test/parser/OptionalTests.cpp` (6), `test/parser/OptionalAndAllocateTests.cpp` (6) |
| S-107 | `Optional<T>` multiple-inherits `Stream<T>` | One-element-or-empty stream | designed | Needs L-03 |
| S-108 | `Pair<K, V>` | Two-field value type with getters | shipped | `test/parser/PairTests.cpp` (3) |

## Stdlib — `cajeta.lang.stream`

| ID | Name | Description | Status | Doc / tests |
|----|------|-------------|--------|-------------|
| S-201 | `Stream<T>` base + `next()` protocol | Pull-protocol root + default empty | shipped | `docs/specification/lang/stream/Streams.md` |
| S-202 | Stream terminals: `count` / `forEach` | No-lambda + first lambda terminal | shipped | `test/parser/StreamTests.cpp` (9) |
| S-203 | Stream terminals: `anyMatch` / `allMatch` / `noneMatch` / `findFirst` / `reduce` | Lambda-taking terminals | shipped | `test/parser/StreamTerminalTests.cpp` (13) |
| S-204 | Stream terminal: `collect(Collector<T, R>)` | Needs S-501 Collector | designed | `docs/specification/lang/stream/Streams.md` |
| S-205 | Stream terminal: `fold<R>(R seed, (R, T) -> R fn)` | Cross-type fold | shipped | Wired in `Stream<T>.fold<R>`; `reduce` rewritten as one-liner over `fold<T>`. Pinned by `test/parser/StreamFoldTests.cpp` |
| S-206 | Stream terminal: `collect<R>(Collector<T, R>)` | Reduce via packaged collector | shipped (hand-rolled) | `Collector<T, R>` data class + `Stream<T>.collect<R>` shipped. `Collectors.toList<T>()` parses but hits the zero-value-param T-canonical collision (same as L-22's known limitation). Pinned by `test/parser/CollectorTests.cpp` |
| S-206 | `ArrayStream<T>` + `T[].stream()` intrinsic | Array-backed stream | shipped | `test/parser/StreamTests.cpp` (9) |
| S-207 | `TakeStream<T>` | `take(n)` wrapper | shipped | `test/parser/StreamIntermediateTests.cpp` (5 of 19) |
| S-208 | `SkipStream<T>` | `skip(n)` wrapper | shipped | `test/parser/StreamIntermediateTests.cpp` (4 of 19) |
| S-209 | `FilterStream<T>` | `filter(pred)` wrapper | shipped | `test/parser/StreamIntermediateTests.cpp` (3 of 19) |
| S-210 | `MapStream<T, R>` | `map(fn)` wrapper — element-type-changing | shipped | `test/parser/StreamIntermediateTests.cpp` (3 of 19) |
| S-211 | `PeekStream<T>` | `peek(fn)` side-effecting passthrough | shipped | `test/parser/StreamIntermediateTests.cpp` (2 of 19) |
| S-212 | `FlatMapStream<T, R>` | `flatMap(fn)` flattening wrapper | shipped | `test/parser/StreamIntermediateTests.cpp` (2 of 19) |
| S-213 | `TakeWhileStream` / `DropWhileStream` | Predicate-based take/skip | designed | `docs/specification/lang/stream/Streams.md` |
| S-214 | `DistinctStream` / `EnumerateStream` / `ZipStream` / `ChainStream` / `SortedStream` / `WindowedStream` | Remaining intermediate combinators | designed | `docs/specification/lang/stream/Streams.md` |

## Stdlib — `cajeta.collection`

| ID | Name | Description | Status | Doc / tests |
|----|------|-------------|--------|-------------|
| S-301 | `ArrayList<T>` | Dynamic array w/ `add`/`get`/`set`/`size`/`stream` | shipped | `test/parser/ArrayListTests.cpp` (7) |
| S-302 | `ArrayList<T>` multiple-inherits `Stream<T>` | `for (v : list)` and `list.count()` without `.stream()` | designed | Needs L-03 |
| S-303 | `ArrayList<T>` remove/clear/contains/indexOf | Additional API surface | designed | |
| S-304 | `HashMap<K, V>` | Open-addressing hash table with put/get/remove/containsKey | shipped | `test/collections/HashMapTests.cpp`, `test/collections/PrimitiveHashMapTests.cpp` |
| S-305 | `HashMap<K, V>` multiple-inherits `Stream<Pair<K, V>>` | Map IS its entry-stream | designed | Needs L-03 |
| S-306 | `HashSet<T>` | Thin wrapper over HashMap | designed | |
| S-307 | `LinkedList<T>` | Doubly-linked list | designed | |
| S-308 | `Deque<T>` / `Stack<T>` | Double-ended queue / LIFO | designed | |
| S-309 | `Heap<T>` (priority queue) | Binary min/max heap with comparator | designed | |
| S-310 | `TreeMap<K, V>` / `TreeSet<T>` / `BTreeMap` / `BTreeSet` | Ordered map / set | designed | |
| S-311 | `Collector<T, R>` interface | Supplier / accumulator / finisher | designed | Blocks S-204 |
| S-312 | `Collectors` factories | `toList`, `toSet`, `toMap`, `counting`, `joining`, etc. | designed | Needs S-311 |
| S-313 | For-loop desugaring through Stream | `for (v : iterable)` when iterable IS-A Stream | designed | Needs L-03 |

## Stdlib — `cajeta.hash`

| ID | Name | Description | Status | Doc / tests |
|----|------|-------------|--------|-------------|
| S-401 | Per-primitive `hash()` | int/uint/float/bool/pointer/String/byte[] intrinsics | shipped | `test/expression/HashTests.cpp` |
| S-402 | `Hash.identity` / `Hash.combine` / `Hash.processSeed` | Utility namespace | shipped | `test/expression/HashTests.cpp` |
| S-403 | `Hasher` interface | Streaming `writeT(...)` + `finish()` contract | designed | `docs/specification/hash/Hashing.md` |
| S-404 | `XXHash3` algorithm class | Fast general-purpose | designed | |
| S-405 | `RapidHash` algorithm class | Max throughput | designed | |
| S-406 | `SipHash` algorithm class | DoS-resistant | designed | |
| S-407 | `MD5` algorithm class | Checksum/identifier (not security) | designed | |
| S-408 | `DefaultHasher` algorithm class | What `Object.hash()` uses internally | designed | |
| S-409 | `Hash.identity` parameter widening to `Object` | Universal-Object accepts any class | designed | Needs the universal-Object class hierarchy |

## Stdlib — `cajeta.time`

Core `cajeta.time` is **implemented** (v1, `feature/time`); tz database + pattern
formatter deferred. Plan: `plans/time/cajeta-time-plan.md`. Spec:
`docs/specification/time/Time.md`. Tests: `test/time/` (JIT). All value types are
pure stack types (single canonical field, structural `operator==`, `int32
compareTo` via `cajeta.lang.Comparable<T>`); ISO-8601 text via per-type `iso()`
returning an owned `#String` (no `toString` override until the String surface
stabilizes).

| ID | Name | Description | Status |
|----|------|-------------|--------|
| S-501 | `Clock` static surface | nanoTime / millisTime / now | shipped — `@Native` CLOCK_MONOTONIC/REALTIME binding |
| S-502 | `Instant` value type | UTC moment, ns precision | shipped — epoch round-trips, plus/minus, between |
| S-503 | `Duration` value type | Time-based amount | shipped — full arithmetic, compareTo, iso |
| S-504 | `Period` value type | Calendar-based amount | shipped — Y/M/D, normalized, LocalDate.plus(Period) |
| S-505 | `LocalDate` / `LocalTime` / `LocalDateTime` | Zone-naive date/time | shipped — Hinnant civil↔epoch-day, validation, carry arithmetic |
| S-506 | `ZoneId` / `ZoneOffset` / `ZonedDateTime` | Time-zone resolution | shipped — `ZoneOffset` + offset-based `ZonedDateTime`; region `ZoneId` resolves DST-aware offsets via native TZif parse of `/usr/share/zoneinfo` (`__cajeta_tz_offset`), UTC fast-path for static builds |
| S-507 | `DateTimeFormatter` | Pattern-based formatting + parsing | partial — strftime `ofPattern` (`%Y %m %d %H %I %M %S %p %j %a %A %b %B %z %Z %f %L %%`…) + `FormatStyle` standards (ISO/BASIC/RFC_1123/US/EURO/SQL) shipped over a `DateTimeFields` engine; fluent step-builder deferred (cajeta codegen for self-returning fluent methods unsound); parsing (text→temporal) deferred |

## Stdlib — `cajeta.io` / `.file` / `.net`

All `cajeta.io.*` is **designed, unimplemented**. See `docs/specification/io/Io.md`,
`IoFile.md`, `IoNet.md`.

| ID | Name | Description | Status |
|----|------|-------------|--------|
| S-601 | `Buffer` + `BufferChain` | Byte substrate | designed |
| S-602 | `InputStream` / `OutputStream` / `Reader` / `Writer` | Stream abstractions | designed |
| S-603 | `Path` value type | Immutable path manipulation | designed |
| S-604 | `FileInfo` value type | Batched stat result | designed |
| S-605 | `Path` read/write one-liners | `readText`/`writeText`/`readBytes`/`writeBytes` | designed |
| S-606 | `File` handle | Random access, seek, truncate, lock | designed |
| S-607 | Directory operations | `children`/`walk`/`glob`/`mkdirs` | designed |
| S-608 | Atomic writes by default | Write to .tmp, fsync, rename | designed |
| S-609 | `Watcher` | inotify/FSEvents/ReadDirectoryChangesW | designed |
| S-610 | Async I/O forms (`*Async`) | Return `Task<T>` | designed |
| S-611 | `Socket` / `ServerSocket` interfaces | Net abstractions | designed |
| S-612 | TCP / UDP / TLS / HTTP / WebSocket | Concrete protocols | designed |

## Stdlib — `cajeta.process`

| ID | Name | Description | Status |
|----|------|-------------|--------|
| S-701 | `ProcessBuilder` / `Process` / `ExitStatus` | Subprocess management | designed |

## Stdlib — `cajeta.thread`

| ID | Name | Description | Status | Doc / tests |
|----|------|-------------|--------|-------------|
| S-801 | Fiber runtime (R1-R5-A') | Stackful fibers + single-carrier cooperative scheduler. NOT YET: work-stealing pool, timer wheel, I/O reactor | shipped | `docs/specification/concurrent/AsyncStatus.md` |
| S-802 | `Task<T>` + `spawn` syntax | Lightweight async tasks | shipped | `test/parser/TaskTypingTests.cpp`, `SpawnDropTests.cpp`, `AsyncSyntaxTests.cpp`, `DetachTests.cpp`, `PerFiberDropChainTests.cpp` |
| S-803 | `Lock` class + intrinsics | Intrinsics (`Cajeta.lockNew()` etc.) shipped; the cajeta-source `Lock`/`LockGuard` classes exist only as inline test source, not yet as a stdlib `cajeta.concurrent` package (R7-A promotes them) | partial | `test/parser/LockIntrinsicTests.cpp`, `LockClassTests.cpp` |
| S-804 | `Fiber` / `Thread` cajeta-source classes | Surface wrappers around runtime | designed | Runtime exists; surface classes haven't been declared |
| S-805 | R5-C / R5-D runtime items | R5-C cooperative cancellation + R5-D scope exception-escalation, in the runtime (`__cajeta_fiber_cancel`, cancel re-raise in `__cajeta_task_wait`) | shipped | `docs/specification/concurrent/AsyncStatus.md` |

## Stdlib — `cajeta.error`

| ID | Name | Description | Status | Doc / tests |
|----|------|-------------|--------|-------------|
| S-901 | Exception hierarchy (Throwable / Exception / RecoverableException / UnrecoverableException) | Root types | shipped | `test/parser/ErrorModelTests.cpp` |
| S-902 | `try` / `catch` / `throw` syntax | Standard control flow | shipped | `test/expression/SwitchAndExceptionTests.cpp` |
| S-903 | `try-with-resources` | Auto-close on scope exit | partial | `test/parser/TryWithResourcesTests.cpp` |
| S-904 | Stack-trace capture on throw | DWARF-symbolicated trace in exception payload | designed | P4.5 debug-mode feature; `docs/CompilerModes.md` |

## Stdlib — `cajeta.codec`

| ID | Name | Description | Status | Doc / tests |
|----|------|-------------|--------|-------------|
| S-1101 | `cajeta.codec.json` spec | JSON value model, three-tier API (`Json.parse<T>` / `Json.toBytes` codegen / pull tokenizer / value tree), per-field annotation surface (`@JsonProperty`, `@JsonIgnore`, `@JsonRequired`, `@JsonAlias`, `@JsonInclude`, class-level `@JsonNamingStrategy` / `@JsonStrict`), lazy number parse, zero-copy string slices, RFC 8259 conformance, error model, performance targets (≥ 500 MB/s scalar tokenizer; v2 SIMD direction noted). Single source of truth for what "JSON support" means in Cajeta — `@ToString(format=TO_STRING_JSON)` and ad-hoc serialization both consume it. JSON does NOT use `@Encoding` (that stays for binary formats only — MessagePack, Protobuf, Avro). | shipped 2026-05-18 | `docs/specification/codec/json/Json.md` |
| S-1102 | `cajeta.codec.json` implementation | Three-tier impl per S-1101. **Phase 1+2+3+4a shipped 2026-05-18**: `JsonReader` pull tokenizer with lazy byte-span recording; reader number/bytes materialization + fluent `JsonWriter` (string-escape encode, integer formatting, geometric-growth buffer); `JsonValue` / `JsonArray` / `JsonObject` tree + recursive descent on both sides; `Json.parse` / `Json.toBytes` factory entry points wrapping the tree path. Phase 4b (Tier-1 codegen synthesizer for `Json.parse<T>` / `Json.toBytes(value:T)` per-class direct binding + per-field annotation surface) deferred to a dedicated session. Tests: `JsonReaderTests` (10), `JsonWriterTests` (7), `JsonReaderNumberTests` (1), `JsonReaderBytesTests` (1), `JsonValueTests` (13), `JsonFactoryTests` (4). | shipped 2026-05-18 (Phases 1–4a) | `runtime/src/cajeta/codec/json/`, 6 test files |

## Aspect model — `cajeta.aspect`

| ID | Name | Description | Status | Doc / tests |
|----|------|-------------|--------|-------------|
| S-1001 | `@Component` / `@Inject` DI | Dependency injection annotations | shipped | `docs/specification/lang/AspectModel.md`; `test/parser/ComponentRegistrationTests.cpp`, `InjectCodegenTests.cpp`, `NamedInjectTests.cpp` |
| S-1002 | `@Before` / `@After` / `@Around` advice | Aspect-oriented method wrapping | shipped | `docs/specification/lang/AspectModel.md`; `test/parser/AdviceCodegenTests.cpp`, `AroundAdviceTests.cpp`, `AfterReturningThrowingTests.cpp` |
| S-1003 | Pointcut matching | Method-name + annotation matchers | shipped | `test/parser/PointcutMatchingTests.cpp` |
| S-1004 | `@Inject` vs implicit injection | Decision: keep `@Inject` or drop in favor of auto | pending | `docs/specification/lang/AspectModel.md` |

## Annotations

See `docs/specification/reflect/Annotations.md` for the full catalog. The annotation-
arg machinery is shipped (L-13); individual handlers are tracked
under the language and stdlib sections above.

### Lombok-mirror — `cajeta.synth`

All shipped 2026-05-18 as a single track. Synthesizer classes live
under `src/cajeta/method/Synthesized*Method.{h,cpp}`. User-declared
methods of the same name+arity always win — synthesizers skip.

| ID | Annotation | Effect | Status | Tests |
|----|------------|--------|--------|-------|
| A-201 | `@Getter` | Per-field accessor `T name()` (size()-style, not getName) | shipped | `test/parser/GetterSetterTests.cpp` (6 of 10) |
| A-202 | `@Setter` | Per-field mutator `void name(T v)`; skips final fields | shipped | `test/parser/GetterSetterTests.cpp` (4 of 10) |
| A-203 | `@ToString` | `String toString()` returning `Class(f1=v1,f2=v2,...)`; `@Exclude` on fields | shipped (PROPERTIES format) | `test/parser/ToStringTests.cpp` (12). `TO_STRING_JSON` deferred to S-1102 |
| A-204 | `@NoArgsConstructor` | Zero-arg ctor, fields zero-init'd | shipped | `test/parser/ConstructorAnnotationTests.cpp` |
| A-205 | `@AllArgsConstructor` | Ctor takes every non-static field | shipped | `test/parser/ConstructorAnnotationTests.cpp` |
| A-206 | `@RequiredArgsConstructor` | Ctor takes only `final` (and future `@NonNull`) fields | shipped | `test/parser/ConstructorAnnotationTests.cpp` (8 across the three) |
| A-207 | `@Data` | Bundle: @Getter + @Setter + @ToString + @AutoHash + @RequiredArgsConstructor | shipped | `test/parser/DataValueAnnotationTests.cpp` |
| A-208 | `@Value` | Bundle: @Getter + @ToString + @AutoHash + @AllArgsConstructor (immutable; no setters) | shipped | `test/parser/DataValueAnnotationTests.cpp` (6 across both) |
| A-209 | `@NonNull` | Parameter null-check at method entry; `@RequiredArgsConstructor` includes the field | shipped | `test/parser/NonNullTests.cpp` (7). Return-type @NonNull deferred |
| A-210 | `@With` | Per-field copy-with mutator `withX(T v)` — alloc + memcpy + overwrite slot + return | shipped | `test/parser/WithAnnotationTests.cpp` (6) |
| A-211 | `@Builder` | Synthesizes nested `Outer.Builder` class + chained setters + `build()` + static `builder()` on Outer. Implicitly enables @AllArgsConstructor | shipped | `test/parser/BuilderAnnotationTests.cpp` (6). Uses real nested-class infra (L-25). `@Builder.Default` deferred |
| A-212 | `@Encoding(EncoderClass)` | See L-24 | shipped | `test/parser/EncodingPhase{A,B}Tests.cpp` (10) |
| A-XXX | `@EqualsAndHashCode` | n/a | **rejected** | Cajeta has no `equals()` on Object (uses `operator==`); `@AutoHash` covers hashing |
| A-XXX | `@Cleanup` | n/a | **rejected** | Cajeta destructors + auto-field-drop cover the use case |

## Tracking conventions

- Each "shipped" row must link to at least one pinning test file.
- "Partial" rows should describe what's missing in the description.
- "Designed" rows should link to the design doc.
- When promoting a feature from designed→partial→shipped, update
  the status and add the test reference in the same commit as the
  implementation.
