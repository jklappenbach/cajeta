# HelloWorld

The smallest possible cajeta program — prints `Hello world.` and exits 0.

```
samples/HelloWorld/
├── README.md
├── build-ir.sh                ← compile to LLVM IR (.ll per module)
├── build-bin.sh               ← compile + link to a native binary
└── src/helloworld/
    └── HelloWorld.cajeta      ← `package helloworld;` + `public static int32 run()`
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
# Native binary (produces build/HelloWorld):
./build-bin.sh
./build/HelloWorld
# → Hello world.

# LLVM IR archive (produces build/ir/**/*.ll, one per module):
./build-ir.sh
ls build/ir/
```

## What's in the source

```cajeta
package helloworld;

public final class HelloWorld {
    public static int32 run() {
        System.stdout.println("Hello world.");
        return 0;
    }
}
```

- `package helloworld;` — every file declares its package; the path under `src/` must match (`src/helloworld/HelloWorld.cajeta` ↔ `helloworld.HelloWorld`).
- `public static int32 run()` — the static no-arg entry method the compiler wires up as the program's C `main`. Its `int32` return becomes the exit status.
- `System.stdout.println(...)` — cajeta's stdout intrinsic; see [`cajeta-docs/stdlib/lang/System.md`](../../cajeta-docs/stdlib/lang/System.md) for the full surface (multi-arg `{}` formatting, `System.env`, `System.property`, …).

## Next steps

- [`samples/tour/`](../tour/) — a walkthrough of every load-bearing language feature in one project.
- [`README.md`](../../README.md) — top-level language reference.
