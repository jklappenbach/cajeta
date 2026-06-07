# `workspace` — parent + members buildtool sample

A monorepo with shared libraries (`shared/core`, `shared/util`)
and apps (`apps/api`, `apps/cli`) that depend on them. Mirrors
the workspace layout from BuildTool.md "Workspaces".

## Layout

```
workspace/
├── cajeta.json                # WORKSPACE manifest — has top-level "workspace" block
├── run.sh
├── shared/
│   ├── core/
│   │   ├── cajeta.json         # member manifest (no "workspace" block)
│   │   └── src/main/cajeta/com/example/core/
│   └── util/
│       ├── cajeta.json
│       └── src/main/cajeta/com/example/util/
└── apps/
    ├── api/
    │   ├── cajeta.json         # depends on ../../shared/core + ../../shared/util via path
    │   └── src/main/cajeta/com/example/api/
    └── cli/
        ├── cajeta.json
        └── src/main/cajeta/com/example/cli/
```

## What this sample demonstrates

- The `workspace` top-level manifest block (root only).
- `workspace.members` listing — member paths are free-form.
- `workspace.shared-dependencies` — version constraints
  inherited by members that opt in.
- Inter-member dependencies via `{ "path": "..." }` references
  (no publish ceremony for the inner loop).
- Members each declare their own `capabilities` (least
  privilege — apps need network, libs don't).
- Cross-member task references in the workspace's tasks
  (`{ "run-task": "shared/core:build" }`).

## What today's run.sh exercises

Workspace member resolution by path (`{ "path": "../../shared/core" }`)
and cross-member task invocation lands with Phase 6c. Until then
the `run.sh` here focuses on inspection:

- `cajeta tasks` at the workspace root.
- `cajeta task <name> --show` rendering both workspace tasks
  AND member tasks (when invoked from the member's directory).
- `cajeta info --properties` showing per-manifest property
  resolution.

## Running

```
./run.sh
```
