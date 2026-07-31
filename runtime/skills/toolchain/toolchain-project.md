---
id: toolchain-project
applies-to: [cajeta/toolchain/project, cajeta/toolchain/manifest, cajeta/toolchain/layout]
title: Project layout, cajeta.json, and the edit→build→run loop
description: Empty directory to running program — what `cajeta init` writes, the manifest fields that matter, the standard source layout, and which loop (jit-run vs build) to use when.
---

# Starting and driving a cajeta project

Invocation detail for each verb lives in its own skill — this one is the map
from **empty directory to running program**, and the manifest/layout facts the
command skills assume.

## Empty directory → running program (verified)

```
cajeta init                      # writes cajeta.json + src/main/cajeta/com/example/basic/Main.cajeta
cajeta build                     # → build/exe/com.example.basic  (prints path + sha256)
./build/exe/com.example.basic    # → hello from com.example.basic
```

**`cajeta run` is NOT scaffolded.** The default archetype defines exactly
`build`, `clean`, `lint`, `release`, `test` — `cajeta <name>` runs a
*manifest-declared task*, so `cajeta run` fails with usage output until you add
a `tasks.run`. Check with `cajeta tasks` before assuming a verb exists
(`cajeta/toolchain/tasks`).

## Layout

```
cajeta.json                          # the manifest (project root)
src/main/cajeta/<package>/*.cajeta   # sources — dirs mirror the package
build/exe/<name>                     # built binary (gitignore build/)
```

`src/main/cajeta` is the **default source root**; a package `com.example.basic`
lives at `src/main/cajeta/com/example/basic/`. Override with
`settings.build.source-root`.

## The manifest fields that matter

```jsonc
{
  "details": {                     // identity
    "name": "com.example.basic",   // also names the default build output
    "version": "0.1.0",
    "cajeta-lang-version": "1.0"   // pins the stdlib API level
  },
  "settings": {
    "capabilities": ["filesystem", "clock"],   // filesystem|network|process|clock|env
    "dependencies": {},                        // third-party only
    "dev-dependencies": { "cajeta.testkit": "1.0.*" },
    "build": {
      "source-root":  "src/main/cajeta",       // default; state for clarity
      "target":       "host",
      "entry-method": "com.example.basic.Main::main"   // note :: — presence = binary
    }
  },
  "tasks": { "build": { "actions": [ { "action": "build", "flavor": "debug" } ] } }
}
```

Load-bearing details:

- **`entry-method` presence is the binary/library signal.** With it, `build`
  emits a native executable; **omit it and the project is a library** — `build`
  emits `build/archive/<name>-<version>.cja` instead. There is no `--emit=lib`
  at project level. (`cajeta init library` scaffolds that shape.)
- The **stdlib is built into the toolchain** — never declare `cajeta.lang`,
  `cajeta.io`, … in `dependencies`, and never fetch them. Dead-code elimination
  links only what you use. `dependencies` is for third-party libraries only.
- **Capabilities are declared, not implicit** — a program doing file I/O,
  sockets, or subprocesses must list `filesystem` / `network` / `process`.
- The manifest is **JSONC**: `//` comments are valid and the archetypes use
  them heavily.
- `${...}` substitution reads `properties`, `params`, and prior action `id`s
  (`${art.path}`, `${art.sha256}`).

## Which loop: `jit-run` or `build`?

| Situation | Use |
|---|---|
| Trying a snippet / verifying language behavior; no project needed | `cajeta jit-run <source-root> <pkg.Class.method>` — compiles to memory and runs an entry method in-process |
| Real project, want the artifact | `cajeta build` (task-driven, honors the manifest) |
| One-off AOT compile outside a project | `cajeta [flags] <entry> <source-root> <archive-root>` |

`jit-run` is the fastest edit→verify loop and needs **no `cajeta.json` at
all** — a bare source tree is enough (`cajeta/toolchain/jit-run`). Its entry
must be a **static** method; its `int32` return becomes the exit code.

## Sharp edges

- A **stray package-less `.cajeta`** anywhere under the source root crashes
  the parser — every source file needs its `package` declaration.
- `entry-method` in the manifest uses `::` (`pkg.Class::main`); `jit-run` and
  the raw compiler take the fully dotted form (`pkg.Class.method`).
- The default archetype's `test` task wires coverage instrumentation and
  plugins — see `cajeta/toolchain/testing` before editing it.

Depth: `docs/specification/buildtool/` (BuildTool.md, LibraryProjectType.md).
