# Cajeta language tour

A single-project walkthrough of the Cajeta language: allocation, ownership, classes, inheritance, templates, streams, collections, lambdas, errors, and Lombok-style annotations. Each topic is a `demo*` method on the entry class `tour.Tour` — read top-to-bottom and you'll have seen every load-bearing feature.

```
samples/tour/
├── README.md              ← you are here
├── build-bin.sh           ← compile + link to a native binary
├── build-ir.sh            ← compile to LLVM IR (.ll per module)
└── src/tour/
    ├── Tour.cajeta        ← entry point + all demo methods
    ├── Point.cajeta       ← basic class with fields + ctor
    ├── Counter.cajeta     ← mutable class used by Classes + Lambdas demos
    ├── Shape.cajeta       ← virtual base class
    ├── Square.cajeta      ← override demo
    ├── Circle.cajeta      ← override demo
    └── Book.cajeta        ← @Builder + @Builder.Default + @ToString
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

# LLVM IR archive (produces build/ir/**/*.ll, one per source module):
./build-ir.sh
ls build/ir/
```

Expected binary output:

```
=== Cajeta language tour ===

-- Allocation --
  stack Point(3,4).distSq() = 25
  heap  Point(5,12).distSq() = 169

-- Ownership --
  take(#Point(7,24)).distSq() = 625

-- Classes --
  Counter after 3 bumps = 3

-- Inheritance --
  Square(5).area() = 25
  Circle(3).area() = 27

-- Templates --
  Stream<int32>  sum = 15
  Stream<float64> sum (truncated) = 7

-- Streams --
  sum of evens in [1..10] = 30
  sum of squares in [1..10] = 385

-- Collections --
  ArrayList count = 3
  ArrayList sum   = 60

-- Lambdas --
  sum via forEach + captured Counter = 10

-- Errors --
  caught throw value = 99

-- Annotations --
  Book.title  = The Mythical Man-Month
  Book.pages  = 322
  default pages = 100
=== tour complete ===
PASS
```

Exit code is the number of self-detected demo failures (0 on success).

## What each demo covers

### `demoAllocation` — `stack` vs `heap`

```cajeta
Point onStack = stack Point(3, 4);          // current frame; dropped at scope exit
Point onHeap  = heap  Point(5, 12);         // malloc'd; freed by drop chain
```

`stack`/`heap` are mandatory at every allocation; `new` is removed. The type `Point` is the same regardless of storage — the borrow checker tracks lifetime as metadata, not the type. Pass either to a function taking `Point` and it just works.

### `demoOwnership` — `#`-transfer

```cajeta
Point owner = heap Point(7, 24);
int32 hypoSq = take(#owner);
// `owner` is moved here; reading it below would be CAJETA_ERROR_USE_AFTER_MOVE.
```

Plain `=` is a borrow; `#name` transfers ownership and marks the source moved at compile time. The borrow checker also catches escape-on-return, alias-mutation, and definite-assignment violations.

### `demoClasses` — fields, ctor, methods

A `Counter` with a mutable internal field and a `bump()` method. Demonstrates that field state survives method calls on a stack-allocated instance.

### `demoInheritance` — virtual dispatch via hash-based vtable

```cajeta
Shape sq = stack Square(5);   // 25
Shape ci = stack Circle(3);   // 27 (toy integer area)
sq.area();   // dispatches to Square.area
ci.area();   // dispatches to Circle.area
```

Single inheritance for state; multiple inheritance for behavior is supported but not exercised here (see `cajeta-docs/stdlib/UnifiedClasses.md` § Inheritance). Override is via the `override` modifier; vtable lookup is hash-based so diamond inheritance resolves uniformly.

### `demoTemplates` — monomorphization-per-instantiation

```cajeta
int32[]   ints = {1, 2, 3, 4, 5};
float64[] dbls = {1.5, 2.5, 3.5};
ints.stream().reduce(0, (a, b) -> a + b);     // Stream<int32>::reduce → 15
dbls.stream().reduce(0.0, (a, b) -> a + b);   // Stream<float64>::reduce → 7.5
```

Each `(Stream, T)` pair materializes as a distinct runtime type with its own vtable — no type erasure. The same `stream()` / `reduce` source compiles to two genuinely different functions for the two T's.

### `demoStreams` — pull-protocol pipelines

```cajeta
xs.stream()
    .filter((x) -> x % 2 == 0)
    .reduce(0, (a, b) -> a + b);
```

Intermediate ops (`filter`, `map`, `flatMap`, `peek`, `take`, `skip`, …) compose; terminals (`count`, `forEach`, `reduce`, `fold`, `collect`, `anyMatch`, `allMatch`, `noneMatch`, `findFirst`) consume. `.parallel()` opt-in dispatches through the structured-concurrency driver when the source is `Splittable<T>` — not exercised here to keep the demo deterministic.

### `demoCollections` — `ArrayList<T>`

```cajeta
ArrayList<int32> list = heap ArrayList<int32>();
list.add(10); list.add(20); list.add(30);
list.count();                                                  // 3
list.stream().reduce(0, (a, b) -> a + b);                      // 60
```

`HashMap<K, V>` / `HashSet<T>` / `LinkedList<T>` follow the same `count()` / `stream()` convention. HashMap's stream views (`keys()` / `values()` / `entries()`) are `Splittable<T>`, so `.parallel()` actually splits the work.

### `demoLambdas` — bare-identifier params + capture by borrow

```cajeta
Counter accum = stack Counter();
xs.stream().forEach((x) -> accum.bumpBy(x));   // accum captured by ref
accum.value();                                  // 10
```

Lambda parameter types are inferred from the surrounding context (here, `Stream<int32>.forEach((int32) -> void)`). Outer locals are captured by borrow; ownership-transfer captures (`(x) -> ... use(#outerOwner) ...`) work too.

### `demoErrors` — `try` / `catch`

```cajeta
try {
    throw 99;
} catch (Exception e) {
    int32 v = (int32) e;
}
```

`throw` accepts any value; the catch clause type-narrows. The exception hierarchy distinguishes Recoverable (catchable, propagates normally) from Unrecoverable (alarm — SIGABRTs at the uncaught boundary).

### `demoAnnotations` — `@Builder` + `@Builder.Default` + `@ToString`

```cajeta
@Builder
@ToString
public class Book {
    public String title;
    @Builder.Default public int32 pages = 100;
}

Book b = Book.builder().title("The Mythical Man-Month").pages(322).build();
Book d = Book.builder().title("Untitled").build();   // pages defaults to 100
```

The annotation processor synthesizes a fluent builder, per-field defaults, and a printable view. See `cajeta-docs/stdlib/Annotations.md` for the full annotation catalog (`@Getter` / `@Setter` / `@Data` / `@Value` / `@AutoHash` / `@EqualsAndHashCode` / `@NonNull` / `@With` / `@Aspect` / …).

## What's NOT in this tour

These are working features in the language but aren't exercised here because they'd either expand the demo significantly or hit JIT-tested paths that the binary emit isn't yet smoke-pinned for:

- **`view` types and wire formats** (`@BigEndian` / `@LittleEndian` / `@HostEndian`, fixed and variable-size fields). See `cajeta-docs/stdlib/Views.md`.
- **Parallel streams** (`.parallel()` over `Splittable<T>` with structured-concurrency workers). See `cajeta-docs/stdlib/StreamParallelism.md`.
- **Async / `scope` / `spawn`**. See `cajeta-docs/stdlib/Thread.md`.
- **JSON codec** (`Json.parse<T>` / `Json.toBytes<T>`). See `cajeta-docs/stdlib/codec/Json.md`.
- **Aspects + DI** (`@Aspect` / `@Component` / `@Inject` / `@Around` / `@Before` / `@After`). See `cajeta-docs/stdlib/AspectModel.md`.
- **Template wildcards** (`Box<? extends Animal>`, capture conversion). See `cajeta-docs/TemplateWildcard.md` and `cajeta-docs/CaptureConversion.md`.

## Known wrinkles

A few rough edges surfaced while bringing this sample up. None block the demo as written; they're listed here so the next sample can avoid them:

- **`Box<int32>.get()` returns garbage in multi-module builds** when the generic class has a single primitive-typed field. Isolated repro: works. The tour-shape repro: fails. The Templates demo uses `int32[].stream()` to dodge it.
- **`System.out` (Java idiom) silently compiles to a no-op**; the canonical receiver is `System.stdout` / `System.stderr` / `System.stdin`.
- **`printf("template {}", arg)` produces no output in binary emit** for some `{}`-template shapes. String concatenation (`"prefix " + value`) is the reliable formatter — it auto-stringifies primitives via `__cajeta_i64_to_str` / `__cajeta_f64_to_str` / `__cajeta_bool_to_str`.
- **`--emit=exe` (in-process lld link)** requires `lld-N-dev` at compiler-build time. Without it, the in-process linker prints a hint. `build-bin.sh` works around this by using `--emit=obj` + `clang-20` for the final link.
