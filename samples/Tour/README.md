# Cajeta language tour

A walkthrough of every load-bearing language feature, one class per feature. Each demo class extends `DemoClass` and overrides `execute()`; `Tour.run()` puts one instance of each demo into an array and walks the array calling `execute()` — so adding a feature means dropping a new `.cajeta` file alongside the others and bumping the `demos[]` initializer in `Tour.cajeta`.

```
samples/Tour/
├── README.md             ← you are here
├── build-bin.sh          ← compile + link to a native binary (build/tour)
├── build-uber.sh         ← compile to a runnable .cja archive (build/uber/Tour.cja)
└── src/tour/
    ├── Tour.cajeta              ← entry point — builds the demos[] array
    ├── DemoClass.cajeta         ← base class with virtual execute()
    │
    ├── AllocationDemo.cajeta    ← stack vs heap allocation
    ├── OwnershipDemo.cajeta     ← #-transfer (use-after-move is a compile error)
    ├── ClassesDemo.cajeta       ← fields, constructors, methods
    ├── InheritanceDemo.cajeta   ← virtual dispatch via vtable
    ├── TemplatesDemo.cajeta     ← Box<T>, Stream<T> monomorphization
    ├── StreamsDemo.cajeta       ← filter / map / reduce
    ├── ArrayListDemo.cajeta     ← dynamic array
    ├── HashMapDemo.cajeta       ← open-addressing hash map
    ├── HashSetDemo.cajeta       ← set-of-T
    ├── LinkedListDemo.cajeta    ← doubly-linked list
    ├── LambdasDemo.cajeta       ← closure capture by borrow
    ├── ErrorsDemo.cajeta        ← try / catch / throw
    ├── AnnotationsDemo.cajeta   ← @Builder + @Builder.Default + @ToString
    ├── PrimitivesDemo.cajeta    ← int8/16/32/64, float32/64, char, boolean
    ├── StringDemo.cajeta        ← trim / contains / substring / replace
    ├── ControlFlowDemo.cajeta   ← if / while / for / enhanced-for
    ├── MathDemo.cajeta          ← Math.max / min / clamp (method-level templates)
    ├── FormatStringDemo.cajeta  ← System.stdout.println(fmt, args...) with `{}`
    ├── ViewsDemo.cajeta         ← view types & wire formats (@BigEndian /
    │                              @LittleEndian / @HostEndian, fixed +
    │                              variable-size fields, nested views)
    ├── ParallelStreamsDemo.cajeta ← .parallel() over Splittable<T>: reduce,
    │                              fold-with-combiner, anyMatch / allMatch /
    │                              noneMatch, findFirst→findAny, forEach
    ├── AsyncDemo.cajeta         ← async / await / spawn / scope / detach —
    │                              structured concurrency over stackful fibers
    ├── JsonDemo.cajeta          ← Tier-1 JSON codec: Json.parse<T> /
    │                              Json.toBytes<T> with per-class synthesizer
    ├── AspectsDiDemo.cajeta     ← compile-time AOP + DI: @Aspect, @Before,
    │                              @After, @Around, @Component, @Inject
    ├── WildcardsDemo.cajeta     ← template wildcards <? extends T> /
    │                              <? super T> + capture-conversion
    │                              read-back (b.set(b.get()))
    ├── SwitchTernaryDemo.cajeta ← switch statement + arrow-form
    │                              switch expression + ternary + instanceof
    ├── FileIoDemo.cajeta        ← cajeta.io.file: one-shot read/write
    │                              + streaming FileReader / FileWriter
    ├── OperatorOverloadDemo.cajeta ← operator+, operator*, operator==,
    │                              operator[] / operator[]= dispatch
    │                              on a Vec2 value class and a Grid
    │                              subscript wrapper
    │
    │  (support classes used by the demos above)
    ├── Point.cajeta              ← class with fields + ctor
    ├── Counter.cajeta            ← mutable class for lambda capture
    ├── Shape.cajeta              ← virtual base
    ├── Square.cajeta             ← override of Shape.area
    ├── Circle.cajeta             ← override of Shape.area
    ├── Book.cajeta               ← @Builder / @Builder.Default / @ToString target
    └── Box.cajeta                ← Box<T> for the templates demo
```

## Build and run

The compiler binary must exist at `<repo>/build/src/cajeta`. If you haven't built it yet:

```sh
cd <repo>
./setup.sh   # one-time
./build.sh   # incremental
```

Then from this directory:

```sh
# Native binary (produces build/tour):
./build-bin.sh
./build/tour

# Runnable .cja uber archive (produces build/uber/Tour.cja):
./build-uber.sh
```

### Output modes at a glance

| Mode | Script | Output |
|---|---|---|
| Native binary | `build-bin.sh` | `build/tour` — ELF executable (~300 KB) |
| .cja uber archive | `build-uber.sh` | `build/uber/Tour.cja` — self-contained bundle (project + stdlib + deps) |

See [`cajeta-docs/Compilation.md`](../../cajeta-docs/Compilation.md) for the full output-mode reference and the `.cja` container spec.

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

1. Drop `samples/Tour/src/tour/MyFeatureDemo.cajeta` next to the others:
   ```cajeta
   package tour;

   public class MyFeatureDemo extends DemoClass {
       public void execute() {
           System.stdout.println("");
           System.stdout.println("-- My feature --");
           // ... show the feature ...
       }
   }
   ```
2. In `Tour.cajeta`, append one line to the `ArrayList<DemoClass>` initializer:
   ```cajeta
   demos.add(heap MyFeatureDemo());
   ```
   Stream iteration picks the new demo up automatically — no count to bump.

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
- **`--emit=exe` (in-process lld link)** requires `lld-N-dev` at compiler-build time. Without it, the in-process linker prints a hint. `build-bin.sh` works around this by using `--emit=obj` + `clang-20` for the final link.
