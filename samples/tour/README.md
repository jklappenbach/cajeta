# Cajeta language tour

A walkthrough of every load-bearing language feature, one class per feature. Each demo class extends `DemoClass` and overrides `execute()`; `Tour.run()` puts one instance of each demo into an array and walks the array calling `execute()` — so adding a feature means dropping a new `.cajeta` file alongside the others and bumping the `demos[]` initializer in `Tour.cajeta`.

> The tour sources (`src/main/cajeta/tour/`) are the **stdlib / language-feature**
> tour. The **XPU tour** — portable `@Kernel` programs run through the runtime
> backend dispatcher (GPU or CPU fallback) — lives in its own subfolder
> [`xpu/`](xpu/README.md), because XPU programs need the `--xpu-backend` flag
> and a device-or-CPU-fallback to run. Build + run it with `xpu/run-gpu.sh`.

This is a standard cajeta project: a `cajeta.json` manifest at the root and
sources under `src/main/cajeta/<package>/` (see
[`docs/BuildTool.md`](../../docs/specification/buildtool/BuildTool.md) and
[`samples/buildtool/basic`](../buildtool/basic)). It builds with the **cajeta
build tool** — no hand-rolled compile/link scripts.

```
samples/tour/
├── README.md             ← you are here (stdlib / language tour)
├── cajeta.json           ← build-tool manifest (build / run / release / clean tasks)
├── build.sh / build.cmd  ← `cajeta build` → build/tour (native binary)
├── run.sh   / run.cmd    ← `cajeta run`   → build + execute
├── xpu/                  ← the XPU tour (@Kernel + the runtime dispatcher)
│   ├── README.md
│   ├── run-gpu.sh        ← compile + run for any backend (default cpu)
│   └── src/tourxpu/XpuTour.cajeta
└── src/main/cajeta/tour/
    │  Demos are grouped into topic subpackages that mirror the stdlib
    │  layout (cajeta.lang, cajeta.collection, cajeta.concurrent, …).
    │  The entry point, the shared base class, and the cross-package
    │  support classes stay in the root `tour` package.
    │
    ├── Tour.cajeta              ← entry point — builds the demos[] array
    │                              (wildcard-imports each subpackage below)
    ├── DemoClass.cajeta         ← base class with virtual execute()
    ├── Point.cajeta             ← support: class with fields + ctor (shared)
    ├── Counter.cajeta           ← support: mutable class for lambda capture (shared)
    │
    ├── lang/                    ← package tour.lang — core language
    │   ├── AllocationDemo.cajeta    ← stack vs heap allocation
    │   ├── OwnershipDemo.cajeta     ← #-transfer (use-after-move is a compile error)
    │   ├── ClassesDemo.cajeta       ← fields, constructors, methods
    │   ├── InheritanceDemo.cajeta   ← virtual dispatch via vtable
    │   ├── TemplatesDemo.cajeta     ← Box<T>, Stream<T> monomorphization
    │   ├── NumericTemplatesDemo.cajeta ← Vec3<T extends Numeric> bounds
    │   ├── WildcardsDemo.cajeta     ← <? extends T> / <? super T> + capture
    │   ├── LambdasDemo.cajeta       ← closure capture by borrow
    │   ├── AnnotationsDemo.cajeta   ← @Builder + @Builder.Default + @ToString
    │   ├── PrimitivesDemo.cajeta    ← int8/16/32/64, float32/64, char, boolean
    │   ├── StringDemo.cajeta        ← trim / contains / substring / replace
    │   ├── ControlFlowDemo.cajeta   ← if / while / for / enhanced-for
    │   ├── FormatStringDemo.cajeta  ← System.stdout.println(fmt, args...) with `{}`
    │   ├── SwitchTernaryDemo.cajeta ← switch + arrow-form expr + ternary + instanceof
    │   ├── OperatorOverloadDemo.cajeta ← operator+/*/==/[]/[]= on Vec2 + Grid
    │   ├── AspectsDiDemo.cajeta     ← compile-time AOP + DI: @Aspect, @Inject, …
    │   ├── Shape/Square/Circle.cajeta ← support: virtual base + overrides
    │   ├── Book.cajeta              ← support: @Builder / @ToString target
    │   └── Box.cajeta               ← support: Box<T> for the templates demo
    │
    ├── collection/              ← package tour.collection — containers & streams
    │   ├── StreamsDemo.cajeta       ← filter / map / reduce
    │   ├── ArrayListDemo.cajeta     ← dynamic array
    │   ├── HashMapDemo.cajeta       ← open-addressing hash map
    │   ├── HashSetDemo.cajeta       ← set-of-T
    │   ├── LinkedListDemo.cajeta    ← doubly-linked list
    │   ├── HeapDemo.cajeta          ← binary heap / priority queue
    │   ├── RedBlackTreeDemo.cajeta  ← ordered map (red-black tree)
    │   ├── BPlusTreeDemo.cajeta     ← in-memory B+ tree
    │   ├── LtmBPlusTreeDemo.cajeta  ← larger-than-memory (paged) B+ tree
    │   ├── ImmutableListDemo/SetDemo/MapDemo.cajeta ← persistent collections
    │   └── Int32Encoder.cajeta      ← support: wire Encoder for LtmBPlusTree
    │
    ├── concurrent/              ← package tour.concurrent — concurrency
    │   ├── AsyncDemo.cajeta         ← async / await / spawn / scope / detach
    │   ├── ConcurrentDemo.cajeta    ← channels, atomics, mutex / rwlock / semaphore
    │   └── ParallelStreamsDemo.cajeta ← .parallel() over Splittable<T>
    │
    ├── math/                    ← package tour.math
    │   ├── MathDemo.cajeta          ← Math.max / min / clamp (method-level templates)
    │   ├── LinearAlgebraDemo.cajeta ← matrices / vectors
    │   └── QuaternionDemo.cajeta    ← quaternion rotation
    │
    ├── error/ErrorsDemo.cajeta  ← package tour.error — try / catch / throw
    ├── wire/ViewsDemo.cajeta    ← package tour.wire — view types & wire formats
    │                              (@BigEndian / @LittleEndian / @HostEndian)
    ├── time/TimeDemo.cajeta     ← package tour.time — Instant / Duration / LocalDate…
    ├── net/NetDemo.cajeta       ← package tour.net — TCP listener / stream
    ├── io/FileIoDemo.cajeta     ← package tour.io — one-shot + streaming file I/O
    └── codec/JsonDemo.cajeta    ← package tour.codec — Tier-1 JSON codec
```

## Build and run

The compiler binary must exist at `<repo>/build/src/cajeta`. If you haven't built it yet:

```sh
cd <repo>
./setup.sh   # one-time
./build.sh   # incremental
```

Then from this directory, drive everything through the build tool:

```sh
./build.sh          # cajeta build  → build/tour (native binary)
./build/tour        # run it

./run.sh            # cajeta run    → build + execute in one step
```

`build.sh` / `run.sh` are thin wrappers around the build tool; you can call it
directly instead:

```sh
cajeta build        # build/tour
cajeta run          # build + execute
cajeta release      # optimized build
cajeta clean        # wipe build/
cajeta tasks        # list the tasks defined in cajeta.json
```

### Tasks at a glance

| Task | Script | Output |
|---|---|---|
| `build`   | `build.sh` / `build.cmd` | `build/tour` — ELF executable |
| `run`     | `run.sh` / `run.cmd`     | builds `build/tour`, then executes it |
| `release` | `cajeta release`         | optimized `build/tour` |
| `clean`   | `cajeta clean`           | removes `build/` |

See [`docs/BuildTool.md`](../../docs/specification/buildtool/BuildTool.md) for the manifest/task
reference and [`docs/Compilation.md`](../../docs/specification/buildtool/Compilation.md) for the
compiler output modes.

## What you'll see

```
=== Cajeta language tour ===

-- Allocation --
  stack Point(3,4).distSq() = 25
  heap  Point(5,12).distSq() = 169

-- Ownership --
  consume(#Point(7,24)).distSq() = 625

-- Classes --
  Counter after 3 bumps = 3

-- Inheritance --
  Square(5).area() = 25
  Circle(3).area() = 27

-- Templates --
  Box<int32>(42).get() = 42
  Stream<int32>  sum = 15
  Stream<float64> sum (truncated) = 7

-- Streams --
  sum of evens in [1..10] = 30
  sum of squares in [1..10] = 385

-- ArrayList --
  ArrayList<int32> count = 3
  ArrayList<int32> sum   = 60
  ArrayList<Point> count = 3
  ArrayList<Point> xSum  = 9

-- HashMap --
  count    = 3
  get(2)   = 200

-- HashSet --
  count        = 3
  contains(20) = true
  contains(99) = false

-- LinkedList --
  count        = 3
  contains(11) = true
  contains(99) = false

-- Lambdas --
  sum via forEach + captured Counter = 10

-- Errors --
  caught throw value = 99

-- Annotations --
  Book.title    = The Mythical Man-Month
  Book.pages    = 322
  default pages = 100

-- Primitives --
  int8  = 7
  int16 = 30000
  int32 = 1000000
  int64 = 9000M
  float32 ~ 314/100
  float64 ~ 271/100
  0xCAFE_BABE (low 16) = 47806
  0b1010_0101          = 165
  boolean = true
  char    = 90

-- Strings --
  original   = '  Hello, Cajeta!  '
  trim()     = 'Hello, Cajeta!'
  contains   = true
  substring  = 'Cajeta'
  replace    = 'Hello, World!'

-- Control flow --
  while: evenSum [1..5] = 6
  while: oddSum  [1..5] = 9
  for:   5! = 120
  for-each: sum of squares = 30

-- Math --
  max(3, 7)        = 7
  min(3, 7)        = 3
  clamp(15, 0, 10) = 10
  clamp(-5, 0, 10) = 0
  clamp(5, 0, 10)  = 5

-- Format strings --
  hello Cajeta, version 1
  pi  ≈ 3.14159
  2 + 3 = 5

-- Views and wire formats --
  @BigEndian PacketHeader.magic    = 51966
  @BigEndian PacketHeader.version  = 1
  @BigEndian PacketHeader.payload  = 256
  @HostEndian CacheEntry.key       = 100
  @HostEndian CacheEntry.hits      = 7
  @LittleEndian UserRecord.id      = 9001
  @LittleEndian UserRecord.name    = Alex
  @LittleEndian UserRecord.email   = a@e.
  nested Line  dx,dy            = 3,4

-- Parallel streams --
  count          par=1000  seq=1000
  reduce sum     1..1000 = 500500
  fold combiner  1..1000 widened = 500500
  anyMatch(>500)   = true
  allMatch(>0)     = true
  noneMatch(<0)    = true
  filter+parallel allMatch(evens) = true
  findFirst(>500) found a value in (500, 1000]: true
  findFirst(>9999) miss isPresent = false
  forEach bump count over 100 elements = 100

-- Async / scope / spawn --
  await leaf()                    = 21
  await spawn leaf()              = 21
  await spawn add(17, 25)         = 42
  await spawn nested() = leaf()*2 = 42
  three independent spawn-awaits  = 21, 30, 42
  scope { 3 bumpWorkers } tally   = 111
  inner scope join — probe        = 999
  detach announce(42) — fire+forget

-- JSON codec --
  parse single object:
    id     = 42
    score  = 1000000
    active = true
    name   = alice
  parse with missing-key field:
    id (set)        = 7
    score (set)     = 99
    active (set)    = false
    name (missing)  set? false
  round-trip primitives:
    serialized length = 41
    parsed.id         = 99
    parsed.score      = 123456789
    parsed.active     = true
  parse nested object:
    id      = 1
    point.x = 10
    point.y = 20
  round-trip nested:
    serialized length = 34
    parsed.id         = 5
    parsed.point.x    = 100
    parsed.point.y    = 200

-- Aspects + DI --
  @Before + @After:
    [before]  entering @Traced method
    [body]    work() running
    [after]   leaving @Traced method
    result = 7
  @Around with proceed:
    [around]  input=5, forwarding x*3=15
    [body]    compute(15) running
    [around]  body returned 30; adding 1
    final result = 31
  DI singleton identity:
    a == b cached singleton -> b.entries = 250
  DI transitive resolution:
    Service.used()  = 3
    Service.spare() = 247

-- Template wildcards & capture conversion --
  ? extends Animal . value.tag() = 2  (Dog::tag overrides at runtime)
  ? extends Animal . value.tag() = 3  (Cat::tag at runtime)
  before super write: bAnimal.value.tag() = 1
  after  super write: bAnimal.value.tag() = 2  (Dog landed in Animal slot)
  capture read-back: b.set(b.get()) accepted; b.value.tag() = 2
  inspect(WildBox<Dog>) = 2; inspect(WildBox<Cat>) = 3

-- Switch / ternary / instanceof --
  statement switch(3) = 23  (case 2/3 fall-through arm)
  expr switch(2) = 200
  expr switch(2) via case 1,2,3 = 50
  1000 + expr switch(2) = 1007
  min(5, 10) via ternary  = 5
  min(3, 8, 6) via nested ternary = 3
  side-effect on true branch: x = 100
  cond=false ? 1.5 : 2 (coerced f64) = 2
  (int32 5) instanceof int32   = true
  (int32 5) instanceof float64 = false
  pi instanceof float64 ? "f64" : "other" = f64

-- File I/O --
  oneShot: wrote 5 bytes, readAllBytes returned 5 bytes
  streamingWriter: wrote 2 lines, total bytes = 23
  streamingReader: read 23 bytes; position after read = 23

=== tour complete ===
```

## Adding a new demo

1. Drop the file into the topic subpackage it belongs to — e.g.
   `samples/tour/src/main/cajeta/tour/lang/MyFeatureDemo.cajeta`. The
   `package` declaration must match the directory (`tour.lang` here), and
   `DemoClass` lives in the root `tour` package, so import it:
   ```cajeta
   package tour.lang;

   import tour.DemoClass;

   public class MyFeatureDemo extends DemoClass {
       public void execute() {
           System.stdout.println("");
           System.stdout.println("-- My feature --");
           // ... show the feature ...
       }
   }
   ```
   (Need a shared support class? `Point` and `Counter` are also in the root
   package — `import tour.Point;` / `import tour.Counter;`.)
2. In `Tour.cajeta`, append one line to the `ArrayList<DemoClass>` initializer:
   ```cajeta
   demos.add(heap MyFeatureDemo());
   ```
   `Tour` wildcard-imports every subpackage (`import tour.lang.*;`, …), so a
   demo in an existing subpackage needs no new import there. Stream iteration
   picks the new demo up automatically — no count to bump.

## What's NOT in this tour

These features work but aren't exercised here because they'd either expand the demo significantly, depend on environment setup, or hit JIT-tested paths that the binary emit isn't yet smoke-pinned for:

- **Networking** — covered by the test suite; deferred from the tour to keep the surface manageable.

## Known wrinkles

A few rough edges surfaced while bringing this sample up. None block the demo as written; they're listed here so the next sample can avoid them:

- ~~**`ArrayList<DemoClass>` doesn't accept class-typed elements correctly**~~ — **fixed.** Cross-file `ArrayList<UserClass>` now works through the forward-reference placeholder mechanism. Pre-fix, `CajetaClass::instantiate`'s placeholder-arg short-circuit treated forward-ref placeholders (real package) the same as T-var placeholders (empty package), silently degrading `ArrayList<UserClass>` to the bare `ArrayList` template; the fix narrows the short-circuit to T-vars only (`src/cajeta/type/TemplateInstantiator.cpp`, `MultiSourceCompileTests.arrayListOfUserClassCrossFile`).
- ~~**Lambda parameter type-inference fails** for `forEach((d) -> d.execute())` over a `Stream<DemoClass>` from `ArrayList<DemoClass>.stream()` — annotating the parameter (`(DemoClass d) -> ...`) sidesteps it.~~ — **fixed.** The MCE lambda-arg propagator now uses `paramTypes.size() < paramNames.size()` (bare-id arity) as the inference-needed signal instead of `!getResolvedType()`, and `findCandidate` skips method-template instantiations from prior call sites so `Stream<UserClass>.fold<R>(...)` candidate lookup isn't confused by an earlier cached `fold<T>` from `reduce` delegation. See `LambdaReturnInferenceTests.foldBareIdLambdaOverArrayListUserClass`.
- ~~**`Box<int32>.get()` returns garbage in multi-module builds**~~ — **fixed** (pre-seed placeholder typeParameters at prescan time, see commit `6d30574`).
- ~~**`System.out` (Java idiom) silently compiles to a no-op**~~ — **fixed.** The compiler now emits `CAJETA_ERROR_UNKNOWN_SYSTEM_STREAM` with a "did you mean `stdout`?" hint.
- ~~**`printf("template {}", arg)` produces no output in binary emit**~~ — **fixed.** The printf intrinsic checks args at lower time and now throws `CAJETA_ERROR_PRINTF_BAD_ARGS` for misshapen calls.
- **`--emit=exe` linking.** When `lld-N-dev` was present at compiler-build time, the compiler links in-process. Otherwise it shells out to the system C driver (`cc`/`clang`/`gcc`) with `-Wl,--gc-sections`, which dead-strips the stdlib code the entry point doesn't reach (e.g. the OpenSSL-backed TLS paths), so the tour links with no extra native libs. The build tool's `build` task uses this path — which is why `cajeta build` / `build.sh` work without `lld`. (A program that actually *uses* TLS would still need `-lssl`/`-lcrypto`; widening that link set is a follow-up.)
