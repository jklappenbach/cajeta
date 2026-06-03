# `basic` — single-package buildtool sample

The simplest cajeta project shape: one manifest, one source tree,
a handful of tasks. Mirrors what `cajeta init` writes by default.

## Layout

```
basic/
├── cajeta.json                 # manifest
├── run.sh                       # demonstration script
└── src/
    ├── main/
    │   ├── cajeta/              # production source
    │   │   └── com/example/basic/Main.cajeta
    │   └── resources/           # bundled into the archive
    └── test/
        ├── cajeta/              # test source
        └── resources/           # test fixtures (overlay precedence)
```

## What this sample demonstrates

- The canonical six-block manifest: `details`, `properties`,
  `settings`, `actions`, `plugins`, `tasks`.
- Properties used to centralize the stack version
  (`${stack-version}`) across multiple `dependencies` entries.
- Per-task profile literals — `build` uses `dev` (parameterized
  so `-p profile=foo` overrides), `test` uses `test`, `release`
  uses `release`. No central `--profile` activation flag.
- The four starter tasks: `build`, `test`, `release`, `clean`.

## What today's `run.sh` exercises

Today's build tool can:

- Parse + validate the manifest.
- List tasks (`cajeta tasks`).
- Render the resolved structure of a task (`cajeta task <name> --show`).
- Resolve and print properties (`cajeta info --properties`).
- Write a lockfile (`cajeta info --write-lockfile`).
- Run tasks whose actions are entirely covered by Phase 0-6a
  (exec, copy, delete, mkdir, sign, verify-sig, version,
  download, build).

The `build` task itself (Phase 5a) will fork the cajeta compiler
and try to compile the source under `src/main/cajeta/`. The
compile pipeline against fully-resolved deps lands with Phase 6b.

## Running

```
./run.sh
```
