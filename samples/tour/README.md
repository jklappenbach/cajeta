# Cajeta language tour

A walkthrough of every load-bearing stdlib and language feature, one demo
class per topic. Each demo extends `DemoClass` and overrides `execute()`;
`Tour.main()` builds an `ArrayList` of demos and runs them in order — so
adding a feature means dropping a new `.cajeta` file into the right topic
package and adding one `demos.add(heap NewDemo());` line in `Tour.cajeta`.

**The demo contract** (`DemoClass`): print to narrate, `check()` to verify.
Every expectation a demo computes goes through `this.check(condition, name)`;
a failed check prints a `FAIL:` line and fails the run. Printing is for the
reader — the checks are the gate.

> The tour sources (`src/main/cajeta/tour/`) are the **stdlib /
> language-feature** tour. Environment-dependent areas live in their own
> entry points under this folder, each a self-checking program with its own
> README and run script:
>
> - [`xpu/`](xpu/README.md) — portable `@Kernel` programs through the runtime
>   backend dispatcher (GPU or CPU fallback; needs the `--xpu-backend` flag).
>   `xpu/run-xpu.sh`.
> - [`ifx/`](ifx/README.md) — the window/input/audio backend contract, driven
>   headlessly against the Null backend floor. `ifx/run-ifx.sh`.
> - [`tls/`](tls/README.md) — a TLS handshake + echo over loopback with a
>   run-time-generated self-signed cert. `tls/run-tls.sh`.
> - [`coco/`](coco/README.md) — the **coverage** tour: one class per finding
>   `cajeta-coco` can report (dead code, untested-but-reachable, a surviving
>   mutant, a high-CRAP method, a redundant test) and what each looks like in
>   the IDE. `coco/run-coco.sh`. Deliberately imperfect, unlike everything
>   else here — which is why it lives in its own entry point and not in the
>   language tour's sources.

This is a standard cajeta project: a `cajeta.json` manifest at the root and
sources under `src/main/cajeta/<package>/` (see
[`docs/BuildTool.md`](../../docs/specification/buildtool/BuildTool.md) and
[`samples/buildtool/basic`](../buildtool/basic)). It builds with the **cajeta
build tool** — no hand-rolled compile/link scripts.

Two repo gates keep the tour honest (`scripts/` at the repo root):

- `check-tour.sh` — every `*Demo.cajeta` is registered in `Tour.cajeta`, the
  tour builds with zero warnings, runs, exits 0, and every self-check holds.
- `check-tour-coverage.sh` — every stdlib package with `@EntryPoint`-tagged
  methods is imported by some tour source.

## Layout

Demos are grouped into topic subpackages mirroring the stdlib layout
(`cajeta.lang` → `tour.lang`, …). The entry point (`Tour.cajeta`), the shared
base class (`DemoClass.cajeta`), and cross-package support classes
(`Point`, `Counter`) stay in the root `tour` package.

```
src/main/cajeta/tour/
├── Tour.cajeta            ← entry point: builds the demo list, runs it
├── DemoClass.cajeta       ← base class: execute() + the check() contract
├── Point.cajeta           ← support: class with fields + ctor (shared)
├── Counter.cajeta         ← support: mutable class for lambda capture
│
├── lang/                  ← core language
│   ├── AllocationDemo         stack vs heap, the drop chain
│   ├── OwnershipDemo          `#` transfer (use-after-move = compile error)
│   ├── ClassesDemo / InheritanceDemo / MultiInheritanceDemo / InterfacesDemo
│   ├── TemplatesDemo / NumericTemplatesDemo / WildcardsDemo
│   ├── LambdasDemo / NamedArgsDemo / OptionalDemo / PairDemo / FactoryDemo
│   ├── SwitchTernaryDemo / ControlFlowDemo / PrimitivesDemo
│   ├── StringDemo / FormatStringDemo / EncodingDemo (the @Encoding annotation)
│   ├── GuidDemo               parse/format + GuidFormatException handling
│   ├── AnnotationsDemo (@Builder/@ToString) / ReflectionDemo / AspectsDiDemo
│   ├── OperatorOverloadDemo / StaticFieldsDemo / StaticNestedDemo
│   └── Shape / Square / Circle / Book / Box   (support classes)
│
├── error/
│   └── ErrorsDemo             hierarchy, typed catch, cause chain, finally
│
├── collection/
│   ├── StreamsDemo / CollectDemo / CollectorsDemo
│   ├── ArrayListDemo / LinkedListDemo / HashMapDemo / HashSetDemo
│   ├── HeapDemo / RedBlackTreeDemo / BPlusTreeDemo
│   ├── LtmBPlusTreeDemo       disk-backed B+ tree, cold-reopen recovery
│   ├── CacheDemo              LRU promotion + TTL expiry
│   ├── SortDemo               stable sort over records
│   ├── ImmutableListDemo / ImmutableSetDemo / ImmutableMapDemo
│   └── Int32Encoder           support: wire Encoder for the LTM tree
│
├── concurrent/
│   ├── AsyncDemo              async / await / spawn / scope / detach
│   ├── ConcurrentDemo         channels, atomics, mutex / rwlock / semaphore
│   └── ParallelStreamsDemo    .parallel() over Splittable<T>
│
├── io/
│   ├── FileIoDemo             one-shot, WRITE vs APPEND, EOF loop, cleanup
│   ├── PathDemo / BufferDemo
│
├── net/
│   ├── NetDemo / UdpDemo / ServerDemo   loopback TCP / UDP / HTTP+WS
│   ├── DnsDemo / UriDemo
│
├── math/
│   ├── TensorDemo / LinearAlgebraDemo (Vec/Mat) / GeometryDemo
│   ├── LinAlgDemo             solve/LU/QR/cholesky/svd/eigh/lstsq/norms
│   ├── StatsDemo              buckets, order stats, cov, special functions
│   ├── AutogradDemo           Grad / Vmap / Jit + the eager Tape
│   ├── FftDemo / PolyDemo / QuaternionDemo / RandomDemo / MathDemo
│   └── NpyDemo                .npy round trip to disk (and cleans up)
│
├── codec/
│   └── JsonDemo / CsvDemo / Base64Demo
├── wire/
│   └── SchemaDemo / ViewsDemo (@BigEndian / @LittleEndian / @HostEndian)
├── hash/HashDemo              digests pinned to RFC vectors
├── process/ProcessDemo        spawn, pipe, reap (no zombies)
├── search/SearchDemo          fuzzy typo-tolerant lookup
└── time/TimeDemo              Instant / Duration / LocalDate
```

## Build and run

The compiler binary must exist at `<repo>/build/src/cajeta`. If you haven't
built it yet:

```sh
cd <repo>
./setup.sh   # one-time
./build.sh   # incremental
```

Then from this directory, drive everything through the build tool:

```sh
./build.sh            # cajeta build  → build/exe/tour (native binary)
./build/exe/tour      # run it

./run.sh              # cajeta run    → build + execute in one step
```

`build.sh` / `run.sh` are thin wrappers around the build tool; you can call
it directly instead: `cajeta build | run | release | clean | tasks`.

## What you'll see

Each demo prints a short narrated section and verifies itself, e.g.:

```
-- Training loop (nn + optim) --
  1. Linear(2,1): 2 parameters, declared order: weight, bias
  2. SGD(lr 0.3, momentum 0.9), 300 steps: mse 227.4 -> 0.002
  3. held-out ride (3.0 km, 11 min): predicted $14.99, true $15.00
  ...

-- Errors (hierarchy, narrowing, cause, finally) --
  bad value  -> ConfigException: port must be numeric, got "http"
  wrapped    -> service startup aborted <- caused by: port 80 outside ...
```

The run ends with the self-check tally (about 300 checks across ~80 demos);
any `FAIL:` line makes `check-tour.sh` red.

## Adding a demo

1. Create `src/main/cajeta/tour/<topic>/YourDemo.cajeta`:

   ```cajeta
   package tour.<topic>;

   import tour.DemoClass;

   // One comment block: what this demonstrates, why the pattern is right.
   public class YourDemo extends DemoClass {
       public void execute() {
           System.stdout.println("");
           System.stdout.println("-- Your topic --");
           // narrate with println; verify with check()
           this.check(1 + 1 == 2, "example check");
       }
   }
   ```

2. Add `demos.add(heap YourDemo());` in `Tour.cajeta` (topic packages are
   wildcard-imported).
3. Run `./run.sh` here, then `./scripts/check-tour.sh` and
   `./scripts/check-tour-coverage.sh` from the repo root.
