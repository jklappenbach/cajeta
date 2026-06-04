# Cajeta

Cajeta is a hybrid systems / application language combining C++-style true templates, multiple-inheritance, and operator overloading.  It's also Java-style class semantics and annotations, It's Rust-inspired ownership for memory management — with a single explicit allocation idiom that lets the caller pick stack or heap at initialization.

The compiler is LLVM-backed (LLVM 23) and produces either IR or native binaries.  Cajeta believes in a compiler that walks you or an AI through the code with abundant linting and verbose error messages. 

Cajeta may not be your choice for embedded, but it's lean enough to perform that role.  A future roadmap will produce a version that will be able to exist even in the most austere environments.

It doesn't have the "safety" of Rust, but it offers many benefits in return:  a memory model that won't fight you.  Syntax and grammar that will make C#, Java, Javascript, and Typescript developers feel at home.

Comfort, all while offering true zero-copy semantics with Views, a lightening fast networking stack, and memory management free of reference counting.

Create amazing things with Cajeta.  

> **Status.** Compiler is in active development; the language design is past v1. The full test suite runs 1717 tests across 32 shards. The list of working features below is exercised by tests; anything not on the list is either in progress or under design.

---

## Version

**Current:** `0.1.0` &nbsp;·&nbsp; baked into the binary at configure time — `cajeta --version` reports it.

Versioning is manual and tied to releases. The flow:

1. Bump `VERSION` at the repo root.
2. Commit + push.
3. `git tag v$(cat VERSION) && git push --tags` — pushing the tag triggers `.github/workflows/release.yml`, which cross-builds the compiler for every supported target and uploads the binaries to the GitHub Release.

Supported binary targets (see [RELEASING.md](RELEASING.md) for the full matrix):

| Target | Triple | Runner |
|---|---|---|
| Linux x86_64 | `x86_64-linux-gnu` | `ubuntu-latest` |
| Linux ARM64 (incl. NVIDIA Grace) | `aarch64-linux-gnu` | `ubuntu-24.04-arm` |
| macOS Apple Silicon | `aarch64-apple-darwin` | `macos-14` |
| Windows x86_64 | `x86_64-pc-windows-msvc` | `windows-latest` |
| Linux RISC-V 64 *(best-effort, QEMU-cross)* | `riscv64-linux-gnu` | `ubuntu-latest` |

---

## Table of contents

- [Version](#version)
- [Quick taste](#quick-taste)
- [Building](#building)
- [Running the tests](#running-the-tests)
- [Language reference](#language-reference)
  - [Allocation: `stack` and `heap`](#allocation-stack-and-heap)
  - [Ownership and `#`-transfer](#ownership-and--transfer)
  - [Classes, inheritance, and dispatch](#classes-inheritance-and-dispatch)
  - [Templates and wildcards](#templates-and-wildcards)
  - [Views (`view`) and wire formats](#views-view-and-wire-formats)
  - [Lambdas and method references](#lambdas-and-method-references)
  - [Streams and parallel terminals](#streams-and-parallel-terminals)
  - [Annotations](#annotations)
  - [Aspects, DI, advice](#aspects-di-advice)
  - [Concurrency (`async`, `scope`, `spawn`)](#concurrency-async-scope-spawn)
  - [Errors and stack traces](#errors-and-stack-traces)
  - [JSON codec](#json-codec)
- [Compiler modes and debug flags](#compiler-modes-and-debug-flags)
- [Primitive types](#primitive-types)
- [Documentation map](#documentation-map)

---

## Quick taste

```cajeta
package demo;

import cajeta.collection.ArrayList;

public class Point {
    public int32 x;
    public int32 y;
    public Point(int32 x, int32 y) { this.x = x; this.y = y; }
    public int32 dist2() { return this.x * this.x + this.y * this.y; }
}

public final class App {
    public static int32 run() {
        Point p = stack Point(3, 4);          // stack-allocated, dropped on scope exit
        Point q = heap  Point(5, 12);         // heap-allocated, freed on scope exit
        ArrayList<Point> ps = heap ArrayList<Point>();
        ps.add(#q);                            // ownership of q transfers into the list

        return p.dist2()                       // 25
             + ps.stream()
                 .map<int32>((pt) -> pt.dist2())
                 .reduce(0, (a, b) -> a + b);  // 169
    }
}
```

`stack`/`heap` are mandatory at every allocation. `new` is removed.

---

## Building

The compiler is configured via CMake and built with Ninja. Two scripts wrap the typical flow:

```sh
./setup.sh    # one-time: installs deps (Linux apt / macOS brew), runs cmake into build/
./build.sh    # incremental ninja build; runs every time you change code
```

### Build prerequisites

`./setup.sh` installs system packages for you. The Ubuntu apt list, if you'd rather drive it manually:

```sh
sudo apt install \
    cmake ninja-build clang-23 llvm-23-dev libllvm23 \
    libantlr4-runtime-dev openjdk-17-jre \
    libgtest-dev libgoogle-glog-dev libzstd-dev vim-common libxxhash-dev
```

Notes:
- LLVM 23 isn't in Ubuntu's stock repos yet. If the `llvm-23-*` packages aren't found, add the [apt.llvm.org](https://apt.llvm.org) source first (`./setup.sh` does this automatically): ``echo "deb http://apt.llvm.org/$(. /etc/os-release; echo $VERSION_CODENAME)/ llvm-toolchain-$(. /etc/os-release; echo $VERSION_CODENAME)-23 main" | sudo tee /etc/apt/sources.list.d/llvm-23.list`` (after importing the repo key).
- `clang-23` is required at compiler-build time to compile `runtime/native/cajeta_runtime.c` to LLVM bitcode, which is then embedded into the Cajeta compiler binary.
- `vim-common` provides `xxd`, used to convert the bitcode bytes into a C array.
- `openjdk-17-jre` runs the bundled ANTLR4 jar in `tools/antlr/`.
- `libxxhash-dev` is the xxhash header for the cajeta.hash runtime.
- The project defaults to LLVM 23 (mainline upstream — the apt/Homebrew packages); the GPU/SPIR-V path (textures, ray query, cooperative matrix) requires it, and the `cajeta-llvm` fork's prebuilt toolchain is 23-based. Use a mainline build, not a vendor fork: ROCm's LLVM is compiled without RTTI and won't link the test JIT helper cleanly. LLVM 18 doesn't know about Zen 5 CPUs; 22 is the hard floor.
- To target a different LLVM, set `CAJETA_LLVM_VERSION` (or `LLVM_DIR`) before running setup: `CAJETA_LLVM_VERSION=23 ./setup.sh`.

### Build outputs

- `build/src/cajeta` — the compiler binary.
- `build/test/cajeta_test` — the test executable.
- `build/src/cajeta_runtime.bc` — the C runtime as LLVM bitcode.
- `build/src/cajeta_runtime_embedded.cpp` — generated C array of the bitcode bytes, linked into the compiler. The compiler links this bitcode into every output module via `CajetaModule::linkRuntime()` so cross-compilation needs no per-target runtime.

---

## Running the tests

Tests are GoogleTest. The fixture in `test/jit/JitTestHelper.{h,cpp}` compiles a Cajeta source string, links the embedded runtime into the produced LLVM module, JITs it via `llvm::orc::LLJIT`, and exposes generated functions to the C++ test as callable pointers.

> **Do not invoke `c++` / `clang++` directly on `test/main.cpp`.** The test executable depends on libgtest, the Cajeta static library, the ANTLR4 runtime, and ~100 LLVM static libs — the CMake build assembles that link line. Always use:
>
> ```sh
> ./setup.sh      # once, to generate build/
> ./build.sh      # every time you change code
> ```

```sh
# Full suite (sharded for parallelism — finishes in ~2 min on Zen 5).
./run_tests.sh

# Direct invocation. CAJETA_SOURCE_ROOT lets parse-flow tests locate sample .cajeta files.
CAJETA_SOURCE_ROOT="$PWD" ./build/test/cajeta_test

# Filter to specific suites or tests (gtest syntax).
CAJETA_SOURCE_ROOT="$PWD" ./build/test/cajeta_test '--gtest_filter=StreamTests.*'
CAJETA_SOURCE_ROOT="$PWD" ./build/test/cajeta_test '--gtest_filter=BinaryOpExpressionTests.intAdd'

# Brief output (one line per result).
CAJETA_SOURCE_ROOT="$PWD" ./build/test/cajeta_test --gtest_brief=1
```

### Debugging tip

Set `CAJETA_DUMP_IR=1` to have the JIT helper print the generated LLVM IR to stderr just before passing it to the JIT. Useful when a test fails verification:

```sh
CAJETA_DUMP_IR=1 CAJETA_SOURCE_ROOT="$PWD" \
    ./build/test/cajeta_test '--gtest_filter=BinaryOpExpressionTests.intAdd'
```

---

## Language reference

### Allocation: `stack` and `heap`

Every class instance is created with one of two explicit allocation prefixes:

```cajeta
MyClass a = stack MyClass(42);                  // stack frame; dropped on scope exit
MyClass b = heap  MyClass(7);                   // malloc'd; freed on scope exit
MyClass c = stack MyClass { field: 100 };       // stack, aggregate init
MyClass d = heap  MyClass { field: 200 };       // heap, aggregate init
MyClass e = stack MyClass();                    // default ctor
MyClass f;                                       // null reference; rejected on read until assigned
```

`MyClass` is one type whether the instance lives on the stack or the heap — the borrow checker tracks lifetime, not the type system. Class instances always pass and return by pointer, never by value (no slicing). See [`UnifiedClasses.md`](cajeta-docs/stdlib/UnifiedClasses.md).

### Ownership and `#`-transfer

Every owned value has exactly one owner. Plain assignment is a borrow; `#name` transfers ownership.

```cajeta
public void demo() {
    MyClass a = heap MyClass();
    MyClass b = a;            // borrow — a still owns; b dangles after a's drop
    MyClass c = #a;           // transfer — c owns; reading `a` after this is a compile error
    // a is no longer readable here
}
```

The borrow checker is static. Use-after-move, borrow-escape-on-return, alias-mutation, and definite-assignment violations are caught at compile time:

```
CAJETA_ERROR_USE_AFTER_MOVE
CAJETA_ERROR_BORROW_ESCAPE
CAJETA_ERROR_VARIABLE_NOT_ASSIGNED
CAJETA_ERROR_BORROW_RETURN_MULTI_PARAM
```

The drop chain is a per-thread linked list of stack-allocated entries; entries fire in reverse declaration order at scope exit, and the throw path unwinds the chain to a try-frame's watermark so drops fire on the exceptional path too. See [`MemoryModel.md`](cajeta-docs/stdlib/MemoryModel.md) and [`FieldOwnership.md`](cajeta-docs/stdlib/FieldOwnership.md).

### Classes, inheritance, and dispatch

Single inheritance for state, **multiple inheritance** for behavior (verified across grammar, vtable construction, and tests):

```cajeta
public abstract class AbstractStream<T> { ... }
public abstract class AbstractHashable<T> { ... }

public class Optional<T> extends AbstractStream<T>, AbstractHashable<T> {
    // Inherits from both bases.
}
```

Dispatch is uniform across storage modes — every class instance carries a vtable pointer at byte offset 0:

```cajeta
public class Shape  { public int32 area() { return 0; } }
public class Square extends Shape {
    int32 side;
    public Square(int32 s) { this.side = s; }
    public int32 area() { return this.side * this.side; }
}

Shape s1 = stack Square(5);   // 25 via vtable
Shape s2 = heap  Square(7);   // 49 via vtable
```

Diamond inheritance resolves via hash-based vtable lookup (`__cajeta_vtable_lookup`); `override` is required on overriding methods.

### Templates and wildcards

True C++-style templates — full monomorphization per instantiation, no type erasure. Type parameters can be class-level or method-level:

```cajeta
public class Box<T> {           // class-level template
    T value;
    public Box(T v) { this.value = v; }
    public T get() { return this.value; }
}

public class Algo {
    public static R fold<R, T>(R seed, T[] items, (R, T) -> R fn) {   // method-level template
        R acc = seed;
        for (T x : items) acc = fn(acc, x);
        return acc;
    }
}
```

**Bounded templates** (`<T extends Foo>`), **template wildcards** (`?`, `? extends Bound`, `? super Bound`), and **capture conversion** are all supported. See [`TemplateWildcard.md`](cajeta-docs/TemplateWildcard.md) and [`CaptureConversion.md`](cajeta-docs/CaptureConversion.md), and a runnable PECS + capture-read-back walk in [`samples/Tour/src/tour/WildcardsDemo.cajeta`](samples/Tour/src/tour/WildcardsDemo.cajeta).

```cajeta
public void inspect(Box<? extends Animal> b) {
    Animal a = b.get();   // capture projects through the extends bound
}

public void store(Box<? super Dog> b, Dog d) {
    b.set(d);             // PECS: write a narrower type into a super-bounded slot
}
```

Method-template inference is at least Java-strength: `fold<R>(seed, fn)` infers R from seed and fn's return shape; lambdas as arguments pick up parameter types from the formal even when the method-template has overloads.

### Views (`view`) and wire formats

A `view` is a typed window over a `byte[]` — no copy, layout dictated by endianness annotations. Useful for binary protocols, file headers, and shared-memory wire formats:

```cajeta
@LittleEndian
public view PacketHeader {
    int32 magic;
    int16 version;
    int16 flags;
    int32 payloadLength;
}

public void parse(byte[] buf) {
    PacketHeader h = stack PacketHeader(buf);   // borrow; buf still owned by caller
    if (h.magic != 0xCAFE) throw new ProtocolException(...);
    process(h.payloadLength);
}
```

Views support `@BigEndian`, `@LittleEndian`, `@HostEndian`, inline nested views, fixed-size and variable-size trailing fields, and ownership-transferred (`stack View(#buf)`) and borrow (`stack View(buf)`) construction forms. Views participate in interface dispatch via fat-pointer kind tags. See [`Views.md`](cajeta-docs/stdlib/Views.md) and a runnable end-to-end walk through all three endianness flavors plus nested views in [`samples/Tour/src/tour/ViewsDemo.cajeta`](samples/Tour/src/tour/ViewsDemo.cajeta).

### Lambdas and method references

```cajeta
// Bare-identifier params infer from context.
xs.stream().filter((x) -> x > 0).map<int64>((x) -> (int64) x).reduce(0L, (a, b) -> a + b);

// Captures: outer locals captured by borrow; transfer via #name in the capture position.
int32 total = 0;
xs.forEach((x) -> { total = total + x; });   // total captured by reference

// Method references.
Stream<String> names = ps.stream().map<String>(Person::getName);
```

Block-body lambdas, return-type inference, capture-by-borrow with lifetime tracking, and parameter-type inference from method-template formals are all working. See [`Lambdas.md`](cajeta-docs/stdlib/Lambdas.md).

### Streams and parallel terminals

A pull-protocol `Stream<T>` base in `cajeta.lang.stream` with the standard intermediate ops (`filter`, `map`, `flatMap`, `peek`, `take`, `skip`, `mapOrSkip`, `mapOrFallback`, `mapOrLog`) and terminals (`count`, `forEach`, `reduce`, `fold`, `collect`, `anyMatch`, `allMatch`, `noneMatch`, `findFirst`):

```cajeta
int32 total = xs.stream()
    .filter((x) -> x > 0)
    .map<int32>((x) -> x * x)
    .reduce(0, (a, b) -> a + b);
```

**Parallel terminals** via `.parallel()`. The driver walks the wrapper chain to find a `Splittable<T>` root (ArrayStream, HashMap.{keys,values,entries}), splits via `trySplit`, spawns workers in a `scope { ... }`, and combines per-terminal:

```cajeta
int64 sum = xs.stream().parallel()
    .fold<int64>(0L,
        (acc, v) -> acc + (int64) v,
        (a, b)   -> a + b);
```

See [`Streams.md`](cajeta-docs/stdlib/Streams.md) and [`StreamParallelism.md`](cajeta-docs/stdlib/StreamParallelism.md). A runnable walk through `reduce`, fold-with-combiner, the predicate terminals, `findFirst`→`findAny`, and `forEach` accumulation under `.parallel()` lives in [`samples/Tour/src/tour/ParallelStreamsDemo.cajeta`](samples/Tour/src/tour/ParallelStreamsDemo.cajeta).

### Annotations

A Lombok-style annotation system layered over the language's reflection. Highlights:

| Annotation                                  | Shipped | What it does |
|---------------------------------------------|---------|--------------|
| `@Getter` / `@Setter` (`access="..."`)      | ✅      | Synthesize accessors; per-field or per-class; all four access levels. |
| `@Builder` / `@Builder.Default`             | ✅      | Synthesize a fluent builder + per-field default initializers. |
| `@ToString` / `@ToString(format=...)`       | ✅      | Synthesize `toString()`; PROPERTIES or JSON shape, `of={"a","b"}` allowlist, `callSuper`, `@Exclude`. |
| `@AutoHash` / `@EqualsAndHashCode`          | ✅      | Synthesize identity/structural hash + `==`. |
| `@NonNull` (parameter position)             | ✅      | Compile-time + runtime null check. |
| `@Data` / `@Value` bundles                  | ✅      | Standard Lombok bundles. |
| `@With`                                     | ✅      | Synthesize copy-with-modification factories. |
| `@RequiredArgsConstructor` / `@AllArgsConstructor` | ✅ | Synthesize constructors. |
| `@BigEndian` / `@LittleEndian` / `@HostEndian` / `@Align` | ✅ | View layout control. |
| `@Encoding(...)`                            | ✅      | Pluggable binary codec for views (MessagePack, Protobuf, Avro, …). |
| `@SuppressLint("rule-id")`                  | ✅      | Per-decl lint suppression. |
| `@Aspect` / `@Inject` / `@Component` / `@Around` / `@Before` / `@After` | ✅ | DI + aspect weaving. |

See [`Annotations.md`](cajeta-docs/stdlib/Annotations.md) for the full catalog.

```cajeta
@Builder
public class Connection {
    @Builder.Default public int32 timeoutMs = 5000;
    @Builder.Default public int32 retries = 3;
    public String host;
    public int32 port;
}

Connection c = Connection.builder()
    .host("api.example.com")
    .port(443)
    .build();   // timeoutMs and retries get their declared defaults
```

### Aspects, DI, advice

Spring-style dependency injection plus AspectJ-style advice, woven at compile time through the LLVM IR. `@Aspect`, `@Component`, `@Inject` for DI; `@Pointcut`, `@Around`, `@Before`, `@AfterReturning`, `@AfterThrowing` for advice. See [`AspectModel.md`](cajeta-docs/stdlib/AspectModel.md) and a runnable walk through `@Before` / `@After` / `@Around` plus DI singleton identity and transitive resolution in [`samples/Tour/src/tour/AspectsDiDemo.cajeta`](samples/Tour/src/tour/AspectsDiDemo.cajeta).

```cajeta
@Aspect
public class TimingAspect {
    @Around("@annotation(Timed)")
    public Object timeIt(ProceedingJoinPoint pjp) {
        int64 start = System.currentTimeMillis();
        Object result = pjp.proceed();
        log.info("{} took {} ms", pjp.signature(), System.currentTimeMillis() - start);
        return result;
    }
}
```

### Concurrency (`async`, `scope`, `spawn`)

Structured concurrency in the style of Rust's `tokio::scope` / Kotlin's `coroutineScope`. `async fn` declares a suspendable function; `scope { ... }` is a join-on-exit block; `spawn` launches a child fiber inside a scope. Today's runtime is a cooperative single-carrier scheduler — the topology is fork-ready; once a multi-carrier scheduler lands, the same code yields wall-clock parallelism without source changes. See [`Thread.md`](cajeta-docs/stdlib/Thread.md).

```cajeta
public static async int32 fetchAll(String[] urls) {
    int32[] sizes = new int32[urls.length];
    scope {
        for (int32 i = 0; i < urls.length; i = i + 1) {
            spawn fetchOne(urls[i], sizes, i);   // scope joins all spawned before continuing
        }
    }
    int32 total = 0;
    for (int32 s : sizes) total = total + s;
    return total;
}
```

Stream `.parallel()` rides this same machinery — `ParallelDriver.reduceParallelChain` opens a `scope` and `spawn`s one worker per split share.

A runnable walk through `async` / `await` / `spawn` / `scope` / `detach` — including primitive-arg propagation, nested awaits, fork-join with a shared `Counter`, the implicit function-body scope, and fire-and-forget detach — lives in [`samples/Tour/src/tour/AsyncDemo.cajeta`](samples/Tour/src/tour/AsyncDemo.cajeta).

### Errors and stack traces

`throw` / `try` / `catch` / `finally` with `Exception` hierarchy. Recoverable vs unrecoverable distinguishes "expected, catchable" from "alarm" — unrecoverable abort the process with a SIGABRT and a stderr dump. Every throw site captures a native stack trace via `backtrace(3)` (gated by `--stack-trace-capture=on`), and the uncaught-throw handler dumps the message + trace + drop chain on exit.

```cajeta
try {
    Connection c = Connection.builder().host(null).build();
} catch (NullPointerException e) {
    log.warn("bad config: {}", e.getMessage());
}
```

The error model is in [`ErrorModel.md`](cajeta-docs/stdlib/ErrorModel.md).

### JSON codec

Symmetric tier-1 `Json.parse<T>` / `Json.toBytes<T>` synthesizers for any class shape:

```cajeta
public class User {
    public int64 id;
    public String name;
    public boolean admin;
}

byte[] bytes = "{\"id\":7,\"name\":\"alice\",\"admin\":true}".bytes();
User u = Json.parse<User>(bytes, bytes.length);   // {7, "alice", true}
byte[] out = Json.toBytes<User>(u);               // round-trip
```

Tier-3 `Json.toBytes(JsonValue)` for ad-hoc tree manipulation. See [`Json.md`](cajeta-docs/stdlib/codec/Json.md) and the runnable parse + round-trip + nested-class walk in [`samples/Tour/src/tour/JsonDemo.cajeta`](samples/Tour/src/tour/JsonDemo.cajeta).

---

## Compiler modes and debug flags

Cajeta supports `--debug`, `--release`, and `--debug-release` mode preselects that wire a coherent set of behavior flags. Individual flags override the preset:

| Flag                          | Default | What it does |
|-------------------------------|---------|--------------|
| `--bounds=on/off/trap`        | on      | Array bounds checks; `trap` uses LLVM `ubsantrap` instructions. |
| `--overflow-checks=on/off/wrapping` | on | Signed-integer overflow traps. |
| `--null-checks=on/off/trap`   | on      | Null-deref checks at borrow time. |
| `--live-set=strict/bounded/off` | strict | Per-thread live-allocation set for auto-drop discipline. |
| `--poison-free=on`            | off (JIT) / on (binary) | `memset(0xDB)` freed chunks before `free()`. |
| `--drop-chain-validate=on`    | off (JIT) / on (binary) | Invariant checks at drop-chain push/pop/mark; aborts with diagnostic on corruption. |
| `--stack-trace-capture=on`    | on      | `backtrace(3)` at every throw site. |
| `--diag-hints=on`             | on      | "Did you mean...?" suggestions on typo'd identifiers and other diagnostics. |
| `--use-after-move-rt=on`      | flag-only (impl pending) | Runtime sentinel-write backup for the static use-after-move tracker. |
| `--ub-traps=on`               | on      | Trap on divide-by-zero, oversized shift, etc. |
| `--emit=ir/exe`               | ir      | Output format: LLVM IR text or native binary. |
| `--archive=archive/exploded`  | exploded | Output layout: 7z archive or flat directory tree. |

See [`CompilerModes.md`](cajeta-docs/CompilerModes.md) for the full table and per-mode defaults.

---

## Primitive types

```
int8   int16   int32   int64   int128
uint8  uint16  uint32  uint64  uint128
float16  float32  float64  float128
float4 / float6 / float8                  (storage-only, for ML kernels)
boolean   char   byte (alias int8)
```

Literals follow Java-ish syntax: `42`, `42L`, `0xFF`, `0b1010`, `1_000_000`, `3.14`, `3.14f`, `'A'`, `"hello"`, `true`, `false`, `null`. See [`Primitives.md`](cajeta-docs/stdlib/Primitives.md) and [`FloatingPointModel.md`](cajeta-docs/stdlib/FloatingPointModel.md).

---

## Documentation map

The deep-dive specs live in `cajeta-docs/`. Start here:

| Topic                       | Doc |
|-----------------------------|-----|
| Class model + allocation    | [`UnifiedClasses.md`](cajeta-docs/stdlib/UnifiedClasses.md) |
| System I/O + env + properties | [`lang/System.md`](cajeta-docs/stdlib/lang/System.md) |
| Memory + ownership          | [`MemoryModel.md`](cajeta-docs/stdlib/MemoryModel.md) |
| Field ownership / auto-drop | [`FieldOwnership.md`](cajeta-docs/stdlib/FieldOwnership.md) |
| Templates + wildcards       | [`TemplateWildcard.md`](cajeta-docs/TemplateWildcard.md), [`CaptureConversion.md`](cajeta-docs/CaptureConversion.md) |
| Lambdas + method refs       | [`Lambdas.md`](cajeta-docs/stdlib/Lambdas.md) |
| Streams                     | [`Streams.md`](cajeta-docs/stdlib/Streams.md), [`StreamParallelism.md`](cajeta-docs/stdlib/StreamParallelism.md) |
| Annotations (Lombok-style)  | [`Annotations.md`](cajeta-docs/stdlib/Annotations.md) |
| Views / wire formats        | [`Views.md`](cajeta-docs/stdlib/Views.md) |
| Aspects + DI                | [`AspectModel.md`](cajeta-docs/stdlib/AspectModel.md) |
| Concurrency                 | [`Thread.md`](cajeta-docs/stdlib/Thread.md), [`AsyncStatus.md`](cajeta-docs/stdlib/AsyncStatus.md) |
| Errors                      | [`ErrorModel.md`](cajeta-docs/stdlib/ErrorModel.md) |
| Lints                       | [`LintRules.md`](cajeta-docs/LintRules.md) |
| Compiler modes              | [`CompilerModes.md`](cajeta-docs/CompilerModes.md) |
| Embedded targets (roadmap)  | [`Embedded.md`](cajeta-docs/Embedded.md) |
| JSON codec                  | [`stdlib/codec/Json.md`](cajeta-docs/stdlib/codec/Json.md) |
| Method-level templates      | [`MethodLevelTemplate.md`](cajeta-docs/stdlib/MethodLevelTemplate.md) |
| Multi-classing              | [`MultiClassing.md`](cajeta-docs/stdlib/MultiClassing.md) |
| Reflection                  | [`Reflection.md`](cajeta-docs/stdlib/Reflection.md) |
| Hashing                     | [`Hashing.md`](cajeta-docs/stdlib/Hashing.md) |
| I/O                         | [`stdlib/io/`](cajeta-docs/stdlib/io/) |
| Time                        | [`Time.md`](cajeta-docs/stdlib/Time.md) |

Open work is tracked in [`todo.md`](todo.md). Historical implementation milestones are under [`cajeta-docs/history/`](cajeta-docs/history/).
