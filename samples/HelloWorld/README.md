# HelloWorld

The smallest possible cajeta program — prints `Hello world.` and exits 0.

The binary is **~36 KB** on x86_64 Linux. The cajeta compiler emits one ELF section per function and global (`FunctionSections` / `DataSections`), the link step runs `--gc-sections` to drop everything HelloWorld doesn't reference (the JSON codec, hash families, parallel-stream driver, …), and `--strip-all` removes the debug + symbol tables. Set `DEBUG=1 ./build-bin.sh` to keep symbols for a debuggable build (~170 KB).

```
samples/HelloWorld/
├── README.md
├── build-ir.sh                ← compile to exploded LLVM IR (.ll per module)
├── build-cja.sh               ← compile to a project-only .cja library archive
├── build-uber.sh              ← compile to a runnable uber .cja archive
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

# Exploded LLVM IR tree (one .ll per module under build/ir/):
./build-ir.sh
ls build/ir/

# Project-only .cja library archive (build/cja/HelloWorld.cja):
./build-cja.sh

# Runnable uber .cja archive (build/uber/HelloWorld.cja):
./build-uber.sh
```

### Output modes at a glance

| Mode | Script | Output |
|---|---|---|
| Native binary | `build-bin.sh` | `build/HelloWorld` — 36 KB ELF executable |
| Exploded IR | `build-ir.sh` | `build/ir/{...}.ll` — one text-IR file per module |
| Cja archive | `build-cja.sh` | `build/cja/HelloWorld.cja` — project-only library form (no stdlib, no deps) |
| Uber archive | `build-uber.sh` | `build/uber/HelloWorld.cja` — project + stdlib + transitively-referenced deps under `deps/<name>-<ver>/` |

See [`cajeta-docs/Compilation.md`](../../cajeta-docs/Compilation.md) for the full output-mode reference and the `.cja` container spec.

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
