# 02 — Kick the tires

Four steps to prove the install works: check the version, scaffold hello
world, compile it to an executable, run it.

## 1. Check the install

```bash
$ cajeta --version
cajeta 0.8.0 (8190ee59)
```

A version string means the toolchain is on your PATH and healthy.

## 2. Create hello world

```bash
$ cajeta init basic hello
Initialized 'basic' archetype in hello:
  cajeta.json
  src/main/cajeta/com/example/basic/Main.cajeta
$ cd hello
```

## 3. Compile to an executable

```bash
$ cajeta build
Task 'build' outputs:
  path = build/exe/com.example.basic
  sha256 = sha256:...
```

The output is a native binary.

## 4. Run it

```bash
$ ./build/exe/com.example.basic
hello from com.example.basic
```

That's the whole loop: `init` → `build` → run. Everything else — what the
manifest means, the other project types, the rest of the `cajeta` command —
comes next.

Next: [Your first project](03-your-first-project.md).
