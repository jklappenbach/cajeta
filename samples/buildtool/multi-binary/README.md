# `multi-binary` — single package, multiple executables

A single cajeta project producing three independent binaries
from one source tree: a server, a migration runner, and an
on-call diagnostic tool. Pattern matches Cargo's `[[bin]]`
sections, mapped to cajeta via `settings.build.binaries`.

## Layout

```
multi-binary/
├── cajeta.json                  # manifest with settings.build.binaries
├── run.sh
└── src/main/cajeta/com/example/multi/
    ├── server/Main.cajeta        # entry: com.example.multi.server.Main::main
    ├── migrate/Main.cajeta       # entry: com.example.multi.migrate.Main::main
    └── diag/Main.cajeta          # entry: com.example.multi.diag.Main::main
```

## What this sample demonstrates

- `settings.build.binaries` — named-binary registry with one
  entry per buildable executable. Each carries an
  `entry-method` and a `description` (the description shows up
  in `cajeta tasks` / `cajeta info`).
- Tasks reference binaries by name:
  `{ "action": "build", "binary": "server" }`.
- One task per binary plus a `build-all` that builds all three
  in parallel via a `parallel` group.

## What today's run.sh exercises

- `cajeta tasks` — lists `build` / `build-migrate` /
  `build-diag` / `build-all`.
- `cajeta task build --show` — renders the server build; the
  resolved entry method appears in the action output.
- `cajeta task build-all --show` — shows the parallel-group
  structure with three children.

Actually invoking `cajeta build` (which calls the build action)
forks the compiler and tries to compile the source under
`src/main/cajeta/`. The compile pipeline against fully-resolved
deps lands in Phase 6b.

## Running

```
./run.sh
```
