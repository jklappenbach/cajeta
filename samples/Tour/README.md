# Cajeta language tour

A walkthrough of every load-bearing language feature, one class per feature. Each demo class extends `DemoClass` and overrides `execute()`; `Tour.run()` puts one instance of each demo into an array and walks the array calling `execute()` — so adding a feature means dropping a new `.cajeta` file alongside the others and bumping the `demos[]` initializer in `Tour.cajeta`.

```
samples/Tour/
├── README.md             ← you are here
├── build-bin.sh          ← compile + link to a native binary (build/tour)
├── build-cja.sh          ← compile to a single .cja archive (build/cja/Tour.cja)
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

# Single .cja archive (produces build/cja/Tour.cja):
./build-cja.sh
```

### Output modes at a glance

| Mode | Script | Output |
|---|---|---|
| Native binary | `build-bin.sh` | `build/tour` — ELF executable (~300 KB) |
| .cja archive | `build-cja.sh` | `build/cja/Tour.cja` — uber-form bundle (~415 KB) |

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
  count = 3
  sum   = 60

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
2. In `Tour.cajeta`, bump the array length and append the instance:
   ```cajeta
   DemoClass[] demos = new DemoClass[19];
   ...
   demos[18] = heap MyFeatureDemo();
   ```
3. Bump the loop bound to match (`i < 19`).

## What's NOT in this tour

These features work but aren't exercised here because they'd either expand the demo significantly, depend on environment setup, or hit JIT-tested paths that the binary emit isn't yet smoke-pinned for:

- **`view` types and wire formats** (`@BigEndian` / `@LittleEndian` / `@HostEndian`, fixed and variable-size fields). See [`cajeta-docs/stdlib/Views.md`](../../cajeta-docs/stdlib/Views.md).
- **Parallel streams** (`.parallel()` over `Splittable<T>` with structured-concurrency workers). See [`cajeta-docs/stdlib/StreamParallelism.md`](../../cajeta-docs/stdlib/StreamParallelism.md).
- **Async / `scope` / `spawn`**. See [`cajeta-docs/stdlib/Thread.md`](../../cajeta-docs/stdlib/Thread.md).
- **JSON codec** (`Json.parse<T>` / `Json.toBytes<T>`). See [`cajeta-docs/stdlib/codec/Json.md`](../../cajeta-docs/stdlib/codec/Json.md).
- **Aspects + DI** (`@Aspect` / `@Component` / `@Inject` / `@Around` / `@Before` / `@After`). See [`cajeta-docs/stdlib/AspectModel.md`](../../cajeta-docs/stdlib/AspectModel.md).
- **Template wildcards** (`Box<? extends Animal>`, capture conversion). See [`cajeta-docs/TemplateWildcard.md`](../../cajeta-docs/TemplateWildcard.md) and [`cajeta-docs/CaptureConversion.md`](../../cajeta-docs/CaptureConversion.md).
- **Switch statements + switch expressions, ternary + instanceof, file I/O, networking** — covered by the test suite; deferred from the tour to keep the surface manageable.

## Known wrinkles

A few rough edges surfaced while bringing this sample up. None block the demo as written; they're listed here so the next sample can avoid them:

- **`ArrayList<DemoClass>` doesn't accept class-typed elements correctly**. `add()` on `ArrayList<T>` where T is a class type does not increment `sizeCount` after the slot write — the resulting list reports count 0 and `stream()` yields nothing. Bare `DemoClass[]` (a native array via `new DemoClass[N]`) works fine and is what the tour uses.
- **Lambda parameter type-inference fails** for `forEach((d) -> d.execute())` over a `Stream<DemoClass>` from `ArrayList<DemoClass>.stream()` — annotating the parameter (`(DemoClass d) -> ...`) sidesteps it.
- ~~**`Box<int32>.get()` returns garbage in multi-module builds**~~ — **fixed** (pre-seed placeholder typeParameters at prescan time, see commit `6d30574`).
- ~~**`System.out` (Java idiom) silently compiles to a no-op**~~ — **fixed.** The compiler now emits `CAJETA_ERROR_UNKNOWN_SYSTEM_STREAM` with a "did you mean `stdout`?" hint.
- ~~**`printf("template {}", arg)` produces no output in binary emit**~~ — **fixed.** The printf intrinsic checks args at lower time and now throws `CAJETA_ERROR_PRINTF_BAD_ARGS` for misshapen calls.
- **`--emit=exe` (in-process lld link)** requires `lld-N-dev` at compiler-build time. Without it, the in-process linker prints a hint. `build-bin.sh` works around this by using `--emit=obj` + `clang-20` for the final link.
