# Cajeta features — implementation status

One row per user-visible stdlib / language feature. IDs are stable;
do not renumber. Each row links to the design doc for the feature
(`cajeta-docs/...`) and, where applicable, the pinning test file(s).

Status legend:
- **shipped** — implemented, tested, in `main`
- **partial** — surface partially implemented; sub-items tracked
- **designed** — surface specified in docs; no implementation yet

## Language features

| ID | Name | Description | Status | Doc / tests |
|----|------|-------------|--------|-------------|
| L-01 | Class declarations (`class T`) | Single-inheritance, fields + methods + ctors | shipped | `cajeta-docs/stdlib/UnifiedClasses.md`; `test/parser/UnifiedClassSyntaxTests.cpp` |
| L-02 | Templates (`<T>`, `<T, R>`) | C++-style monomorphization; not generics | shipped | `cajeta-docs/stdlib/UnifiedClasses.md` § Templates; `test/parser/TemplateBasicTests.cpp` |
| L-03 | Multiple inheritance | Class extends multiple classes/interfaces; required for "container IS a stream" pattern | partial | grammar/vtable/dispatch/super/Gap 8 layout/poly-MI dispatch shipped: per-parent sub-object layout, this-offset adjustment at parent ctor/method call sites, upcast pointer adjustment at LocalVariableDeclaration sites, secondary vtables for non-first-parent sub-objects with synthesized offset thunks for cross-class overrides. Design for collision handling + parent-view selection (P-1..P-6 / R-1..R-4) captured in `cajeta-docs/stdlib/MultiClassing.md`; 3-phase implementation outline tracked under Priority 1 in ToDo.md. Remaining implementation gaps: (1) collision rejection in `buildVirtualTable` / `getFieldLlvmIndex` (MultiClassing Phase 1); (2) `super[Base]` / `this[Base]` grammar + dispatch (Phase 2, supersedes deferred Gap 9 `super[Base].method()`); (3) true-diamond shared-ancestor bookkeeping (Phase 3 — vbase machinery, shared by default per R-4). Other upcast sites (assignment expression, return statement, parameter passing) still need `CajetaClass::adjustForUpcast` threaded through. Pinned by `test/parser/MultipleInheritanceGapTests.cpp` and `test/parser/DynamicDispatchTests.cpp`. `cajeta-docs/stdlib/MultiClassing.md`, `cajeta-docs/stdlib/Streams.md`, Collections.md |
| L-04 | Interfaces (`interface T`) | Fat-pointer dispatch, multiple implementation | shipped | `cajeta-docs/stdlib/UnifiedClasses.md` § Interfaces; `test/parser/InterfaceTests.cpp` |
| L-05 | Lambdas + function types | `(T) -> R` types, closure capture, indirect call | shipped | `cajeta-docs/stdlib/Lambdas.md`; `test/parser/LambdaL{1,15,2,3,4}Tests.cpp` |
| L-06 | Function-typed field invocation | `this.fn(args)` where `fn` is a `(T) -> R` field | shipped | `src/cajeta/asn/expression/MethodCallExpression.cpp`; covered by `test/parser/StreamIntermediateTests.cpp` (filter/map/peek tests would fail without it) |
| L-07 | `heap` / `stack` allocation prefix | Unified-class storage selection | shipped | `cajeta-docs/stdlib/UnifiedClasses.md`; `test/parser/UnifiedClassSyntaxTests.cpp` |
| L-08 | Memory model (`#` transfer, drop chain) | Single-owner heap; static + runtime checks | shipped | `cajeta-docs/stdlib/MemoryModel.md`, `cajeta-docs/stdlib/FieldOwnership.md`; many parser tests |
| L-09 | Wire-format views (`struct`) | Zero-copy typed overlays; `@BigEndian`/`@Align` | shipped | `cajeta-docs/stdlib/Views.md`; `test/parser/StructView*.cpp` |
| L-10 | Switch expressions | Pattern-matching syntax | shipped | `test/expression/SwitchExpressionTests.cpp` |
| L-11 | Static class fields | `static T field` lowered to LLVM global | shipped | `test/parser/StaticFieldTests.cpp` (9), `test/parser/LambdaStaticCaptureTests.cpp` (2) |
| L-12 | `@AutoHash` synthesis | Structural `hash()` walk over fields | shipped | `cajeta-docs/stdlib/Hashing.md`; `test/parser/AutoHashTests.cpp` |
| L-13 | Annotation argument capture | `Annotatable.getString/Int/Bool/ClassRef/StringList/IntList` | shipped | `cajeta-docs/stdlib/Annotations.md`; tests across `ComponentRegistrationTests`, `NamedInjectTests`, `OrderChainingTests`, `AnnotationParsingTests` |
| L-14 | Compiler modes (`--debug`/`--release`/etc.) | `CompilerMode` enum + 13 toggle flags | shipped | `cajeta-docs/CompilerModes.md` |
| L-15 | Source-tagged drop entries | 40-byte debug drop record with file:line tag | shipped | `test/parser/SourceTaggedDropTests.cpp` |
| L-16 | SIGABRT handler with chain walk | Runtime ctor installs handler; dumps per-thread drop chain | shipped | runtime smoke-tested via existing crash paths |
| L-17 | Template-instantiation field codegen | Class-ref fields lay out as `ptr` (not inline body) | shipped | recent commits `ef521b9`, `78981f6`, `4cbd88c`, `8fcfaf5` |
| L-18 | Subtype-aware ctor/method lookup | BFS over arg's superClasses for parameter binding | shipped | `test/parser/SubtypeArgLookupTests.cpp` |
| L-19 | `super.method()` calls | Direct upcall to parent's implementation | shipped | `test/parser/MultipleInheritanceGapTests.cpp` (superMethodCallReachesParent + explicitSuperCtorWithArgs). `cajeta-docs/stdlib/UnifiedClasses.md` § super |
| L-20 | Lambda capture by reference | `let captures = ...` semantics | shipped (partial) | `cajeta-docs/stdlib/Lambdas.md`; full closure semantics in Lambda tests |
| L-21 | Chained call form (`a.b().c()`) | `xs.stream().filter(p).map(f).count()` | partial | works for simple chains; P6.6 covers the remaining cases |
| L-22 | Method-level type parameters (`<T> void f(T x)`) | Per-method `<T>` declaration | designed | blocks `Stream.fold<R>`, `Optional<int32>.Some(42)` |
| L-23 | Templated-static-factory call syntax | `Optional<int32>.Some(42)` | designed | needs L-22 |
| L-24 | `@Encoding(EncoderClass)` for views | Encoder interface for wire-format serialization | designed | `cajeta-docs/stdlib/Annotations.md` § `@Encoding`; needs `Encoder<T>` |

## Stdlib — `cajeta.lang`

| ID | Name | Description | Status | Doc / tests |
|----|------|-------------|--------|-------------|
| S-101 | `Object` base | Universal root; identity defaults | shipped | `cajeta-docs/stdlib/Lang.md` |
| S-102 | `String` (intrinsic ops) | `+`, contains, indexOf, substring, upper/lower, trim, replace, equals, size, split | shipped | `test/expression/StringMethodsTests.cpp` |
| S-103 | `String` owned vs view modes | `viewOf`, `toOwned`, lifetime tying | designed | `cajeta-docs/stdlib/Lang.md` |
| S-104 | `String.fromCodePoints` / `.repeat` / `.lines` / `getBytes` / `codePointAt` / `compare` / `lastIndexOf` | Less common String surface | designed | `cajeta-docs/stdlib/Lang.md` |
| S-105 | `Encoding` enum | Explicit encoding type for String construction | designed | UTF-8 currently hardcoded |
| S-106 | `Optional<T>` construction + extraction | `isPresent`/`isEmpty`/`get`/`orElse` | shipped | `test/parser/OptionalTests.cpp` (6), `test/parser/OptionalAndAllocateTests.cpp` (6) |
| S-107 | `Optional<T>` multiple-inherits `Stream<T>` | One-element-or-empty stream | designed | Needs L-03 |
| S-108 | `Pair<K, V>` | Two-field value type with getters | shipped | `test/parser/PairTests.cpp` (3) |

## Stdlib — `cajeta.lang.stream`

| ID | Name | Description | Status | Doc / tests |
|----|------|-------------|--------|-------------|
| S-201 | `Stream<T>` base + `next()` protocol | Pull-protocol root + default empty | shipped | `cajeta-docs/stdlib/Streams.md` |
| S-202 | Stream terminals: `count` / `forEach` | No-lambda + first lambda terminal | shipped | `test/parser/StreamTests.cpp` (9) |
| S-203 | Stream terminals: `anyMatch` / `allMatch` / `noneMatch` / `findFirst` / `reduce` | Lambda-taking terminals | shipped | `test/parser/StreamTerminalTests.cpp` (13) |
| S-204 | Stream terminal: `collect(Collector<T, R>)` | Needs S-501 Collector | designed | `cajeta-docs/stdlib/Streams.md` |
| S-205 | Stream terminal: `fold<R>(R seed, (R, T) -> R fn)` | Cross-type fold | designed | Needs L-22 method-level templates |
| S-206 | `ArrayStream<T>` + `T[].stream()` intrinsic | Array-backed stream | shipped | `test/parser/StreamTests.cpp` (9) |
| S-207 | `TakeStream<T>` | `take(n)` wrapper | shipped | `test/parser/StreamIntermediateTests.cpp` (5 of 19) |
| S-208 | `SkipStream<T>` | `skip(n)` wrapper | shipped | `test/parser/StreamIntermediateTests.cpp` (4 of 19) |
| S-209 | `FilterStream<T>` | `filter(pred)` wrapper | shipped | `test/parser/StreamIntermediateTests.cpp` (3 of 19) |
| S-210 | `MapStream<T, R>` | `map(fn)` wrapper — element-type-changing | shipped | `test/parser/StreamIntermediateTests.cpp` (3 of 19) |
| S-211 | `PeekStream<T>` | `peek(fn)` side-effecting passthrough | shipped | `test/parser/StreamIntermediateTests.cpp` (2 of 19) |
| S-212 | `FlatMapStream<T, R>` | `flatMap(fn)` flattening wrapper | shipped | `test/parser/StreamIntermediateTests.cpp` (2 of 19) |
| S-213 | `TakeWhileStream` / `DropWhileStream` | Predicate-based take/skip | designed | `cajeta-docs/stdlib/Streams.md` |
| S-214 | `DistinctStream` / `EnumerateStream` / `ZipStream` / `ChainStream` / `SortedStream` / `WindowedStream` | Remaining intermediate combinators | designed | `cajeta-docs/stdlib/Streams.md` |

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
| S-403 | `Hasher` interface | Streaming `writeT(...)` + `finish()` contract | designed | `cajeta-docs/stdlib/Hashing.md` |
| S-404 | `XXHash3` algorithm class | Fast general-purpose | designed | |
| S-405 | `RapidHash` algorithm class | Max throughput | designed | |
| S-406 | `SipHash` algorithm class | DoS-resistant | designed | |
| S-407 | `MD5` algorithm class | Checksum/identifier (not security) | designed | |
| S-408 | `DefaultHasher` algorithm class | What `Object.hash()` uses internally | designed | |
| S-409 | `Hash.identity` parameter widening to `Object` | Universal-Object accepts any class | designed | Needs the universal-Object class hierarchy |

## Stdlib — `cajeta.time`

All `cajeta.time` is **designed, unimplemented**. See `cajeta-docs/stdlib/Time.md`.

| ID | Name | Description | Status |
|----|------|-------------|--------|
| S-501 | `Clock` static surface | nanoTime / millisTime / now | designed |
| S-502 | `Instant` value type | UTC moment, ns precision | designed |
| S-503 | `Duration` value type | Time-based amount | designed |
| S-504 | `Period` value type | Calendar-based amount | designed |
| S-505 | `LocalDate` / `LocalTime` / `LocalDateTime` | Zone-naive date/time | designed |
| S-506 | `ZoneId` / `ZoneOffset` / `ZonedDateTime` | Time-zone resolution | designed |
| S-507 | `DateTimeFormatter` | Pattern-based formatting + parsing | designed |

## Stdlib — `cajeta.io` / `.file` / `.net`

All `cajeta.io.*` is **designed, unimplemented**. See `cajeta-docs/stdlib/Io.md`,
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
| S-801 | Fiber runtime (R1-R5-A') | Stackful fibers, scheduler, timer wheel, reactor | shipped | `cajeta-docs/AsyncStatus.md` |
| S-802 | `Task<T>` + `spawn` syntax | Lightweight async tasks | shipped | `test/parser/TaskTypingTests.cpp`, `SpawnDropTests.cpp`, `AsyncSyntaxTests.cpp`, `DetachTests.cpp`, `PerFiberDropChainTests.cpp` |
| S-803 | `Lock` class + intrinsics | Mutex via `Cajeta.lockNew()` etc. | shipped | `test/parser/LockIntrinsicTests.cpp`, `LockClassTests.cpp` |
| S-804 | `Fiber` / `Thread` cajeta-source classes | Surface wrappers around runtime | designed | Runtime exists; surface classes haven't been declared |
| S-805 | R5-C / R5-D runtime items | Per AsyncStatus.md | designed | `cajeta-docs/AsyncStatus.md` |

## Stdlib — `cajeta.error`

| ID | Name | Description | Status | Doc / tests |
|----|------|-------------|--------|-------------|
| S-901 | Exception hierarchy (Throwable / Exception / RecoverableException / UnrecoverableException) | Root types | shipped | `test/parser/ErrorModelTests.cpp` |
| S-902 | `try` / `catch` / `throw` syntax | Standard control flow | shipped | `test/expression/SwitchAndExceptionTests.cpp` |
| S-903 | `try-with-resources` | Auto-close on scope exit | partial | `test/parser/TryWithResourcesTests.cpp` |
| S-904 | Stack-trace capture on throw | DWARF-symbolicated trace in exception payload | designed | P4.5 debug-mode feature; `cajeta-docs/CompilerModes.md` |

## Aspect model — `cajeta.aspect`

| ID | Name | Description | Status | Doc / tests |
|----|------|-------------|--------|-------------|
| S-1001 | `@Component` / `@Inject` DI | Dependency injection annotations | shipped | `cajeta-docs/stdlib/AspectModel.md`; `test/parser/ComponentRegistrationTests.cpp`, `InjectCodegenTests.cpp`, `NamedInjectTests.cpp` |
| S-1002 | `@Before` / `@After` / `@Around` advice | Aspect-oriented method wrapping | shipped | `cajeta-docs/stdlib/AspectModel.md`; `test/parser/AdviceCodegenTests.cpp`, `AroundAdviceTests.cpp`, `AfterReturningThrowingTests.cpp` |
| S-1003 | Pointcut matching | Method-name + annotation matchers | shipped | `test/parser/PointcutMatchingTests.cpp` |
| S-1004 | `@Inject` vs implicit injection | Decision: keep `@Inject` or drop in favor of auto | pending | `cajeta-docs/stdlib/AspectModel.md` |

## Annotations

See `cajeta-docs/stdlib/Annotations.md` for the full catalog. The annotation-
arg machinery is shipped (L-13); individual handlers are tracked
under the language and stdlib sections above.

## Tracking conventions

- Each "shipped" row must link to at least one pinning test file.
- "Partial" rows should describe what's missing in the description.
- "Designed" rows should link to the design doc.
- When promoting a feature from designed→partial→shipped, update
  the status and add the test reference in the same commit as the
  implementation.
